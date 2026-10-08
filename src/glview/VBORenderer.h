#pragma once

#include <array>
#include <cmath>
#include <utility>
#include <memory>
#include <cstddef>
#include <vector>
#include "core/ModifierOverlays.h"
#include "glview/Renderer.h"
#include "glview/ShaderUtils.h"
#include "geometry/linalg.h"
#include "geometry/Polygon2d.h"
#include "glview/system-gl.h"
#include "glview/VBOBuilder.h"

namespace VBOUtils {

void shader_attribs_enable(const ShaderUtils::ShaderInfo& shaderinfo);
void shader_attribs_disable(const ShaderUtils::ShaderInfo& shaderinfo);

}  // namespace VBOUtils

class VBOShaderVertexState : public VertexState
{
public:
  VBOShaderVertexState(size_t draw_offset, size_t element_offset, GLuint vertices_vbo,
                       GLuint elements_vbo)
    : VertexState(0, 0, 0, draw_offset, element_offset, vertices_vbo, elements_vbo)
  {
  }
};

class VBORenderer : public Renderer
{
public:
  VBORenderer();
  virtual size_t calcNumEdgeVertices(const PolySet& polyset) const;
  virtual size_t calcNumEdgeVertices(const Polygon2d& polygon) const;

  void add_shader_pointers(
    VBOBuilder& vbo_builder,
    const ShaderUtils::ShaderInfo
      *shaderinfo);  // This could stay protected, were it not for VertexStateManager

  // The # and % subtrees and interference overlaps, drawn translucent; only the overlaps aren't
  // picked.
  void setOverlays(std::vector<overlay::Mesh> overlays);

protected:
  void add_shader_data(VBOBuilder& vbo_builder);
  void shader_attribs_enable(const ShaderUtils::ShaderInfo&) const;
  void shader_attribs_disable(const ShaderUtils::ShaderInfo&) const;

  // For prepare(), draw() and getBoundingBox() to call after handling the geometry: first the
  // translucent faces of polysets and the # and % overlays, then the interference overlaps.
  // prepareTranslucent() sorts them for the modelview it finds, so it runs after the camera's set.
  void prepareTranslucent(const std::vector<std::shared_ptr<const PolySet>>& polysets,
                          const Color4f& default_color, const ShaderUtils::ShaderInfo *shaderinfo,
                          double crease_degrees, bool edges);
  void drawTranslucent(bool showedges, const ShaderUtils::ShaderInfo *shaderinfo) const;
  void dropTranslucent() { translucent_.reset(); }
  void prepareOverlays();
  void drawOverlays(const ShaderUtils::ShaderInfo *shaderinfo) const;
  [[nodiscard]] BoundingBox overlayBoundingBox() const;

private:
  // Drawn after everything opaque, back to front and without writing depth, so nothing behind a
  // translucent face is hidden. Triangle t is vertices 3t to 3t + 2: the geometry's first, then the
  // overlays'.
  struct Translucent {
    Translucent() { GL_CHECKD(glGenBuffers(1, &sorted_elements)); }
    Translucent(const Translucent&) = delete;
    Translucent& operator=(const Translucent&) = delete;
    ~Translucent() { GL_CHECKD(glDeleteBuffers(1, &sorted_elements)); }

    VertexStateContainer container{false};
    GLuint sorted_elements = 0;
    std::vector<Vector3f> centroids;
    size_t geometry_triangles = 0;                    // the rest are overlays
    std::shared_ptr<VertexState> barycentric_state;   // for the edge shader; null without edges
    std::shared_ptr<VertexState> sorted_state;        // every triangle, from sorted_elements
    std::array<GLfloat, 3> sorted_for{NAN, NAN, NAN};  // the modelview's z row
  };
  void buildTranslucent(const std::vector<std::shared_ptr<const PolySet>>& polysets,
                        const Color4f& default_color, const ShaderUtils::ShaderInfo *shaderinfo,
                        double crease_degrees, bool edges);
  void sortTranslucent();

  std::vector<overlay::Mesh> overlays_;
  std::unique_ptr<Translucent> translucent_;
  std::vector<VertexStateContainer> overlay_vertex_state_containers_;  // the x-ray ones
};
