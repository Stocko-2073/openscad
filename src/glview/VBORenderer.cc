/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright (C) 2009-2011 Clifford Wolf <clifford@clifford.at> and
 *                          Marius Kintel <marius@kintel.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  As a special exception, you have permission to link this program
 *  with the CGAL library and distribute executables, as long as you
 *  follow the requirements of the GNU GPL in regard to all of the
 *  software in the executable aside from CGAL.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "glview/VBORenderer.h"
#include "glview/ShaderUtils.h"
#include "geometry/linalg.h"
#include "geometry/Polygon2d.h"
#include "geometry/PolySet.h"
#include "utils/printutils.h"
#include "utils/hash.h"  // IWYU pragma: keep

#include <cassert>
#include <array>
#include <unordered_map>
#include <utility>
#include <memory>
#include <cstddef>
#include <vector>

namespace VBOUtils {

void shader_attribs_enable(const ShaderUtils::ShaderInfo& shaderinfo)
{
  for (const auto& [name, location] : shaderinfo.attributes) {
    GL_TRACE("glEnableVertexAttribArray(%d)", location);
    GL_CHECKD(glEnableVertexAttribArray(location));
  }
}

void shader_attribs_disable(const ShaderUtils::ShaderInfo& shaderinfo)
{
  for (const auto& [name, location] : shaderinfo.attributes) {
    GL_TRACE("glEnableVertexAttribArray(%d)", location);
    GL_CHECKD(glDisableVertexAttribArray(location));
  }
}

}  // namespace VBOUtils

VBORenderer::VBORenderer() : Renderer()
{
}

size_t VBORenderer::calcNumVertices(const PolySet& polyset) const
{
  size_t buffer_size = 0;
  for (const auto& poly : polyset.indices) {
    if (poly.size() == 3) {
      buffer_size++;
    } else if (poly.size() == 4) {
      buffer_size += 2;
    } else {
      // poly.size() because we'll render a triangle fan from the centroid
      // FIXME: Are we still using this code path?
      buffer_size += poly.size();
    }
  }
  return buffer_size * 3;
}

size_t VBORenderer::calcNumEdgeVertices(const PolySet& polyset) const
{
  size_t buffer_size = 0;
  for (const auto& polygon : polyset.indices) {
    buffer_size += polygon.size();
  }
  return buffer_size;
}

size_t VBORenderer::calcNumEdgeVertices(const Polygon2d& polygon) const
{
  size_t buffer_size = 0;
  // Render only outlines
  for (const Outline2d& o : polygon.outlines()) {
    buffer_size += o.vertices.size();
  }
  return buffer_size;
}

void VBORenderer::add_shader_pointers(VBOBuilder& vbo_builder, const ShaderUtils::ShaderInfo *shaderinfo)
{
  const std::shared_ptr<VertexData> vertex_data = vbo_builder.data();

  if (!vertex_data) return;

  const auto start_offset = vbo_builder.verticesOffset();

  std::shared_ptr<VertexState> ss = std::make_shared<VBOShaderVertexState>(
    vbo_builder.writeIndex(), 0, vbo_builder.verticesVBO(), vbo_builder.elementsVBO());
  GLsizei count = 0, stride = 0;
  GLenum type = 0;
  size_t offset = 0;

  GLuint attribute_index = shaderinfo->attributes.at("barycentric");
  if (attribute_index > 0) {
    count =
      vertex_data->attributes()[vbo_builder.shader_attributes_index_ + BARYCENTRIC_ATTRIB]->count();
    type =
      vertex_data->attributes()[vbo_builder.shader_attributes_index_ + BARYCENTRIC_ATTRIB]->glType();
    stride = vertex_data->stride();
    offset = start_offset +
             vertex_data->interleavedOffset(vbo_builder.shader_attributes_index_ + BARYCENTRIC_ATTRIB);
    ss->glBegin().emplace_back(
      [attribute_index, count, type, stride, offset, ss_ptr = std::weak_ptr<VertexState>(ss)]() {
        auto ss = ss_ptr.lock();
        if (ss) {
          // NOLINTBEGIN(performance-no-int-to-ptr)
          GL_TRACE("glVertexAttribPointer(%d, %d, %d, GL_FALSE, %d, %p)",
                   attribute_index % count % type % stride % (GLvoid *)(ss->drawOffset() + offset));
          GL_CHECKD(glVertexAttribPointer(attribute_index, count, type, GL_FALSE, stride,
                                          (GLvoid *)(ss->drawOffset() + offset)));
          // NOLINTEND(performance-no-int-to-ptr)
        }
      });
  }

  vbo_builder.states().emplace_back(std::move(ss));
}

void VBORenderer::setOverlays(std::vector<overlay::Mesh> overlays)
{
  overlays_ = std::move(overlays);
  overlay_vertex_state_containers_.clear();
}

namespace {

// Interference overlaps are drawn through the geometry; the rest is depth-tested against it.
bool isXRay(const overlay::Mesh& mesh) { return mesh.kind == overlay::Kind::Interference; }

}  // namespace

void VBORenderer::prepareOverlays()
{
  if (overlays_.empty() || !overlay_vertex_state_containers_.empty()) return;

  for (const bool xray : {false, true}) {
    VertexStateContainer& container = overlay_vertex_state_containers_.emplace_back();
    VBOBuilder vbo_builder(std::make_unique<VertexStateFactory>(), container);
    vbo_builder.addSurfaceData();

    size_t num_vertices = 0;
    for (const auto& mesh : overlays_) {
      if (isXRay(mesh) == xray) num_vertices += calcNumVertices(*mesh.polyset);
    }
    if (num_vertices == 0) continue;
    vbo_builder.allocateBuffers(num_vertices);

    for (const auto& mesh : overlays_) {
      if (isXRay(mesh) != xray) continue;
      Color4f color;
      switch (mesh.kind) {
      case overlay::Kind::Highlight:    getColorSchemeColor(ColorMode::HIGHLIGHT, color); break;
      case overlay::Kind::Background:   getColorSchemeColor(ColorMode::BACKGROUND, color); break;
      case overlay::Kind::Interference: color = Color4f(1.0f, 0.25f, 0.25f, 0.6f); break;
      }
      vbo_builder.writeSurface();
      vbo_builder.create_surface(*mesh.polyset, Transform3d::Identity(), color, false, true);
    }

    vbo_builder.createInterleavedVBOs();
  }
}

void VBORenderer::drawOverlays(const ShaderUtils::ShaderInfo *shaderinfo) const
{
  // Picking sees the geometry only.
  if (shaderinfo && shaderinfo->type == ShaderUtils::ShaderType::SELECT_RENDERING) return;
  if (overlay_vertex_state_containers_.size() != 2) return;

  // Translucent: tested against the geometry's depth without writing any, and pulled forward so a
  // # subtree lying on the surface tints it.
  GL_TRACE0("glDepthMask(GL_FALSE)");
  GL_CHECKD(glDepthMask(GL_FALSE));
  GL_TRACE0("glDepthFunc(GL_LEQUAL)");
  GL_CHECKD(glDepthFunc(GL_LEQUAL));
  GL_TRACE0("glEnable(GL_POLYGON_OFFSET_FILL)");
  GL_CHECKD(glEnable(GL_POLYGON_OFFSET_FILL));
  GL_TRACE0("glPolygonOffset(-1, -1)");
  GL_CHECKD(glPolygonOffset(-1.0f, -1.0f));
  for (const auto& vertex_state : overlay_vertex_state_containers_[0].states()) vertex_state->draw();
  GL_TRACE0("glDisable(GL_POLYGON_OFFSET_FILL)");
  GL_CHECKD(glDisable(GL_POLYGON_OFFSET_FILL));

  // Overlaps are inside the geometry, so they are drawn through it.
  if (!overlay_vertex_state_containers_[1].states().empty()) {
    GL_TRACE0("glDisable(GL_DEPTH_TEST)");
    GL_CHECKD(glDisable(GL_DEPTH_TEST));
    for (const auto& vertex_state : overlay_vertex_state_containers_[1].states()) vertex_state->draw();
    GL_TRACE0("glEnable(GL_DEPTH_TEST)");
    GL_CHECKD(glEnable(GL_DEPTH_TEST));
  }

  GL_TRACE0("glDepthFunc(GL_LESS)");
  GL_CHECKD(glDepthFunc(GL_LESS));
  GL_TRACE0("glDepthMask(GL_TRUE)");
  GL_CHECKD(glDepthMask(GL_TRUE));
}

BoundingBox VBORenderer::overlayBoundingBox() const
{
  BoundingBox bbox;
  for (const auto& mesh : overlays_) bbox.extend(mesh.polyset->getBoundingBox());
  return bbox;
}
