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

struct RenderResult {
  std::shared_ptr<const Geometry> geometry;  // null if there is none or the render failed
  std::optional<Hash128> digest;             // the root's (core/NodeDigest.h), unless it failed
  std::vector<overlay::Mesh> overlays;
  // Held while the result is shown; null if the render failed before making it.
  std::shared_ptr<const pick::WholeGeometry> wholeGeometry;
  // Includes making the overlays and wholeGeometry.
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
  // `wholeGeometry` is what the last render's result held, for this render to reuse.
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
