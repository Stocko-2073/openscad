#include "UXTest.h"

#include <QString>
#include <vector>

#include "geometry/linalg.h"
#include "gui/QGLView.h"
#include "platform/PlatformUtils.h"

void UXTest::setWindow(MainWindow *window_)
{
  window = window_;
}

std::vector<int> UXTest::pick(const Vector3d& from, const Vector3d& to)
{
  QGLView::PickResult picked;
  picked.rayOrigin = from;
  picked.rayDirection = to - from;
  picked.depthT = 0.5;  // the view drew something there; the picker finds where the ray meets it
  return window->pickPrimitives(picked);
}

void UXTest::restoreWindowInitialState()
{
  QString filename =
    QString::fromStdString(PlatformUtils::resourceBasePath()) + "/tests/basic-ux/default.scad";
  window->tabManager->open(filename);

  while (window->tabCount > 1) {
    window->tabManager->closeCurrentTab();
  }

  window->designActionAutoReload->setChecked(true);  // Enable auto-reload  & render
}
