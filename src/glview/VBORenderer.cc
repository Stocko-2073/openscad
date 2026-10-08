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

#include <algorithm>
#include <cassert>
#include <array>
#include <cstdint>
#include <cstring>
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
  translucent_.reset();
  overlay_vertex_state_containers_.clear();
}

namespace {

bool isXRay(const overlay::Mesh& mesh) { return mesh.kind == overlay::Kind::Interference; }

// About 40k triangles for a translucent box filling the view.
constexpr double kTranslucentPiecesAcross = 100;

// The order of keys from least to greatest, equal keys in index order: an LSD radix sort, since
// turning the view re-sorts every translucent triangle each frame.
void sortedOrder(const std::vector<GLfloat>& keys, std::vector<GLuint>& order)
{
  struct Entry {
    uint32_t key;
    GLuint index;
  };
  const size_t n = keys.size();
  std::vector<Entry> entries(n), next(n);
  for (size_t i = 0; i < n; ++i) {
    uint32_t bits;
    std::memcpy(&bits, &keys[i], sizeof bits);
    // Flipped so that the unsigned order is the float order.
    entries[i] = {bits & 0x80000000u ? ~bits : bits | 0x80000000u, static_cast<GLuint>(i)};
  }
  for (int shift = 0; shift < 32; shift += 8) {
    std::array<size_t, 257> start{};
    for (const auto& e : entries) ++start[(e.key >> shift & 0xff) + 1];
    if (std::find(start.begin(), start.end(), n) != start.end()) continue;  // one digit throughout
    for (size_t d = 1; d < start.size(); ++d) start[d] += start[d - 1];
    for (const auto& e : entries) next[start[e.key >> shift & 0xff]++] = e;
    entries.swap(next);
  }
  order.resize(n);
  for (size_t i = 0; i < n; ++i) order[i] = entries[i].index;
}

}  // namespace

void VBORenderer::prepareTranslucent(const std::vector<std::shared_ptr<const PolySet>>& polysets,
                                     const Color4f& default_color,
                                     const ShaderUtils::ShaderInfo *shaderinfo, double crease_degrees,
                                     bool edges)
{
  if (!translucent_) {
    translucent_ = std::make_unique<Translucent>();
    buildTranslucent(polysets, default_color, shaderinfo, crease_degrees, edges);
  }
  sortTranslucent();
}

void VBORenderer::buildTranslucent(const std::vector<std::shared_ptr<const PolySet>>& polysets,
                                   const Color4f& default_color,
                                   const ShaderUtils::ShaderInfo *shaderinfo, double crease_degrees,
                                   bool edges)
{
  Translucent& translucent = *translucent_;
  // Triangles are sorted by their centroids, which misplaces a large one against the small ones it
  // passes in front of or behind, so large ones are cut to a size set by the whole scene.
  BoundingBox bbox;
  for (const auto& polyset : polysets) bbox.extend(polyset->getBoundingBox());
  bbox.extend(overlayBoundingBox());
  const double max_edge = bbox.isEmpty() ? 0 : bbox.sizes().norm() / kTranslucentPiecesAcross;

  std::vector<size_t> counts;
  size_t num_vertices = 0;
  for (const auto& polyset : polysets) {
    counts.push_back(VBOBuilder::surfaceVertexCount(*polyset, FaceFilter::Translucent,
                                                    default_color, false, max_edge));
    num_vertices += counts.back();
  }
  for (const auto& mesh : overlays_) {
    if (!isXRay(mesh)) {
      num_vertices +=
        VBOBuilder::surfaceVertexCount(*mesh.polyset, FaceFilter::All, {}, true, max_edge);
    }
  }
  if (num_vertices == 0) return;

  VBOBuilder vbo_builder(std::make_unique<VertexStateFactory>(), translucent.container);
  vbo_builder.addSurfaceData();
  vbo_builder.addShaderData();
  vbo_builder.allocateBuffers(num_vertices);
  // Its offset is where the next vertex goes, so it comes before any surface.
  if (edges && shaderinfo) {
    add_shader_pointers(vbo_builder, shaderinfo);
    translucent.barycentric_state = vbo_builder.states().back();
  }

  for (size_t i = 0; i < polysets.size(); ++i) {
    if (counts[i] == 0) continue;
    vbo_builder.writeSurface();
    vbo_builder.create_surface(*polysets[i], Transform3d::Identity(), default_color, true, false,
                               crease_degrees, FaceFilter::Translucent, edges ? 0 : 0b111, max_edge,
                               &translucent.centroids);
  }
  translucent.geometry_triangles = translucent.centroids.size();
  for (const auto& mesh : overlays_) {
    Color4f color;
    switch (mesh.kind) {
    case overlay::Kind::Highlight:    getColorSchemeColor(ColorMode::HIGHLIGHT, color); break;
    case overlay::Kind::Background:   getColorSchemeColor(ColorMode::BACKGROUND, color); break;
    case overlay::Kind::Interference: continue;
    }
    vbo_builder.writeSurface();
    vbo_builder.create_surface(*mesh.polyset, Transform3d::Identity(), color, true, true, 0,
                               FaceFilter::All, 0b111, max_edge, &translucent.centroids);
  }

  // create_surface() leaves a state per surface; these two draw them all from the start.
  translucent.sorted_state = std::make_shared<VertexState>(
    GL_TRIANGLES, static_cast<GLsizei>(3 * translucent.centroids.size()), GL_UNSIGNED_INT, 0, 0,
    translucent.container.verticesVBO(), translucent.sorted_elements);
  vbo_builder.states().push_back(translucent.sorted_state);
  vbo_builder.addAttributePointers(0);
  translucent.geometry_state = std::make_shared<VertexState>(
    GL_TRIANGLES, static_cast<GLsizei>(3 * translucent.geometry_triangles), 0, 0, 0,
    translucent.container.verticesVBO(), 0);
  vbo_builder.states().push_back(translucent.geometry_state);
  vbo_builder.addAttributePointers(0);

  vbo_builder.createInterleavedVBOs();
  translucent.container.states().clear();
}

void VBORenderer::sortTranslucent()
{
  Translucent& translucent = *translucent_;
  if (translucent.centroids.empty()) return;

  // Eye-space depth is this row's dot product plus a constant, so only turning re-sorts.
  GLfloat modelview[16];
  GL_CHECKD(glGetFloatv(GL_MODELVIEW_MATRIX, modelview));
  const std::array<GLfloat, 3> row = {modelview[2], modelview[6], modelview[10]};
  if (row == translucent.sorted_for) return;
  translucent.sorted_for = row;

  const Vector3f z(row[0], row[1], row[2]);
  std::vector<GLfloat> keys;
  keys.reserve(translucent.centroids.size());
  for (const auto& centroid : translucent.centroids) keys.push_back(z.dot(centroid));
  // Farthest first; a # overlay coinciding with its geometry ties, and stays after it.
  std::vector<GLuint> order;
  sortedOrder(keys, order);
  std::vector<GLuint> elements;
  elements.reserve(3 * order.size());
  for (const GLuint triangle : order) {
    for (GLuint corner = 0; corner < 3; ++corner) elements.push_back(3 * triangle + corner);
  }

  GL_TRACE("glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, %d)", translucent.sorted_elements);
  GL_CHECKD(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, translucent.sorted_elements));
  GL_TRACE("glBufferData(GL_ELEMENT_ARRAY_BUFFER, %d, %p, GL_STREAM_DRAW)",
           (elements.size() * sizeof(GLuint)) % (void *)elements.data());
  GL_CHECKD(glBufferData(GL_ELEMENT_ARRAY_BUFFER, elements.size() * sizeof(GLuint), elements.data(),
                         GL_STREAM_DRAW));
  GL_TRACE0("glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)");
  GL_CHECKD(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
}

void VBORenderer::drawTranslucent(bool showedges, const ShaderUtils::ShaderInfo *shaderinfo) const
{
  if (!translucent_ || !translucent_->sorted_state) return;
  const Translucent& translucent = *translucent_;

  if (shaderinfo && shaderinfo->type == ShaderUtils::ShaderType::SELECT_RENDERING) {
    GL_TRACE("glUseProgram(%d)", shaderinfo->resource.shader_program);
    GL_CHECKD(glUseProgram(shaderinfo->resource.shader_program));
    translucent.geometry_state->draw();
    GL_TRACE0("glUseProgram(0)");
    GL_CHECKD(glUseProgram(0));
    return;
  }

  // Edges only with the geometry's: overlays alone keep the fixed-function lighting.
  const bool edges = showedges && translucent.barycentric_state &&
                     translucent.geometry_triangles > 0 && shaderinfo &&
                     shaderinfo->type == ShaderUtils::ShaderType::EDGE_RENDERING;
  if (edges) {
    GL_TRACE("glUseProgram(%d)", shaderinfo->resource.shader_program);
    GL_CHECKD(glUseProgram(shaderinfo->resource.shader_program));
    VBOUtils::shader_attribs_enable(*shaderinfo);
    translucent.barycentric_state->draw();
  }
  // Pulled forward so a # subtree lying on the surface tints it.
  GL_TRACE0("glDepthMask(GL_FALSE)");
  GL_CHECKD(glDepthMask(GL_FALSE));
  GL_TRACE0("glDepthFunc(GL_LEQUAL)");
  GL_CHECKD(glDepthFunc(GL_LEQUAL));
  GL_TRACE0("glEnable(GL_POLYGON_OFFSET_FILL)");
  GL_CHECKD(glEnable(GL_POLYGON_OFFSET_FILL));
  GL_TRACE0("glPolygonOffset(-1, -1)");
  GL_CHECKD(glPolygonOffset(-1.0f, -1.0f));

  translucent.sorted_state->draw();

  GL_TRACE0("glDisable(GL_POLYGON_OFFSET_FILL)");
  GL_CHECKD(glDisable(GL_POLYGON_OFFSET_FILL));
  GL_TRACE0("glDepthFunc(GL_LESS)");
  GL_CHECKD(glDepthFunc(GL_LESS));
  GL_TRACE0("glDepthMask(GL_TRUE)");
  GL_CHECKD(glDepthMask(GL_TRUE));
  if (edges) {
    VBOUtils::shader_attribs_disable(*shaderinfo);
    GL_TRACE0("glUseProgram(0)");
    GL_CHECKD(glUseProgram(0));
  }
}

void VBORenderer::prepareOverlays()
{
  if (overlays_.empty() || !overlay_vertex_state_containers_.empty()) return;

  VertexStateContainer& container = overlay_vertex_state_containers_.emplace_back();
  size_t num_vertices = 0;
  for (const auto& mesh : overlays_) {
    if (isXRay(mesh)) num_vertices += VBOBuilder::surfaceVertexCount(*mesh.polyset);
  }
  if (num_vertices == 0) return;
  VBOBuilder vbo_builder(std::make_unique<VertexStateFactory>(), container);
  vbo_builder.addSurfaceData();
  vbo_builder.allocateBuffers(num_vertices);
  for (const auto& mesh : overlays_) {
    if (!isXRay(mesh)) continue;
    vbo_builder.writeSurface();
    vbo_builder.create_surface(*mesh.polyset, Transform3d::Identity(),
                               Color4f(1.0f, 0.25f, 0.25f, 0.6f), false, true);
  }
  vbo_builder.createInterleavedVBOs();
}

void VBORenderer::drawOverlays(const ShaderUtils::ShaderInfo *shaderinfo) const
{
  if (shaderinfo && shaderinfo->type == ShaderUtils::ShaderType::SELECT_RENDERING) return;
  if (overlay_vertex_state_containers_.empty()) return;

  // Overlaps are inside the geometry, so they are drawn through it.
  GL_TRACE0("glDisable(GL_DEPTH_TEST)");
  GL_CHECKD(glDisable(GL_DEPTH_TEST));
  for (const auto& vertex_state : overlay_vertex_state_containers_.front().states()) {
    vertex_state->draw();
  }
  GL_TRACE0("glEnable(GL_DEPTH_TEST)");
  GL_CHECKD(glEnable(GL_DEPTH_TEST));
}

BoundingBox VBORenderer::overlayBoundingBox() const
{
  BoundingBox bbox;
  for (const auto& mesh : overlays_) bbox.extend(mesh.polyset->getBoundingBox());
  return bbox;
}
