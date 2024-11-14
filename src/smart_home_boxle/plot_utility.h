// Copyright (c) 2024 Ralph Lange
// All rights reserved.
//
// This source code is licensed under the BSD 3-Clause license found in the
// LICENSE file in the root directory of this source tree.

#pragma once


#include <functional>
#include <vector>


struct PlotTick {
  double value;
  String label;
};


struct PlotPoint {
  double x;
  double y;
};


class PlotUtility {
 public:
  PlotUtility(int posX, int posY, int width, int height, double minX, double maxX, double minY, double maxY)
  : posX(posX), posY(posY), width(width), height(height), minX(minX), maxX(maxX), minY(minY), maxY(maxY)
  {
    assert(width > 0);
    assert(height > 0);
    assert(minX < maxX);
    assert(minY < maxY);
  }

  void setXTicks(const std::vector<PlotTick>& ticks) {
    for (const auto& tick : ticks) {
      assert(minX <= tick.value && tick.value <= maxX);
    }
    xTicks.clear();
    xTicks = ticks;
  }

  void setYTicks(const std::vector<PlotTick>& ticks) {
    for (const auto& tick : ticks) {
      assert(minY <= tick.value && tick.value <= maxY);
    }
    yTicks.clear();
    yTicks = ticks;
  }

  void drawXAxis(std::function<void(int,int,int,int)> drawLineFunc) {
    int x0 = posX;
    int y0 = posY + height - 1;
    int x1 = posX + width - 1;
    int y1 = posY + height - 1;
    drawLineFunc(x0, y0, x1, y1);
  }

  void drawYAxis(std::function<void(int,int,int,int)> drawLineFunc) {
    int x0 = posX;
    int y0 = posY + height - 1;
    int x1 = posX;
    int y1 = posY;
    drawLineFunc(x0, y0, x1, y1);
  }

  int getXPixelForXValue(double x) {
    return static_cast<int>(posX + (width - 1) * (x - minX) / (maxX - minX) + 0.5);
  }

  int getYPixelForYValue(double y) {
    return static_cast<int>(posY + (height - 1) * (maxY - y) / (maxY - minY) + 0.5);
  }

  void drawXTicks(std::function<void(int,int,double,String)> drawTickFunc) {
    for (const PlotTick& tick : xTicks) {
      int x = getXPixelForXValue(tick.value);
      int y = posY + height - 1;
      double relativePosition = (tick.value - minX) / (maxX - minX);
      drawTickFunc(x, y, relativePosition, tick.label);
    }
  }

  void drawYTicks(std::function<void(int,int,double,String)> drawTickFunc) {
    for (const PlotTick& tick : yTicks) {
      int x = posX;
      int y = getYPixelForYValue(tick.value);
      double relativePosition = (tick.value - minY) / (maxY - minY);
      drawTickFunc(x, y, relativePosition, tick.label);
    }
  }

  void drawPoints(const std::vector<PlotPoint>& points, std::function<void(int,int,PlotPoint)> drawPointFunc) {
    for (const PlotPoint& point : points) {
      int x = getXPixelForXValue(point.x);
      int y = getYPixelForYValue(point.y);
      drawPointFunc(x, y, point);
    }
  }

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
