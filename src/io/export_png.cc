#include <cassert>
#include <cstdio>
#include <memory>
#include <ostream>
#include <vector>

#include "core/ModifierOverlays.h"
#include "geometry/Geometry.h"
#include "geometry/linalg.h"
#include "glview/Camera.h"
#include "glview/OffscreenView.h"
#include "glview/RenderSettings.h"
#include "glview/Renderer.h"
#include "io/export.h"
#include "utils/printutils.h"

#ifndef NULLGL
#include "glview/PolySetRenderer.h"
#include "glview/VBORenderer.h"
#if not defined(USE_POLYSET_FOR_CGAL)
#include "glview/cgal/CGALRenderer.h"
#endif

namespace {

void setupCamera(Camera& cam, const BoundingBox& bbox)
{
  if (cam.viewall) cam.viewAll(bbox);
}

}  // namespace

bool export_png(const std::shared_ptr<const Geometry>& root_geom,
                const std::vector<overlay::Mesh>& overlays, const ViewOptions& options, Camera& camera,
                std::ostream& output)
{
  assert(root_geom != nullptr);
  PRINTD("export_png geom");
  std::unique_ptr<OffscreenView> glview;
  try {
    glview = std::make_unique<OffscreenView>(camera.pixel_width, camera.pixel_height);
  } catch (const OffscreenViewException& ex) {
    fprintf(stderr, "Can't create OffscreenView: %s.\n", ex.what());
    return false;
  }
  std::shared_ptr<VBORenderer> geomRenderer;
#if defined(USE_POLYSET_FOR_CGAL)
  geomRenderer = std::make_shared<PolySetRenderer>(root_geom);
#else
  // Choose PolySetRenderer for PolySet and Polygon2d, and for Manifold since we
  // know that all geometries are convertible to PolySet.
  if (RenderSettings::inst()->backend3D == RenderBackend3D::ManifoldBackend ||
      std::dynamic_pointer_cast<const PolySet>(root_geom) ||
      std::dynamic_pointer_cast<const Polygon2d>(root_geom)) {
    geomRenderer = std::make_shared<PolySetRenderer>(root_geom);
  } else {
    geomRenderer = std::make_shared<CGALRenderer>(root_geom);
  }
#endif
  geomRenderer->setOverlays(overlays);
  const BoundingBox bbox = geomRenderer->getBoundingBox();
  setupCamera(camera, bbox);

  glview->setCamera(camera);
  glview->setRenderer(geomRenderer);
  glview->setColorScheme(RenderSettings::inst()->colorscheme);
  glview->setShowCrosshairs(options["crosshairs"]);
  glview->setShowAxes(options["axes"]);
  glview->setShowScaleProportional(options["scales"]);
  glview->setShowEdges(options["edges"]);
  glview->paintGL();
  glview->save(output);
  return true;
}

#else  // NULLGL

bool export_png(const std::shared_ptr<const Geometry>& root_geom,
                const std::vector<overlay::Mesh>& overlays, const ViewOptions& options, Camera& camera,
                std::ostream& output)
{
  return false;
}

#endif  // NULLGL
