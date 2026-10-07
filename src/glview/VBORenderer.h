#pragma once

#include <array>
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
  virtual size_t calcNumVertices(const PolySet& polyset) const;
  virtual size_t calcNumEdgeVertices(const PolySet& polyset) const;
  virtual size_t calcNumEdgeVertices(const Polygon2d& polygon) const;

  void add_shader_pointers(
    VBOBuilder& vbo_builder,
    const ShaderUtils::ShaderInfo
      *shaderinfo);  // This could stay protected, were it not for VertexStateManager

  // The # and % subtrees and interference overlaps, drawn translucent and never picked.
  void setOverlays(std::vector<overlay::Mesh> overlays);

protected:
  void add_shader_data(VBOBuilder& vbo_builder);
  void shader_attribs_enable(const ShaderUtils::ShaderInfo&) const;
  void shader_attribs_disable(const ShaderUtils::ShaderInfo&) const;

  // For prepare(), draw() and getBoundingBox() to call after handling the geometry.
  void prepareOverlays();
  void drawOverlays(const ShaderUtils::ShaderInfo *shaderinfo) const;
  [[nodiscard]] BoundingBox overlayBoundingBox() const;

private:
  std::vector<overlay::Mesh> overlays_;
  std::vector<VertexStateContainer> overlay_vertex_state_containers_;  // depth-tested, then x-ray
};
