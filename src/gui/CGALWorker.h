#pragma once

#include <QObject>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include "core/ModifierOverlays.h"
#include "core/PickAttribution.h"
#include "utils/Hash128.h"

class Geometry;
class Tree;
namespace interference {
struct Report;
}

// What a render hands the 3D view.
struct RenderResult {
  std::shared_ptr<const Geometry> geometry;  // null if there is none or the render failed
  std::optional<Hash128> digest;             // the root's (core/NodeDigest.h), unless it failed
  std::vector<overlay::Mesh> overlays;
  // What the picker names whole, for it to hold while the result is shown; null if the render
  // failed before it.
  std::shared_ptr<const pick::WholeGeometry> wholeGeometry;
  // Evaluating the geometry and the overlays, and holding what the picker names whole.
  std::chrono::steady_clock::duration geometryTime{};
  std::shared_ptr<const interference::Report> interference;  // when the check ran
  std::chrono::steady_clock::duration interferenceTime{};
};

class CGALWorker : public QObject
{
  Q_OBJECT;

public:
  CGALWorker();
  ~CGALWorker() override;

public slots:
  // Renders on the worker thread; with `checkInterference`, also checks the top-level parts for
  // overlaps and draws them. `wholeGeometry` is what the last render's result held for the picker.
  void start(const Tree& tree, bool checkInterference,
             std::shared_ptr<const pick::WholeGeometry> wholeGeometry);

protected slots:
  void work();

signals:
  void done(std::shared_ptr<const RenderResult>);

protected:
  class QThread *thread;
  const class Tree *tree;
  bool checkInterference{false};
  std::shared_ptr<const pick::WholeGeometry> wholeGeometry;
};
