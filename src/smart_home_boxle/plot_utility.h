// Copyright (c) 2024 Ralph Lange
// All rights reserved.
//
// This source code is licensed under the BSD 3-Clause license found in the
// LICENSE file in the root directory of this source tree.

#pragma once


#include <functional>
#include <vector>


/// Represents a tick on an axis with value and label.
struct PlotTick {
  double value;
  String label;
};


/// Represents a point in the plot area.
struct PlotPoint {
  double x;
  double y;
};


/// Generic class to render a 2D plot with linear axes. The class is generic
/// in the sense that the actual drawing functions are passed by function
/// pointers or lambda expressions, i.e., it may be used with any type of
/// 2D screen.
class PlotUtility {
 public:
  /// Ctor expecting position on the screen (posX, posY, width, height) and
  /// ranges of the x and y axes.
  PlotUtility(int posX, int posY, int width, int height, double minX, double maxX, double minY, double maxY)
  : posX(posX), posY(posY), width(width), height(height), minX(minX), maxX(maxX), minY(minY), maxY(maxY)
  {
    assert(width > 0);
    assert(height > 0);
    assert(minX < maxX);
    assert(minY < maxY);
  }


  /// Sets the ticks (string label at given value) on the x axis.
  void setXTicks(const std::vector<PlotTick>& ticks) {
    for (const auto& tick : ticks) {
      assert(minX <= tick.value && tick.value <= maxX);
    }
    xTicks.clear();
    xTicks = ticks;
  }


  /// Sets the ticks (string label at given value) on the y axis.
  void setYTicks(const std::vector<PlotTick>& ticks) {
    for (const auto& tick : ticks) {
      assert(minY <= tick.value && tick.value <= maxY);
    }
    yTicks.clear();
    yTicks = ticks;
  }


  /// Draws the x axis using the given function to draw a line.
  void drawXAxis(std::function<void(int,int,int,int)> drawLineFunc) {
    int x0 = posX;
    int y0 = posY + height - 1;
    int x1 = posX + width - 1;
    int y1 = posY + height - 1;
    drawLineFunc(x0, y0, x1, y1);
  }


  /// Draws the y axis using the given function to draw a line.
  void drawYAxis(std::function<void(int,int,int,int)> drawLineFunc) {
    int x0 = posX;
    int y0 = posY + height - 1;
    int x1 = posX;
    int y1 = posY;
    drawLineFunc(x0, y0, x1, y1);
  }


  /// Computes the x pixel value for the given x value in the plot range.
  int getXPixelForXValue(double x) {
    return static_cast<int>(posX + (width - 1) * (x - minX) / (maxX - minX) + 0.5);
  }


  /// Computes the y pixel value for the given y value in the plot range.
  int getYPixelForYValue(double y) {
    return static_cast<int>(posY + (height - 1) * (maxY - y) / (maxY - minY) + 0.5);
  }


  /// Draws the x axis ticks using the given function to draw tick label at
  /// x0,y0 and the relative value (between 0 and 1) along the x axis. The
  /// relative value can for example be used to adjust between left, center,
  /// and right alignment.
  void drawXTicks(std::function<void(int,int,double,String)> drawTickFunc) {
    for (const PlotTick& tick : xTicks) {
      int x = getXPixelForXValue(tick.value);
      int y = posY + height - 1;
      double relativePosition = (tick.value - minX) / (maxX - minX);
      drawTickFunc(x, y, relativePosition, tick.label);
    }
  }


  /// Draws the y axis ticks using the given function to draw tick label at
  /// x0,y0 and the relative value (between 0 and 1) along the y axis. The
  /// relative value can for example be used to adjust between top, middle,
  /// and bottom alignment.
  void drawYTicks(std::function<void(int,int,double,String)> drawTickFunc) {
    for (const PlotTick& tick : yTicks) {
      int x = posX;
      int y = getYPixelForYValue(tick.value);
      double relativePosition = (tick.value - minY) / (maxY - minY);
      drawTickFunc(x, y, relativePosition, tick.label);
    }
  }


  /// Draws the given points in the plot area using the given function
  /// to draw a point at x0,y0.
  void drawPoints(const std::vector<PlotPoint>& points, std::function<void(int,int,PlotPoint)> drawPointFunc) {
    for (const PlotPoint& point : points) {
      int x = getXPixelForXValue(point.x);
      int y = getYPixelForYValue(point.y);
      drawPointFunc(x, y, point);
    }
  }


  /// Draws lines between consecutive points of the given vector of points
  /// using the given drawing functions.
  void drawLinesBetweenPoints(const std::vector<PlotPoint>& points, std::function<void(int,int,int,int,PlotPoint,PlotPoint)> drawLineFunc) {
    PlotPoint prevPoint;
    int prevX = 0;
    int prevY = 0;
    bool isFirst = true;
    for (const PlotPoint& point : points) {
      int x = getXPixelForXValue(point.x);
      int y = getYPixelForYValue(point.y);
      if (isFirst) {
        isFirst = false;
      } else {
        drawLineFunc(prevX, prevY, x, y, prevPoint, point);
      }
      prevPoint = point;
      prevX = x;
      prevY = y;
    }
  }

 private:
  const int posX;
  const int posY;
  const int width;
  const int height;
  const double minX;
  const double maxX;
  const double minY;
  const double maxY;
  std::vector<PlotTick> xTicks;
  std::vector<PlotTick> yTicks;
};
