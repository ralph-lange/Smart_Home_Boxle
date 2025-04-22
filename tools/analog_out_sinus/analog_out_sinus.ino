// Copyright (c) 2024-2025 Ralph Lange
// All rights reserved.
//
// This source code is licensed under the BSD 3-Clause license found in the
// LICENSE file in the root directory of this source tree.


const int AMMETER_PIN = 25;
const float SINUS_FREQUENCY = 0.25f;


void setup() {
  Serial.begin(115200);
  pinMode(AMMETER_PIN, OUTPUT);
}


void loop() {
  float uptimeInSeconds = millis() / 1000.0f;
  int i = 128 + static_cast<int>(127.0f * sin(SINUS_FREQUENCY * 2.0f * PI * uptimeInSeconds));
  Serial.println(i);
  analogWrite(AMMETER_PIN, i);  // i=0 to 255 gives output range from 0.0 to 3.3 V
  delay(25);
}
