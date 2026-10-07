#pragma once

#include <QObject>
#include <vector>

#include "geometry/linalg.h"
#include "gui/MainWindow.h"

class UXTest : public QObject
{
  Q_OBJECT;

public:
  void setWindow(MainWindow *window);

protected:
  void restoreWindowInitialState();
  // The node indices a right-click names where the ray from `from` to `to` meets the result, best
  // first.
  std::vector<int> pick(const Vector3d& from, const Vector3d& to);

  MainWindow *window;
};
