#pragma once

#include <QObject>
#include <memory>
#include <vector>

#include "core/ModifierOverlays.h"

class Geometry;
class Tree;

// What a render hands the 3D view.
struct RenderResult {
  std::shared_ptr<const Geometry> geometry;  // null if there is none or the render failed
  std::vector<overlay::Mesh> overlays;
};

class CGALWorker : public QObject
{
  Q_OBJECT;

public:
  CGALWorker();
  ~CGALWorker() override;

public slots:
  void start(const Tree& tree);

protected slots:
  void work();

signals:
  void done(std::shared_ptr<const RenderResult>);

protected:
  class QThread *thread;
  const class Tree *tree;
};
