#include "glview/VBOBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cassert>
#include <array>
#include <utility>
#include <vector>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <numeric>

#include "geometry/linalg.h"
#include "geometry/Polygon2d.h"
#include "utils/degree_trig.h"
#include "utils/printutils.h"
#include "utils/hash.h"  // IWYU pragma: keep

namespace {

Vector3d triangleNormal(const Vector3d& p0, const Vector3d& p1, const Vector3d& p2)
{
  const double ax = p1[0] - p0[0], bx = p1[0] - p2[0];
  const double ay = p1[1] - p0[1], by = p1[1] - p2[1];
  const double az = p1[2] - p0[2], bz = p1[2] - p2[2];
  const double nx = ay * bz - az * by;
  const double ny = az * bx - ax * bz;
  const double nz = ax * by - ay * bx;
  const double nl = sqrt(nx * nx + ny * ny + nz * nz);
  return {nx / nl, ny / nl, nz / nl};
}

// Bit i of hidden_edges hides the edge opposite corner i: the shader draws an edge where a coordinate
// nears 0, and one that is 1 at all three corners never does.
std::array<GLubyte, 3> barycentricFlags(size_t corner, uint8_t hidden_edges)
{
  std::array<GLubyte, 3> flags;
  for (size_t i = 0; i < 3; ++i) flags[i] = i == corner || (hidden_edges >> i & 1);
  return flags;
}

// A color that sets only some components, like color(alpha=0.5), takes the rest from the default.
Color4f polygonColor(const PolySet& ps, size_t i, const Color4f& default_color, bool force_default_color)
{
  if (force_default_color || i >= ps.color_indices.size()) return default_color;
  const int32_t index = ps.color_indices[i];
  if (index < 0 || static_cast<size_t>(index) >= ps.colors.size()) return default_color;
  return ps.colors[index].filledFrom(default_color);
}

bool passes(FaceFilter filter, const Color4f& color)
{
  return filter == FaceFilter::All || (filter == FaceFilter::Translucent) == (color.a() < 1.0f);
}

// Per triangle, the barycentricFlags() bits of the edges it shares with a same-colored triangle that
// lies flat with it or folds less than crease_degrees from it.
std::vector<uint8_t> hiddenEdges(const PolySet& ps, double crease_degrees, const Color4f& default_color,
                                 bool force_default_color)
{
  const size_t n = ps.indices.size();
  std::vector<Vector3d> normals(n);  // NaN where degenerate
  std::vector<double> areas(n);
  std::vector<uint32_t> first(ps.vertices.size() + 1, 0);
  for (size_t t = 0; t < n; ++t) {
    const auto& f = ps.indices[t];
    if (f.size() != 3) continue;
    const Vector3d& v0 = ps.vertices[f[0]];
    const Vector3d c = (ps.vertices[f[1]] - v0).cross(ps.vertices[f[2]] - v0);
    areas[t] = c.norm();
    normals[t] = c / areas[t];
    for (size_t k = 0; k < 3; ++k) ++first[std::min(f[k], f[(k + 1) % 3])];
  }
  std::partial_sum(first.begin(), first.end(), first.begin());
  // Bucketed by lower vertex v in [first[v], first[v + 1]), keyed by upper vertex << 1 | descending.
  struct HalfEdge {
    uint32_t key, edge;  // edge = 3 * triangle + k
  };
  std::vector<HalfEdge> half_edges(first.back());
  for (size_t t = 0; t < n; ++t) {
    const auto& f = ps.indices[t];
    if (f.size() != 3) continue;
    for (uint32_t k = 0; k < 3; ++k) {
      const uint32_t a = f[k], b = f[(k + 1) % 3];
      half_edges[--first[std::min(a, b)]] = {std::max(a, b) << 1 | (a > b),
                                             static_cast<uint32_t>(3 * t + k)};
    }
  }
  const double min_dot = cos_degrees(crease_degrees) + 1e-9;  // a fold of exactly the crease shows
  const BoundingBox bbox = ps.getBoundingBox();
  const double tolerance = 1e-6 * bbox.min().cwiseAbs().cwiseMax(bbox.max().cwiseAbs()).maxCoeff();
  std::vector<uint8_t> hidden(n, 0);
  for (size_t v = 0; v + 1 < first.size(); ++v) {
    const auto begin = half_edges.begin() + first[v], end = half_edges.begin() + first[v + 1];
    std::sort(begin, end, [](HalfEdge x, HalfEdge y) { return x.key < y.key; });
    for (auto run = begin, it = begin; run != end; run = it) {
      while (it != end && it->key >> 1 == run->key >> 1) ++it;
      // Only a manifold, consistently wound edge pairs up.
      if (it - run != 2 || run[0].key == run[1].key) continue;
      const uint32_t e = run[0].edge, g = run[1].edge, p = e / 3, q = g / 3;
      if (polygonColor(ps, p, default_color, force_default_color) !=
          polygonColor(ps, q, default_color, force_default_color)) {
        continue;
      }
      const double dot = normals[p].dot(normals[q]);
      bool hide = dot > min_dot;
      // Under 60° only, so a sliver thinner than the tolerance can't flatten a crease it lies along.
      if (!hide && !(dot < 0.5)) {
        const auto [s, l] = areas[p] < areas[q] ? std::pair{e, q} : std::pair{g, p};
        const Vector3d& apex = ps.vertices[ps.indices[s / 3][(s % 3 + 2) % 3]];
        hide = std::abs(normals[l].dot(apex - ps.vertices[v])) <= tolerance;
      }
      if (hide) {
        hidden[p] |= 1 << (e % 3 + 2) % 3;
        hidden[q] |= 1 << (g % 3 + 2) % 3;
      }
    }
  }
  return hidden;
}

// Halves the triangle across its longest edge until no edge is longer than max_edge, calling
// f(p0, p1, p2, hidden_edges) for each piece. A cut is a hidden edge; a piece of an edge shows as the
// edge does. Neighbors halve a shared edge at the same point.
template <typename F>
void subdivide(const Vector3d& p0, const Vector3d& p1, const Vector3d& p2, uint8_t hidden_edges,
               double max_edge2, const F& f)
{
  if (max_edge2 <= 0) {
    f(p0, p1, p2, hidden_edges);
    return;
  }
  const std::array<const Vector3d *, 3> p = {&p0, &p1, &p2};
  size_t k = 0;  // the corner opposite the longest edge
  double longest2 = 0;
  for (size_t i = 0; i < 3; ++i) {
    const double length2 = (*p[(i + 1) % 3] - *p[(i + 2) % 3]).squaredNorm();
    if (length2 > longest2) {
      longest2 = length2;
      k = i;
    }
  }
  if (!(longest2 > max_edge2)) {
    f(p0, p1, p2, hidden_edges);
    return;
  }
  const size_t a = (k + 1) % 3, b = (k + 2) % 3;
  const Vector3d mid = (*p[a] + *p[b]) / 2;
  const auto bit = [hidden_edges](size_t i) { return static_cast<uint8_t>(hidden_edges >> i & 1); };
  subdivide(*p[k], *p[a], mid, bit(k) | 0b010 | bit(b) << 2, max_edge2, f);
  subdivide(*p[k], mid, *p[b], bit(k) | bit(a) << 1 | 0b100, max_edge2, f);
}

// Calls f(color, p0, p1, p2, hidden_edges) for each triangle create_surface() makes of the faces
// that pass the filter, hidden giving each face's hidden edges.
template <typename F>
void forEachTriangle(const PolySet& ps, const Transform3d& m, const Color4f& default_color,
                     bool force_default_color, FaceFilter filter, const std::vector<uint8_t>& hidden,
                     double max_edge, const F& f)
{
  const double max_edge2 = max_edge * max_edge;
  for (size_t i = 0, n = ps.indices.size(); i < n; i++) {
    const auto& poly = ps.indices[i];
    const Color4f color = polygonColor(ps, i, default_color, force_default_color);
    if (!passes(filter, color)) continue;
    const auto triangle = [&](const Vector3d& p0, const Vector3d& p1, const Vector3d& p2,
                              uint8_t hidden_edges) {
      subdivide(p0, p1, p2, hidden_edges, max_edge2,
                [&](const Vector3d& q0, const Vector3d& q1, const Vector3d& q2, uint8_t q_hidden) {
                  f(color, q0, q1, q2, q_hidden);
                });
    };
    if (poly.size() == 3) {
      const Vector3d p0 = m * ps.vertices[poly.at(0)];
      const Vector3d p1 = m * ps.vertices[poly.at(1)];
      const Vector3d p2 = m * ps.vertices[poly.at(2)];

      triangle(p0, p1, p2, hidden.empty() ? 0 : hidden[i]);
    } else if (poly.size() == 4) {
      const Vector3d p0 = m * ps.vertices[poly.at(0)];
      const Vector3d p1 = m * ps.vertices[poly.at(1)];
      const Vector3d p2 = m * ps.vertices[poly.at(2)];
      const Vector3d p3 = m * ps.vertices[poly.at(3)];

      // Without the diagonal.
      triangle(p0, p1, p3, 0b001);
      triangle(p2, p3, p1, 0b001);
    } else {
      Vector3d center = Vector3d::Zero();
      for (const auto& idx : poly) {
        center += ps.vertices[idx];
      }
      center /= poly.size();
      for (size_t i = 1; i <= poly.size(); i++) {
        const Vector3d p0 = m * center;
        const Vector3d p1 = m * ps.vertices[poly.at(i % poly.size())];
        const Vector3d p2 = m * ps.vertices[poly.at(i - 1)];

        // Without the spokes.
        triangle(p0, p2, p1, 0b110);
      }
    }
  }
}

}  // namespace

size_t VBOBuilder::surfaceVertexCount(const PolySet& ps, FaceFilter filter, const Color4f& default_color,
                                      bool force_default_color, double max_edge)
{
  size_t count = 0;
  if (max_edge > 0) {
    forEachTriangle(ps, Transform3d::Identity(), default_color, force_default_color, filter, {},
                    max_edge, [&count](const auto&...) { count += 3; });
    return count;
  }
  for (size_t i = 0, n = ps.indices.size(); i < n; ++i) {
    if (filter != FaceFilter::All &&
        !passes(filter, polygonColor(ps, i, default_color, force_default_color))) {
      continue;
    }
    const size_t size = ps.indices[i].size();
    count += size == 3 ? 3 : size == 4 ? 6 : 3 * size;
  }
  return count;
}

// splitmix64's finalizer, so that the low bits, which pick the slot, depend on every byte.
uint64_t ElementsMap::hash(const GLbyte *vertex, size_t stride)
{
  uint64_t h = 0x9e3779b97f4a7c15ull ^ stride;
  for (size_t i = 0; i < stride; i += 8) {
    uint64_t word = 0;
    std::memcpy(&word, vertex + i, std::min<size_t>(8, stride - i));
    h = (h ^ word) * 0xbf58476d1ce4e5b9ull;
    h ^= h >> 29;
  }
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebull;
  h ^= h >> 31;
  return h;
}

void ElementsMap::prefetch(uint64_t hash) const
{
#if defined(__GNUC__) || defined(__clang__)
  if (!slots_.empty()) __builtin_prefetch(&slots_[hash & (slots_.size() - 1)]);
#endif
}

std::pair<GLuint, bool> ElementsMap::insert(const GLbyte *vertex, size_t stride, const GLbyte *keys,
                                            uint64_t hash)
{
  if ((size_ + 1) * 2 > slots_.size()) rehash(std::max<size_t>(64, slots_.size() * 2), stride, keys);

  const uint64_t tag = hash & 0xffffffff00000000ull;
  const size_t mask = slots_.size() - 1;
  for (size_t pos = hash & mask;; pos = (pos + 1) & mask) {
    const uint64_t slot = slots_[pos];
    if (slot == 0) {
      const auto index = static_cast<GLuint>(size_);
      slots_[pos] = tag | (static_cast<uint64_t>(index) + 1);
      used_.push_back(pos);
      ++size_;
      return {index, true};
    }
    if ((slot & 0xffffffff00000000ull) == tag) {
      const auto index = static_cast<GLuint>((slot & 0xffffffffull) - 1);
      if (std::memcmp(keys + index * stride, vertex, stride) == 0) return {index, false};
    }
  }
}

void ElementsMap::reserve(size_t count, size_t stride, const GLbyte *keys)
{
  size_t slot_count = 64;
  while (slot_count < count * 2) slot_count *= 2;
  if (slot_count > slots_.size()) rehash(slot_count, stride, keys);
  used_.reserve(count);
}

void ElementsMap::clear()
{
  for (const size_t pos : used_) slots_[pos] = 0;
  used_.clear();
  size_ = 0;
}

void ElementsMap::rehash(size_t slot_count, size_t stride, const GLbyte *keys)
{
  slots_.assign(slot_count, 0);
  used_.clear();
  const size_t mask = slot_count - 1;
  for (size_t index = 0; index < size_; ++index) {
    const uint64_t hash = ElementsMap::hash(keys + index * stride, stride);
    size_t pos = hash & mask;
    while (slots_[pos] != 0) pos = (pos + 1) & mask;
    slots_[pos] = (hash & 0xffffffff00000000ull) | (static_cast<uint64_t>(index) + 1);
    used_.push_back(pos);
  }
}

void addAttributeValues(IAttributeData&)
{
}

void VertexData::getLastVertex(std::vector<GLbyte>& interleaved_buffer) const
{
  GLbyte *dst_start = interleaved_buffer.data();
  for (const auto& data : attributes_) {
    size_t size = data->sizeofAttribute();
    GLbyte *dst = dst_start;
    const GLbyte *src = data->toBytes() + data->sizeInBytes() - data->sizeofAttribute();
    std::memcpy((void *)dst, (void *)src, size);
    dst_start += size;
  }
}

void VertexData::remove(size_t count)
{
  for (const auto& data : attributes_) {
    data->remove(count);
  }
}

// Adds attributes needed for regular 3D polygon rendering:
// position, normal, color
void VBOBuilder::addSurfaceData()
{
  auto vertex_data = std::make_shared<VertexData>();
  vertex_data->addPositionData(std::make_shared<AttributeData<GLfloat, 3, GL_FLOAT>>());
  vertex_data->addNormalData(std::make_shared<AttributeData<GLfloat, 3, GL_FLOAT>>());
  vertex_data->addColorData(std::make_shared<AttributeData<GLfloat, 4, GL_FLOAT>>());
  surface_index_ = vertices_.size();
  vertices_.emplace_back(std::move(vertex_data));
}

void VBOBuilder::addEdgeData()
{
  auto vertex_data = std::make_shared<VertexData>();
  vertex_data->addPositionData(std::make_shared<AttributeData<GLfloat, 3, GL_FLOAT>>());
  vertex_data->addColorData(std::make_shared<AttributeData<GLfloat, 4, GL_FLOAT>>());
  edge_index_ = vertices_.size();
  vertices_.emplace_back(std::move(vertex_data));
}

void VBOBuilder::createVertex(const std::array<Vector3d, 3>& points,
                              const std::array<Vector3d, 3>& normals, const Color4f& color,
                              size_t active_point_index, size_t primitive_index, size_t shape_size,
                              bool outlines, bool /*mirror*/)
{
  addAttributeValues(*(data()->positionData()), points[active_point_index][0],
                     points[active_point_index][1], points[active_point_index][2]);
  if (data()->hasNormalData()) {
    addAttributeValues(*(data()->normalData()), normals[active_point_index][0],
                       normals[active_point_index][1], normals[active_point_index][2]);
  }
  if (data()->hasColorData()) {
    addAttributeValues(*(data()->colorData()), color.r(), color.g(), color.b(), color.a());
  }

  if (!interleaved_buffer_.empty()) {
    vertex_.resize(data()->stride());
    data()->getLastVertex(vertex_);
    data()->clear();
    emitVertex(vertex_.data(), vertex_.size(),
               useElements() ? ElementsMap::hash(vertex_.data(), vertex_.size()) : 0);
  } else if (useElements()) {
    // The attributes keep each new vertex for createInterleavedVBOs() to interleave.
    vertex_.resize(data()->stride());
    data()->getLastVertex(vertex_);
    const auto [index, added] =
      elements_map_.insert(vertex_.data(), vertex_.size(), elementsKeys(vertex_.size()));
    if (added) {
      staged_keys_.insert(staged_keys_.end(), vertex_.begin(), vertex_.end());
      vertices_offset_ += vertex_.size();
    } else {
      data()->remove();
    }
    addAttributeValues(*elementsData(), index);
    elements_offset_ += elementsData()->sizeofAttribute();
  } else {
    vertices_offset_ = sizeInBytes();
  }
}

const GLbyte *VBOBuilder::elementsKeys(size_t stride)
{
  if (!interleaved_buffer_.empty()) {
    return interleaved_buffer_.data() + vertices_offset_ - elements_map_.size() * stride;
  }
  if (elements_map_.size() == 0) staged_keys_.clear();
  return staged_keys_.data();
}

void VBOBuilder::emitVertex(const GLbyte *vertex, size_t stride, uint64_t hash)
{
  assert(vertices_offset_ + stride <= interleaved_buffer_.size());
  if (useElements()) {
    const auto [index, added] = elements_map_.insert(vertex, stride, elementsKeys(stride), hash);
    if (added) {
      std::memcpy(interleaved_buffer_.data() + vertices_offset_, vertex, stride);
      vertices_offset_ += stride;
    }
    addAttributeValues(*elementsData(), index);
    elements_offset_ += elementsData()->sizeofAttribute();
  } else {
    std::memcpy(interleaved_buffer_.data() + vertices_offset_, vertex, stride);
    vertices_offset_ += stride;
  }
}

bool VBOBuilder::directTriangles(bool enable_barycentric)
{
  if (interleaved_buffer_.empty()) return false;
  const auto& vertex_data = *data();
  const auto& attributes = vertex_data.attributes();
  const auto is = [&](size_t index, GLenum type, size_t size, size_t count) {
    return attributes[index]->glType() == type && attributes[index]->sizeofType() == size &&
           attributes[index]->count() == count;
  };
  return vertex_data.hasPositionData() && vertex_data.positionIndex() == 0 &&
         vertex_data.hasNormalData() && vertex_data.normalIndex() == 1 && vertex_data.hasColorData() &&
         vertex_data.colorIndex() == 2 && attributes.size() == (enable_barycentric ? 4 : 3) &&
         is(0, GL_FLOAT, sizeof(GLfloat), 3) && is(1, GL_FLOAT, sizeof(GLfloat), 3) &&
         is(2, GL_FLOAT, sizeof(GLfloat), 4) &&
         (!enable_barycentric || (shader_attributes_index_ + BARYCENTRIC_ATTRIB == 3 &&
                                  is(3, GL_UNSIGNED_BYTE, sizeof(GLubyte), 4)));
}

void VBOBuilder::emitTriangle(const Color4f& color, const Vector3d& p0, const Vector3d& p1,
                              const Vector3d& p2, uint8_t hidden_edges, bool enable_barycentric,
                              bool mirror)
{
  const Vector3d n = triangleNormal(p0, p1, p2);
  const std::array<const Vector3d *, 3> points = {&p0, &p1, &p2};
  constexpr size_t floats_size = 10 * sizeof(GLfloat);
  const size_t stride = floats_size + (enable_barycentric ? 4 * sizeof(GLubyte) : 0);
  const bool elements = useElements();
  std::array<std::array<GLbyte, floats_size + 4 * sizeof(GLubyte)>, 3> vertices;
  std::array<uint64_t, 3> hashes{};
  for (size_t corner = 0; corner < 3; ++corner) {
    const Vector3d& p = *points[corner];
    std::array<GLfloat, 10> floats;
    for (size_t i = 0; i < 3; ++i) {
      floats[i] = static_cast<GLfloat>(p[i]);
      floats[3 + i] = static_cast<GLfloat>(n[i]);
    }
    floats[6] = color.r();
    floats[7] = color.g();
    floats[8] = color.b();
    floats[9] = color.a();
    std::memcpy(vertices[corner].data(), floats.data(), floats_size);
    if (enable_barycentric) {
      const auto flags = barycentricFlags(corner, hidden_edges);
      const std::array<GLubyte, 4> barycentric = {flags[0], flags[1], flags[2], 0};
      std::memcpy(vertices[corner].data() + floats_size, barycentric.data(), barycentric.size());
    }
    if (elements) {
      // Fetch the three slots at once rather than wait for each in turn.
      hashes[corner] = ElementsMap::hash(vertices[corner].data(), stride);
      elements_map_.prefetch(hashes[corner]);
    }
  }
  // As create_triangle() orders them: a mirrored triangle is wound the other way.
  const std::array<size_t, 3> order =
    mirror ? std::array<size_t, 3>{0, 2, 1} : std::array<size_t, 3>{0, 1, 2};
  for (const size_t corner : order) emitVertex(vertices[corner].data(), stride, hashes[corner]);
}

void VBOBuilder::createInterleavedVBOs()
{
  for (const auto& state : vertex_state_container_.states()) {
    state->setDrawOffset(this->indexOffset(state->drawOffset()));
  }

  // If the upfront size was not known, the the buffer has to be built
  size_t total_size = this->sizeInBytes();
  // If VertexArray is not empty, and initial size is zero
  if (interleaved_buffer_.empty() && total_size) {
    GL_TRACE("glBindBuffer(GL_ARRAY_BUFFER, %d)", vertex_state_container_.verticesVBO());
    GL_CHECKD(glBindBuffer(GL_ARRAY_BUFFER, vertex_state_container_.verticesVBO()));
    GL_TRACE("glBufferData(GL_ARRAY_BUFFER, %d, %p, GL_STATIC_DRAW)", total_size % (void *)nullptr);
    GL_CHECKD(glBufferData(GL_ARRAY_BUFFER, total_size, nullptr, GL_STATIC_DRAW));

    size_t dst_start = 0;
    for (const auto& vertex_data : vertices_) {
      // All attribute vectors need to be the same size to interleave
      size_t idx = 0, last_size = 0, stride = vertex_data->stride();
      for (const auto& data : vertex_data->attributes()) {
        size_t size = data->sizeofAttribute();
        const GLbyte *src = data->toBytes();
        size_t dst = dst_start;

        if (src) {
          if (idx != 0) {
            if (last_size != data->size() / data->count()) {
              PRINTDB("attribute data for vertex incorrect size at index %d = %d",
                      idx % (data->size() / data->count()));
              PRINTDB("last_size = %d", last_size);
              assert(false);
            }
          }
          last_size = data->size() / data->count();
          for (size_t i = 0; i < last_size; ++i) {
            // This path is chosen in vertex-object-renderers non-direct mode
            GL_TRACE("A glBufferSubData(GL_ARRAY_BUFFER, %p, %d, %p)", (void *)dst % size % (void *)src);
            GL_CHECKD(glBufferSubData(GL_ARRAY_BUFFER, dst, size, src));
            src += size;
            dst += stride;
          }
          dst_start += size;
        }
        idx++;
      }
      dst_start = vertex_data->sizeInBytes();
    }

    GL_TRACE0("glBindBuffer(GL_ARRAY_BUFFER, 0)");
    GL_CHECKD(glBindBuffer(GL_ARRAY_BUFFER, 0));
  } else if (!interleaved_buffer_.empty()) {
    GL_TRACE("glBindBuffer(GL_ARRAY_BUFFER, %d)", vertex_state_container_.verticesVBO());
    GL_CHECKD(glBindBuffer(GL_ARRAY_BUFFER, vertex_state_container_.verticesVBO()));
    GL_TRACE("glBufferData(GL_ARRAY_BUFFER, %d, %p, GL_STATIC_DRAW)",
             interleaved_buffer_.size() % (void *)interleaved_buffer_.data());
    GL_CHECKD(glBufferData(GL_ARRAY_BUFFER, interleaved_buffer_.size(), interleaved_buffer_.data(),
                           GL_STATIC_DRAW));
    GL_TRACE0("glBindBuffer(GL_ARRAY_BUFFER, 0)");
    GL_CHECKD(glBindBuffer(GL_ARRAY_BUFFER, 0));
  }

  PRINTDB("useElements() = %d, elements_size_ = %d", useElements() % elements_size_);
  if (useElements()) {
    GL_TRACE("glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, %d)", vertex_state_container_.elementsVBO());
    GL_CHECKD(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vertex_state_container_.elementsVBO()));
    if (elements_size_ == 0) {
      GL_TRACE("glBufferData(GL_ELEMENT_ARRAY_BUFFER, %d, %p, GL_STATIC_DRAW)",
               elements_.sizeInBytes() % (void *)nullptr);
      GL_CHECKD(glBufferData(GL_ELEMENT_ARRAY_BUFFER, elements_.sizeInBytes(), nullptr, GL_STATIC_DRAW));
    }
    size_t last_size = 0;
    for (const auto& e : elements_.attributes()) {
      GL_TRACE("glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, %d, %d, %p)",
               last_size % e->sizeInBytes() % (void *)e->toBytes());
      GL_CHECKD(glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, last_size, e->sizeInBytes(), e->toBytes()));
      last_size += e->sizeInBytes();
    }
    GL_TRACE0("glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)");
    GL_CHECKD(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
  }
}

void VBOBuilder::addAttributePointers(size_t start_offset)
{
  if (!this->data()) return;

  std::shared_ptr<VertexData> vertex_data = this->data();
  std::shared_ptr<VertexState> vertex_state = vertex_state_container_.states().back();

  GLsizei count = vertex_data->positionData()->count();
  GLenum type = vertex_data->positionData()->glType();
  GLsizei stride = vertex_data->stride();
  size_t offset = start_offset + vertex_data->interleavedOffset(vertex_data->positionIndex());
  vertex_state->glBegin().emplace_back([]() {
    GL_TRACE0("glEnableClientState(GL_VERTEX_ARRAY)");
    GL_CHECKD(glEnableClientState(GL_VERTEX_ARRAY));
  });
  vertex_state->glBegin().emplace_back(
    [count, type, stride, offset, vs_ptr = std::weak_ptr<VertexState>(vertex_state)]() {
      auto vs = vs_ptr.lock();
      if (vs) {
        // NOLINTBEGIN(performance-no-int-to-ptr)
        GL_TRACE("glVertexPointer(%d, %d, %d, %p)",
                 count % type % stride % (GLvoid *)(vs->drawOffset() + offset));
        GL_CHECKD(glVertexPointer(count, type, stride, (GLvoid *)(vs->drawOffset() + offset)));
        // NOLINTEND(performance-no-int-to-ptr)
      }
    });
  vertex_state->glEnd().emplace_back([]() {
    GL_TRACE0("glDisableClientState(GL_VERTEX_ARRAY)");
    GL_CHECKD(glDisableClientState(GL_VERTEX_ARRAY));
  });

  if (vertex_data->hasNormalData()) {
    type = vertex_data->normalData()->glType();
    size_t offset = start_offset + vertex_data->interleavedOffset(vertex_data->normalIndex());
    vertex_state->glBegin().emplace_back([]() {
      GL_TRACE0("glEnableClientState(GL_NORMAL_ARRAY)");
      GL_CHECKD(glEnableClientState(GL_NORMAL_ARRAY));
    });
    vertex_state->glBegin().emplace_back(
      [type, stride, offset, vs_ptr = std::weak_ptr<VertexState>(vertex_state)]() {
        auto vs = vs_ptr.lock();
        if (vs) {
          // NOLINTBEGIN(performance-no-int-to-ptr)
          GL_TRACE("glNormalPointer(%d, %d, %p)", type % stride % (GLvoid *)(vs->drawOffset() + offset));
          GL_CHECKD(glNormalPointer(type, stride, (GLvoid *)(vs->drawOffset() + offset)));
          // NOLINTEND(performance-no-int-to-ptr)
        }
      });
    vertex_state->glEnd().emplace_back([]() {
      GL_TRACE0("glDisableClientState(GL_NORMAL_ARRAY)");
      GL_CHECKD(glDisableClientState(GL_NORMAL_ARRAY));
    });
  }
  if (vertex_data->hasColorData()) {
    count = vertex_data->colorData()->count();
    type = vertex_data->colorData()->glType();
    size_t offset = start_offset + vertex_data->interleavedOffset(vertex_data->colorIndex());
    vertex_state->glBegin().emplace_back([]() {
      GL_TRACE0("glEnableClientState(GL_COLOR_ARRAY)");
      GL_CHECKD(glEnableClientState(GL_COLOR_ARRAY));
    });
    vertex_state->glBegin().emplace_back(
      [count, type, stride, offset, vs_ptr = std::weak_ptr<VertexState>(vertex_state)]() {
        auto vs = vs_ptr.lock();
        if (vs) {
          // NOLINTBEGIN(performance-no-int-to-ptr)
          GL_TRACE("glColorPointer(%d, %d, %d, %p)",
                   count % type % stride % (GLvoid *)(vs->drawOffset() + offset));
          GL_CHECKD(glColorPointer(count, type, stride, (GLvoid *)(vs->drawOffset() + offset)));
          // NOLINTEND(performance-no-int-to-ptr)
        }
      });
    vertex_state->glEnd().emplace_back([]() {
      GL_TRACE0("glDisableClientState(GL_COLOR_ARRAY)");
      GL_CHECKD(glDisableClientState(GL_COLOR_ARRAY));
    });
  }
}

// Allocates GPU memory for vertices (and elements if enabled)
// for holding the given number of vertices.
void VBOBuilder::allocateBuffers(size_t num_vertices)
{
  size_t vbo_buffer_size = num_vertices * stride();
  interleaved_buffer_.resize(vbo_buffer_size);
  GL_TRACE("glBindBuffer(GL_ARRAY_BUFFER, %d)", vertex_state_container_.verticesVBO());
  GL_CHECKD(glBindBuffer(GL_ARRAY_BUFFER, vertex_state_container_.verticesVBO()));
  GL_TRACE("glBufferData(GL_ARRAY_BUFFER, %d, %p, GL_STATIC_DRAW)", vbo_buffer_size % (void *)nullptr);
  GL_CHECKD(glBufferData(GL_ARRAY_BUFFER, vbo_buffer_size, nullptr, GL_STATIC_DRAW));
  if (useElements()) {
    // Use smallest possible index data type
    if (num_vertices <= 0xff) {
      addElementsData(std::make_shared<AttributeData<GLubyte, 1, GL_UNSIGNED_BYTE>>());
    } else if (num_vertices <= 0xffff) {
      addElementsData(std::make_shared<AttributeData<GLushort, 1, GL_UNSIGNED_SHORT>>());
    } else {
      addElementsData(std::make_shared<AttributeData<GLuint, 1, GL_UNSIGNED_INT>>());
    }
    // FIXME: How do we know how much to allocate?
    // FIXME: Should we preallocate so we don't have to make a bunch of glBufferSubData() calls?
    size_t elements_size = num_vertices * elements_.stride();
    setElementsSize(elements_size);
    GL_TRACE("glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, %d)", vertex_state_container_.elementsVBO());
    GL_CHECKD(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vertex_state_container_.elementsVBO()));
    GL_TRACE("glBufferData(GL_ELEMENT_ARRAY_BUFFER, %d, %p, GL_STATIC_DRAW)",
             elements_size % (void *)nullptr);
    GL_CHECKD(glBufferData(GL_ELEMENT_ARRAY_BUFFER, elements_size, nullptr, GL_STATIC_DRAW));
  }
}

// FIXME: This specifically adds barycentric vertex attributes, so document/rename accordingly
void VBOBuilder::addShaderData()
{
  const std::shared_ptr<VertexData> vertex_data = data();
  shader_attributes_index_ = vertex_data->attributes().size();
  vertex_data->addAttributeData(
    std::make_shared<AttributeData<GLubyte, 4, GL_UNSIGNED_BYTE>>());  // barycentric
}

void VBOBuilder::add_barycentric_attribute(size_t corner, uint8_t hidden_edges)
{
  const std::shared_ptr<VertexData> vertex_data = data();
  const auto barycentric_flags = barycentricFlags(corner, hidden_edges);
  addAttributeValues(*(vertex_data->attributes()[shader_attributes_index_ + BARYCENTRIC_ATTRIB]),
                     barycentric_flags[0], barycentric_flags[1], barycentric_flags[2], 0);
}

void VBOBuilder::create_triangle(const Color4f& color, const Vector3d& p0, const Vector3d& p1,
                                 const Vector3d& p2, uint8_t hidden_edges, bool enable_barycentric,
                                 bool mirror)
{
  const Vector3d n = triangleNormal(p0, p1, p2);

  if (!data()) return;

  if (enable_barycentric) {
    add_barycentric_attribute(0, hidden_edges);
  }
  createVertex({p0, p1, p2}, {n, n, n}, color, 0);

  if (!mirror) {
    if (enable_barycentric) {
      add_barycentric_attribute(1, hidden_edges);
    }
    createVertex({p0, p1, p2}, {n, n, n}, color, 1);
  }
  if (enable_barycentric) {
    add_barycentric_attribute(2, hidden_edges);
  }
  createVertex({p0, p1, p2}, {n, n, n}, color, 2);
  if (mirror) {
    if (enable_barycentric) {
      add_barycentric_attribute(1, hidden_edges);
    }
    createVertex({p0, p1, p2}, {n, n, n}, color, 1);
  }
}

// Creates a VBO "surface" from the PolySet.
// This will usually create a new VertexState and append it to our
// vertex states
void VBOBuilder::create_surface(const PolySet& ps, const Transform3d& m, const Color4f& default_color,
                                bool enable_barycentric, bool force_default_color,
                                double crease_degrees, FaceFilter filter, uint8_t hidden_mask,
                                double max_edge, std::vector<Vector3f> *centroids)
{
  const std::shared_ptr<VertexData> vertex_data = data();

  if (!vertex_data) {
    return;
  }

  const bool mirrored = m.matrix().determinant() < 0;
  size_t triangle_count = 0;

  const auto last_size = verticesOffset();

  size_t elements_offset = 0;
  if (useElements()) {
    elements_offset = elementsOffset();
    elementsMap().clear();
    elementsMap().reserve(surfaceVertexCount(ps), vertex_data->stride(),
                          elementsKeys(vertex_data->stride()));
  }

  const bool direct = directTriangles(enable_barycentric);
  const std::vector<uint8_t> hidden =
    enable_barycentric && hidden_mask != 0b111
      ? hiddenEdges(ps, crease_degrees, default_color, force_default_color)
      : std::vector<uint8_t>();
  forEachTriangle(ps, m, default_color, force_default_color, filter, hidden, max_edge,
                  [&](const Color4f& color, const Vector3d& p0, const Vector3d& p1,
                      const Vector3d& p2, uint8_t hidden_edges) {
                    hidden_edges |= hidden_mask;
                    if (direct) {
                      emitTriangle(color, p0, p1, p2, hidden_edges, enable_barycentric, mirrored);
                    } else {
                      create_triangle(color, p0, p1, p2, hidden_edges, enable_barycentric, mirrored);
                    }
                    if (centroids) centroids->emplace_back(((p0 + p1 + p2) / 3).cast<float>());
                    triangle_count++;
                  });

  GLenum elements_type = 0;
  if (useElements()) elements_type = elementsData()->glType();
  std::shared_ptr<VertexState> vertex_state =
    createVertexState(GL_TRIANGLES, triangle_count * 3, elements_type, writeIndex(), elements_offset);
  vertex_state_container_.states().emplace_back(std::move(vertex_state));
  addAttributePointers(last_size);
}

void VBOBuilder::create_edges(const Polygon2d& polygon, const Transform3d& m, const Color4f& color)
{
  const std::shared_ptr<VertexData> vertex_data = data();

  if (!vertex_data) return;

  auto& vertex_states = states();

  // Render only outlines
  for (const Outline2d& o : polygon.outlines()) {
    const auto last_size = verticesOffset();
    size_t elements_offset = 0;
    if (useElements()) {
      elements_offset = elementsOffset();
      elementsMap().clear();
    }
    for (const Vector2d& v : o.vertices) {
      const Vector3d p0 = m * Vector3d(v[0], v[1], 0.0);
      createVertex({p0}, {}, color, 0, 0, o.vertices.size(), true, false);
    }

    GLenum elements_type = 0;
    if (useElements()) elements_type = elementsData()->glType();
    std::shared_ptr<VertexState> line_loop =
      createVertexState(GL_LINE_LOOP, o.vertices.size(), elements_type, writeIndex(), elements_offset);
    vertex_states.emplace_back(std::move(line_loop));
    addAttributePointers(last_size);
  }
}

void VBOBuilder::create_polygons(const PolySet& ps, const Transform3d& m, const Color4f& color)
{
  assert(ps.getDimension() == 2);
  const std::shared_ptr<VertexData> vertex_data = data();

  if (!vertex_data) return;

  auto& vertex_states = states();

  PRINTD("create_polygons 2D");
  const bool mirrored = m.matrix().determinant() < 0;
  size_t triangle_count = 0;
  const auto last_size = verticesOffset();
  size_t elements_offset = 0;
  if (useElements()) {
    elements_offset = elementsOffset();
    elementsMap().clear();
    elementsMap().reserve(surfaceVertexCount(ps), vertex_data->stride(),
                          elementsKeys(vertex_data->stride()));
  }

  const bool direct = directTriangles(false);
  const auto triangle = [&](const Vector3d& p0, const Vector3d& p1, const Vector3d& p2) {
    if (direct) emitTriangle(color, p0, p1, p2, 0, false, mirrored);
    else create_triangle(color, p0, p1, p2, 0, false, mirrored);
  };

  for (const auto& poly : ps.indices) {
    if (poly.size() == 3) {
      const Vector3d p0 = m * ps.vertices[poly.at(0)];
      const Vector3d p1 = m * ps.vertices[poly.at(1)];
      const Vector3d p2 = m * ps.vertices[poly.at(2)];

      triangle(p0, p1, p2);
      triangle_count++;
    } else if (poly.size() == 4) {
      const Vector3d p0 = m * ps.vertices[poly.at(0)];
      const Vector3d p1 = m * ps.vertices[poly.at(1)];
      const Vector3d p2 = m * ps.vertices[poly.at(2)];
      const Vector3d p3 = m * ps.vertices[poly.at(3)];

      triangle(p0, p1, p3);
      triangle(p2, p3, p1);
      triangle_count += 2;
    } else {
      Vector3d center = Vector3d::Zero();
      for (const auto& point : poly) {
        center[0] += ps.vertices[point][0];
        center[1] += ps.vertices[point][1];
      }
      center[0] /= poly.size();
      center[1] /= poly.size();

      for (size_t i = 1; i <= poly.size(); i++) {
        const Vector3d p0 = m * center;
        const Vector3d p1 = m * ps.vertices[poly.at(i % poly.size())];
        const Vector3d p2 = m * ps.vertices[poly.at(i - 1)];

        triangle(p0, p2, p1);
        triangle_count++;
      }
    }
  }

  GLenum elements_type = 0;
  if (useElements()) elements_type = elementsData()->glType();
  std::shared_ptr<VertexState> vs =
    createVertexState(GL_TRIANGLES, triangle_count * 3, elements_type, writeIndex(), elements_offset);
  vertex_states.emplace_back(std::move(vs));
  addAttributePointers(last_size);
}
