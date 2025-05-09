// Copyright (c) 2024-2025 Ralph Lange
// All rights reserved.
//
// This source code is licensed under the BSD 3-Clause license found in the
// LICENSE file in the root directory of this source tree.


// TODOs:
// - Restore zoom function, connect it with push buttons, and preserve zoom between reboots.
// - Implement calibration function using the four push buttons.


#include <cstdint>

#include <vector>

#include <WiFi.h>  // Platform 'esp32' by Espressif (here V2.0.11)
#include <time.h>
#include <HTTPClient.h>

#include <ArduinoJson.h>  // Library 'ArduinoJson' by Benoit Blanchon (here V7.2.0)

#include <GxEPD2_3C.h>  // Library 'GxEPD2' by Jean-Marc Zingg (here V1.6.0).

#include <U8g2_for_Adafruit_GFX.h>  // Library 'U8g2_for_Adafruit_GFX' by oliver (here V1.8.0)
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include "plot_utility.h"
#include "secrets.h"  // Define WIFI_SSID, WIFI_PASSWORD, and THINGSPEAK_CHANNEL in this file.


TaskHandle_t longRunningFunctionsTask;

const int AMMETER_PIN = 26;

const int PUSH_BUTTON_A_PIN = 17;
const int PUSH_BUTTON_B_PIN = 16;
const int PUSH_BUTTON_C_PIN = 2;
const int PUSH_BUTTON_D_PIN = 15;

const int E_PAPER_CS = 14;
const int E_PAPER_DC = 25;
const int E_PAPER_RST = 33;
const int E_PAPER_BUSY = 12;

GxEPD2_3C<GxEPD2_583c_Z83, GxEPD2_583c_Z83::HEIGHT>* displayPtr;

#define NTP_SERVER "de.pool.ntp.org"

const int MAX_ZOOM = 6;
const std::array<int, MAX_ZOOM + 1> ZOOM_TO_RESOLUTION_MINUTES{ 10,   20,       30,       60,       60,       240,       720};
const std::array<int, MAX_ZOOM + 1> ZOOM_TO_RANGE_MINUTES{     720, 1440, 2 * 2440, 4 * 1440, 8 * 1440, 16 * 1440, 32 * 1440};


U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

SemaphoreHandle_t globalMutex = NULL;
int zoom = 3;
bool isNtpInitialized = false;


#define HAS_STEPPER_AS_AMMETER false
#if HAS_STEPPER_AS_AMMETER
  #include <ESP32Servo.h>  // Library 'ESP32Servo' V3.0.6 by Kevin Harrington, John K. Bennett.
  Servo ammeterServo;
#endif


/// Struct for a single data item from the photovoltaic system. 
struct PVSingleData {
  double age = 0.0;
  float pAC = 0.0f;
  float uAC = 0.0f;
  float frequency = 0.0f;
  float temperature = 0.0f;
  float efficiency = 0.0f;
  float totalYield = 0.0f;  
};

PVSingleData newestData; 


/// Waits up to the given number of milliseconds for the WiFi to connect.
void waitUntilWiFiConnectedOrTimeout(long timeout_ms) {
  assert(timeout_ms >= 5000);

  const int WAIT_STEP_TIME_MS = 100;
  long maxSteps = timeout_ms / WAIT_STEP_TIME_MS;
  for (long step = 0; step < maxSteps && WiFi.status() != WL_CONNECTED; ++step) {
    delay(WAIT_STEP_TIME_MS);
  }  
}


/// Tries to connect to WiFi the given number of times.
bool tryConnectWiFi(size_t attemps) {
  size_t index = 0;
  Serial.print(F("Connecting to WiFi ."));
  while(WiFi.status() != WL_CONNECTED && index < attemps) {
    index++;
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print(".");
    waitUntilWiFiConnectedOrTimeout(5000);
  } 
  Serial.println("");
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("Could not connect to WiFi."));
    return false;
  }

  Serial.println(F("Connected to WiFi successfully."));
  return true;
}


/// Sends the given HTTP request and returns the response - or an empty
/// string if the request failed.
String tryHTTPRequest(const String& url, size_t attempts) {
  Serial.print(F("Sending HTTP request to "));
  Serial.print(url);
  Serial.println(".");
  
  int response = -1;
  String content = "";
  size_t index = 0;
  do {
    index++;
    HTTPClient http;
    http.begin(url.c_str());
    response = http.GET();
    content = http.getString();
    http.end();
  } while(response != 200 && index < attempts);
  
  if (response != 200) {
    Serial.print(F("Problem with REST query: HTTP error code is "));
    Serial.println(response);
    return String("");
  }
  
  Serial.println(F("REST query successful."));
  return content;
}


/// Queries the newest/latest data from the ThingSpeak channel. The argument
/// currentTime is used to determine the relative age of the data.
void queryNewestData(tm& currentTime) {
  String url = "https://api.thingspeak.com/channels/" + String(THINGSPEAK_CHANNEL) + "/feeds.json?results=1";
  String content = tryHTTPRequest(url, 5);
  DynamicJsonDocument doc(10 * 1024);
  DeserializationError errorMsg = deserializeJson(doc, content.c_str());
  if (errorMsg) {
    Serial.print(F("JSON deserialization failed: "));
    Serial.println(errorMsg.f_str());
    return;
  }
  if (doc["feeds"].size() == 0) {
    Serial.println(F("Feed is empty!"));
    return;
  }

  tm timestamp;
  strptime(doc["feeds"][0]["created_at"], "%Y-%m-%dT%H:%M:%SZ", &timestamp);

  xSemaphoreTake(globalMutex, 10 * portTICK_PERIOD_MS);
  newestData.age = static_cast<double>(difftime(mktime(&currentTime), mktime(&timestamp)));
  newestData.pAC = doc["feeds"][0]["field3"];
  newestData.uAC = doc["feeds"][0]["field1"];
  newestData.frequency = doc["feeds"][0]["field2"];
  newestData.temperature = doc["feeds"][0]["field4"];
  newestData.efficiency = doc["feeds"][0]["field5"];
  newestData.totalYield = doc["feeds"][0]["field6"];
  xSemaphoreGive(globalMutex); 
}


/// Queries a timeseries/curve for the given field from the ThingSpeak channel.
/// The currentTime is used to determine the relative age of each data point.
/// The argument zoom determines the temporal resolution.
std::vector<PlotPoint> queryCurveGeneric(tm& currentTime, int zoom, int field) {
  String fieldAsString(field);
  String url = "https://api.thingspeak.com/channels/" + String(THINGSPEAK_CHANNEL) + "/fields/" + fieldAsString + ".json?median=" + String(ZOOM_TO_RESOLUTION_MINUTES[zoom]) + "&minutes=" + String(ZOOM_TO_RANGE_MINUTES[zoom]);
  String content = tryHTTPRequest(url, 5);
  DynamicJsonDocument doc(50 * 1024);
  DeserializationError errorMsg = deserializeJson(doc, content.c_str());
  if (errorMsg) {
    Serial.print(F("JSON deserialization failed: "));
    Serial.println(errorMsg.f_str());
    return std::vector<PlotPoint>();
  }
  
  std::vector<PlotPoint> result;
  result.reserve(200);
  for (size_t index = 0; index < doc["feeds"].size(); ++index) {
    double pac = doc["feeds"][index]["field" + fieldAsString];
    tm timestamp;
    strptime(doc["feeds"][index]["created_at"], "%Y-%m-%dT%H:%M:%SZ", &timestamp);
    double relativeTime = static_cast<double>(difftime(mktime(&timestamp), mktime(&currentTime)));
    result.push_back({relativeTime, pac});
  }
  
  return result;
}


/// Queries the P_AC timeseries/curve from the ThingSpeak channel. The
/// currentTime is used to determine the relative age of each data point.
/// The argument zoom determines the temporal resolution.
std::vector<PlotPoint> queryPACCurve(tm& currentTime, int zoom) {
  return queryCurveGeneric(currentTime, zoom, 3);
}


/// Queries the frequency timeseries/curve from the ThingSpeak channel. The
/// currentTime is used to determine the relative age of each data point.
/// The argument zoom determines the temporal resolution.
std::vector<PlotPoint> queryFrequencyCurve(tm& currentTime, int zoom) {
  return queryCurveGeneric(currentTime, zoom, 2);
}


/// Queries the U_AC timeseries/curve from the ThingSpeak channel. The
/// currentTime is used to determine the relative age of each data point.
/// The argument zoom determines the temporal resolution.
std::vector<PlotPoint> queryUACCurve(tm& currentTime, int zoom) {
  return queryCurveGeneric(currentTime, zoom, 1);
}


/// The main function (static schedule) for all long-running functions
/// such as querying ThingSpeak and updating the e-Ink display.
void longRunningFunctionsMain(void*) {
  WiFiClient client;
  WiFi.mode(WIFI_STA);

  long nextPlotRedrawMillis = 0;

  while(millis() < 3600 * 1000) {
    if (WiFi.status() != WL_CONNECTED) {
      tryConnectWiFi(5);      
    } else if (!isNtpInitialized) {
      Serial.println(F("Initializing NTP ..."));
      configTime(0, 0, NTP_SERVER);
      struct tm currentTime;
      if (getLocalTime(&currentTime, 10000)){
        Serial.println(F("Queried NTP server successfully."));
        isNtpInitialized = true;
      } else {
        Serial.println(F("Could not initialize NTP!"));
      }
    } else if (millis() > nextPlotRedrawMillis) {
      queryDataAndRedraw(zoom);
      nextPlotRedrawMillis = millis() + 180 * 1000;  // Normally do not redraw faster than 3 minutes.
    }
    delay(10);
  }

  ESP.restart();
}


/// The main function (static schedule) for all short-running functions
/// such as determining the state of the push buttons and updating the
/// analogy display.
void shortRunningFunctionsMain() {
  unsigned long nextRegularAmmeterUpdateMillis = millis();
  int lastAnalogDisplayValue = -1;
  while(true) {
    int analogDisplayValue = 0;

    if (digitalRead(PUSH_BUTTON_A_PIN) == LOW) {
      Serial.println("Push button A (very left) is pressed.");
    }
    if (digitalRead(PUSH_BUTTON_B_PIN) == LOW) {
      Serial.println("Push button B (middle left) is pressed.");
    }
    if (digitalRead(PUSH_BUTTON_C_PIN) == LOW) {
      Serial.println("Push button C (middle right) is pressed.");
    }
    if (digitalRead(PUSH_BUTTON_D_PIN) == LOW) {
      Serial.println("Push button D (very right) is pressed.");
    }

    if (digitalRead(PUSH_BUTTON_A_PIN) == LOW) {
      // Sinus wave with 0.25 Hz.
      analogDisplayValue = 128 + static_cast<int>(127.0f * sin(0.25 * 2.0f * PI * millis() / 1000.0f));
    } else if (digitalRead(PUSH_BUTTON_B_PIN) == LOW) {
      // Show zero power value.
      float pAC = 0.0f;
      analogDisplayValue = static_cast<int>(255.0f * pAC / 1000.0f);
    } else if (digitalRead(PUSH_BUTTON_C_PIN) == LOW) {
      // Show power value of 300 W.
      float pAC = 300.0f;
      analogDisplayValue = static_cast<int>(255.0f * pAC / 1000.0f);
    } else if (digitalRead(PUSH_BUTTON_D_PIN) == LOW) {
      // Show power value of 600 W.
      float pAC = 600.0f;
      analogDisplayValue = static_cast<int>(255.0f * pAC / 1000.0f);
    } else {
      xSemaphoreTake(globalMutex, 10 * portTICK_PERIOD_MS);
      if (newestData.totalYield > 0 && newestData.age < 900) {
        analogDisplayValue = static_cast<int>(255.0f * newestData.pAC / 1000.0f);
      }
      xSemaphoreGive(globalMutex);
    }
    analogDisplayValue = std::min(analogDisplayValue, 255);
    analogDisplayValue = std::max(analogDisplayValue, 0);

    if (analogDisplayValue != lastAnalogDisplayValue || millis() >= nextRegularAmmeterUpdateMillis) {
#ifdef STEPPER_AS_AMMETER
      ammeterServo.attach(AMMETER_PIN);
      int angle = static_cast<int>(90.0f * analogDisplayValue / 255.0f);
      ammeterServo.write(angle);
      delay(200);
      ammeterServo.detach();
#else
      analogWrite(AMMETER_PIN, analogDisplayValue);
#endif
      // Avoid continuous update of ammeter, which may cause strange sounds
      // if ammeter is implemented by stepper motor.
      nextRegularAmmeterUpdateMillis = millis() + 5000;
      lastAnalogDisplayValue = analogDisplayValue;
    }
    delay(100);
  }
}


/// The start-up function called by the ESP32 platform.
void setup() {
  Serial.begin(115200);

  displayPtr = new GxEPD2_3C<GxEPD2_583c_Z83, GxEPD2_583c_Z83::HEIGHT>(GxEPD2_583c_Z83(E_PAPER_CS, E_PAPER_DC, E_PAPER_RST, E_PAPER_BUSY));

  pinMode(AMMETER_PIN, OUTPUT);

  pinMode(PUSH_BUTTON_A_PIN, INPUT_PULLUP);
  pinMode(PUSH_BUTTON_B_PIN, INPUT_PULLUP);
  pinMode(PUSH_BUTTON_C_PIN, INPUT_PULLUP);
  pinMode(PUSH_BUTTON_D_PIN, INPUT_PULLUP);

  Serial.print("Initializing display ...");
  displayPtr->init();
  u8g2Fonts.begin(*displayPtr);
  Serial.println(" done.");

  globalMutex = xSemaphoreCreateMutex();
}


/// The main function called by the ESP32 platform. It is splitted into two
/// threads for long-running and short-running functions.
void loop() {
  xTaskCreatePinnedToCore(longRunningFunctionsMain, "longRunningFunctionsTask", 25000, NULL, 0, &longRunningFunctionsTask, 0);
  shortRunningFunctionsMain();
}


/// Converts the given relative time (negative number of seconds) into a
/// string representation in hours or even days for the x-axes of the plots. 
String relativeHoursOrDayLabelFromSeconds(int seconds) {
  assert(seconds <= 0);

  if (seconds == 0) {
    return String("now");
  } else if (seconds == -3 * 3600) {
    return String("-3h");
  } else if (seconds == -6 * 3600) {
    return String("-6h");
  } else if (seconds == -12 * 3600) {
    return String("-12h");
  } else if (seconds <= -24 * 3600) {
    return "-" + String(seconds / (-24 * 3600)) + "d";
  }

  assert(false);  
  return String("?");
}


/// Queries all data from the ThingSpeak channel and updates the whole
/// e-Ink display accordingly.
void queryDataAndRedraw(int zoom) {
  struct tm currentTime;
  if (!getLocalTime(&currentTime, 10000)) {
    Serial.println(F("Could not get current time!"));
    return;
  }

  // secondsDiffModuloOneMinute = currentTime.tm_sec - (millis() / 1000);
  queryNewestData(currentTime);
  std::vector<PlotPoint> pacCurve = queryPACCurve(currentTime, zoom);
  std::vector<PlotPoint> uacCurve = queryUACCurve(currentTime, zoom);
  std::vector<PlotPoint> frequencyCurve = queryFrequencyCurve(currentTime, zoom);
    
  displayPtr->setFullWindow();
  displayPtr->setRotation(2);
  displayPtr->setTextColor(GxEPD_BLACK);
  u8g2Fonts.setForegroundColor(GxEPD_BLACK);
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
  displayPtr->firstPage();

  PlotUtility pacPlot(40, 235, 360 - 15 - 40, 208, - ZOOM_TO_RANGE_MINUTES[zoom] * 60, 0, 0, 650);
  pacPlot.setXTicks({{- ZOOM_TO_RANGE_MINUTES[zoom] * 60, relativeHoursOrDayLabelFromSeconds(- ZOOM_TO_RANGE_MINUTES[zoom] * 60)},
                  {- ZOOM_TO_RANGE_MINUTES[zoom] * 30, relativeHoursOrDayLabelFromSeconds(- ZOOM_TO_RANGE_MINUTES[zoom] * 30)},
                  {0, relativeHoursOrDayLabelFromSeconds(0)}});
  pacPlot.setYTicks({{0, "0"}, {200, "200"}, {400, "400"}, {600, "600"}});
  
  PlotUtility uacPlot(360 + 35, 235, 635 - (360 + 35), 86, - ZOOM_TO_RANGE_MINUTES[zoom] * 60, 0, 220, 240);
  uacPlot.setXTicks({{- ZOOM_TO_RANGE_MINUTES[zoom] * 60, relativeHoursOrDayLabelFromSeconds(- ZOOM_TO_RANGE_MINUTES[zoom] * 60)},
                  {- ZOOM_TO_RANGE_MINUTES[zoom] * 30, relativeHoursOrDayLabelFromSeconds(- ZOOM_TO_RANGE_MINUTES[zoom] * 30)},
                  {0, relativeHoursOrDayLabelFromSeconds(0)}});
  uacPlot.setYTicks({{220, "220"}, {230, "230"}, {240, "240"}});
  
  PlotUtility frequencyPlot(360 + 35, 235 + 208 - 86, 635 - (360 + 35), 86, - ZOOM_TO_RANGE_MINUTES[zoom] * 60, 0, 49.9, 50.1);
  frequencyPlot.setXTicks({{- ZOOM_TO_RANGE_MINUTES[zoom] * 60, relativeHoursOrDayLabelFromSeconds(- ZOOM_TO_RANGE_MINUTES[zoom] * 60)},
                  {- ZOOM_TO_RANGE_MINUTES[zoom] * 30, relativeHoursOrDayLabelFromSeconds(- ZOOM_TO_RANGE_MINUTES[zoom] * 30)},
                  {0, relativeHoursOrDayLabelFromSeconds(0)}});
  frequencyPlot.setYTicks({{49.9, "49.9"}, {50.0, "50.0"}, {50.1, "50.1"}});
  
  Serial.println(F("Starting redrawing of e-paper display."));
  do {
    displayPtr->fillScreen(GxEPD_WHITE);

    // Current P_AC.
    if (newestData.totalYield > 0 && newestData.age < 900) {
      u8g2Fonts.setFont(u8g2_font_logisoso92_tn);
      String currentPAC = String(newestData.pAC, 0);
      int16_t textWidth = u8g2Fonts.getUTF8Width(currentPAC.c_str());
      u8g2Fonts.setCursor(200 - 20 - textWidth, 156);
      u8g2Fonts.print(currentPAC);
      displayPtr->setFont(&FreeSansBold24pt7b);
      displayPtr->setCursor(200, 156);
      displayPtr->print("Watt");
    } else {
      displayPtr->setFont(&FreeSansBold24pt7b);
      displayPtr->setCursor(0, 156);
      displayPtr->print("Kein Ertrag!");
    }

    // Current time.
    displayPtr->setFont(&FreeSans12pt7b);
    char stringBuffer[50];
    strftime(stringBuffer, sizeof(stringBuffer), "%H:%M", &currentTime);
    String timeString = stringBuffer + String(" (UTC)");
    displayPtr->setCursor(635 - display_getTextWidth(timeString), 21);
    displayPtr->print(timeString);

    // Other current values
    displayPtr->setFont(&FreeSans12pt7b);
    String currentUAC = "Netzspannung: -";
    String currentFrequency = "Frequenz: -";
    String currentTemperature = "Temperatur: -";
    String currentEfficiency = "Effizienz: -";
    String totalYield = "Gesamtertrag: -";
    if (newestData.totalYield > 0) {
      if (newestData.age < 900) {
        currentUAC = "Netzspannung: " + String(newestData.uAC, 1) + " V";
        currentFrequency = "Frequenz: " + String(newestData.frequency, 2) + " Hz";
        currentTemperature = "Temperatur: " + String(newestData.temperature, 1) + " C";
        currentEfficiency = "Effizienz: " + String(newestData.efficiency, 1) + " %";
      }
      totalYield = "Gesamtertrag: " + String(newestData.totalYield, 1) + " kWh";
    }
    displayPtr->setCursor(360, 60);
    displayPtr->print(currentUAC);
    displayPtr->setCursor(360, 92);
    displayPtr->print(currentFrequency);
    displayPtr->setCursor(360, 124);
    displayPtr->print(currentTemperature);
    displayPtr->setCursor(360, 156);
    displayPtr->print(currentEfficiency);
    displayPtr->setCursor(360, 188);
    displayPtr->print(totalYield);

    {
      int y = pacPlot.getYPixelForYValue(600.0);
      displayPtr->drawLine(40, y, 360 - 15, y, GxEPD_RED);
    }

    pacPlot.drawXAxis([displayPtr](int x0, int y0, int x1, int y1) {
      displayPtr->drawLine(x0, y0, x1, y1, GxEPD_BLACK);
    });

    pacPlot.drawYAxis([displayPtr](int x0, int y0, int x1, int y1) {
      displayPtr->drawLine(x0, y0, x1, y1, GxEPD_BLACK);
    });

    pacPlot.drawXTicks([displayPtr](int x, int y, double relativePosition, String label) {
      displayPtr->drawLine(x, y, x, y + 2, GxEPD_BLACK);
      displayPtr->setFont(&FreeSans9pt7b);
      displayPtr->setCursor(x - static_cast<int>(relativePosition * display_getTextWidth(label)), y + 18);
      displayPtr->print(label.c_str());
    });

    pacPlot.drawYTicks([displayPtr](int x, int y, double relativePosition, String label) {
      displayPtr->drawLine(x - 2, y, x, y, GxEPD_BLACK);
      displayPtr->setFont(&FreeSans9pt7b);
      displayPtr->setCursor(40 - 4 - display_getTextWidth(label), y + 5);
      displayPtr->print(label.c_str());
    });

    pacPlot.drawPoints(pacCurve, [displayPtr](int x, int y, PlotPoint point) {
      displayPtr->fillRect(x - 1, y - 1, 3, 3, GxEPD_BLACK);
    });

    pacPlot.drawLinesBetweenPoints(pacCurve, [displayPtr](int x0, int y0, int x1, int y1, PlotPoint point0, PlotPoint point1) {
      displayPtr->drawLine(x0, y0, x1, y1, GxEPD_BLACK);
    });

    {
      int y = uacPlot.getYPixelForYValue(230.0);
      displayPtr->drawLine(360 + 35, y, 635, y, GxEPD_RED);
    }

    uacPlot.drawXAxis([displayPtr](int x0, int y0, int x1, int y1) {
      displayPtr->drawLine(x0, y0, x1, y1, GxEPD_BLACK);
    });

    uacPlot.drawYAxis([displayPtr](int x0, int y0, int x1, int y1) {
      displayPtr->drawLine(x0, y0, x1, y1, GxEPD_BLACK);
    });

    uacPlot.drawXTicks([displayPtr](int x, int y, double relativePosition, String label) {
      displayPtr->drawLine(x, y, x, y + 2, GxEPD_BLACK);
      displayPtr->setFont(&FreeSans9pt7b);
      displayPtr->setCursor(x - static_cast<int>(relativePosition * display_getTextWidth(label)), y + 18);
      displayPtr->print(label.c_str());
    });

    uacPlot.drawYTicks([displayPtr](int x, int y, double relativePosition, String label) {
      displayPtr->drawLine(x - 2, y, x, y, GxEPD_BLACK);
      displayPtr->setFont(&FreeSans9pt7b);
      displayPtr->setCursor(360 + 35 - 4 - display_getTextWidth(label), y + 5);
      displayPtr->print(label.c_str());
    });

    uacPlot.drawPoints(uacCurve, [displayPtr](int x, int y, PlotPoint point) {
      if (point.y != 0.0) {
        displayPtr->fillRect(x - 1, y - 1, 3, 3, GxEPD_BLACK);
      }
    });

    uacPlot.drawLinesBetweenPoints(uacCurve, [displayPtr](int x0, int y0, int x1, int y1, PlotPoint point0, PlotPoint point1) {
      if (point0.y != 0.0 && point1.y != 0.0) {
        displayPtr->drawLine(x0, y0, x1, y1, GxEPD_BLACK);
      }
    });

    {
      int y = frequencyPlot.getYPixelForYValue(50.0);
      displayPtr->drawLine(360 + 35, y, 635, y, GxEPD_RED);      
    }

    frequencyPlot.drawXAxis([displayPtr](int x0, int y0, int x1, int y1) {
      displayPtr->drawLine(x0, y0, x1, y1, GxEPD_BLACK);
    });

    frequencyPlot.drawYAxis([displayPtr](int x0, int y0, int x1, int y1) {
      displayPtr->drawLine(x0, y0, x1, y1, GxEPD_BLACK);
    });

    frequencyPlot.drawXTicks([displayPtr](int x, int y, double relativePosition, String label) {
      displayPtr->drawLine(x, y, x, y + 2, GxEPD_BLACK);
      displayPtr->setFont(&FreeSans9pt7b);
      displayPtr->setCursor(x - static_cast<int>(relativePosition * display_getTextWidth(label)), y + 18);
      displayPtr->print(label.c_str());
    });

    frequencyPlot.drawYTicks([displayPtr](int x, int y, double relativePosition, String label) {
      displayPtr->drawLine(x - 2, y, x, y, GxEPD_BLACK);
      displayPtr->setFont(&FreeSans9pt7b);
      displayPtr->setCursor(360 + 35 - 4 - display_getTextWidth(label), y + 5);
      displayPtr->print(label.c_str());
    });

    frequencyPlot.drawPoints(frequencyCurve, [displayPtr](int x, int y, PlotPoint point) {
      if (point.y != 0.0) {
        displayPtr->fillRect(x - 1, y - 1, 3, 3, GxEPD_BLACK);
      }
    });

    frequencyPlot.drawLinesBetweenPoints(frequencyCurve, [displayPtr](int x0, int y0, int x1, int y1, PlotPoint point0, PlotPoint point1) {
      if (point0.y != 0.0 && point1.y != 0.0) {
        displayPtr->drawLine(x0, y0, x1, y1, GxEPD_BLACK);
      }
    });
  } while (displayPtr->nextPage());
  Serial.println(F("Redrawing of e-paper display completed."));
  
  Serial.print(F("Powering off the display ..."));
  displayPtr->powerOff();
  Serial.println(F(" done."));
}


/// Returns the width of the given text in pixels on the e-Ink display.
uint16_t display_getTextWidth(String text) {
  int16_t x, y;
  uint16_t width, height;
  displayPtr->getTextBounds(text, 0, 0, &x, &y, &width, &height);
  return width;
}
