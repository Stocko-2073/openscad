#pragma once

#include "geometry/linalg.h"
#include "glview/ShaderUtils.h"
#include "glview/ColorMap.h"
#include "core/enums.h"
#include "core/Selection.h"

#ifdef _MSC_VER  // NULL
#include <map>
#include <cstdlib>
#endif

#include <vector>

class Renderer
{
public:
  Renderer();
  virtual ~Renderer() = default;

  virtual void prepare(const ShaderUtils::ShaderInfo *shaderinfo) = 0;
  virtual void draw(bool showedges, const ShaderUtils::ShaderInfo *shaderinfo) const = 0;
  [[nodiscard]] virtual BoundingBox getBoundingBox() const = 0;

  enum class ColorMode {
    NONE,
    MATERIAL,
    CUTOUT,
    HIGHLIGHT,
    BACKGROUND,
    MATERIAL_EDGES,
    CUTOUT_EDGES,
    HIGHLIGHT_EDGES,
    BACKGROUND_EDGES,
    CGAL_FACE_2D_COLOR,
    CGAL_EDGE_2D_COLOR,
    EMPTY_SPACE
  };

  bool getColorSchemeColor(ColorMode colormode, Color4f& outcolor) const;
  bool getShaderColor(Renderer::ColorMode colormode, const Color4f& object_color,
                      Color4f& outcolor) const;
  virtual void setColorScheme(const ColorScheme& cs);

  /**
   * @brief Find object(s) the mouse is pointing at in the viewport.
   * Used to find the point or line the user's mouse cursor is closest to when over the viewport.
   * Near_pt will be on the "camera lens" and far_pt will be "the back wall" of the render cube.
   */
  virtual std::vector<SelectedObject> findModelObject(const Vector3d& near_pt, const Vector3d& far_pt,
                                                      int mouse_x, int mouse_y, double tolerance);

protected:
  std::map<ColorMode, Color4f> colormap_;
  const ColorScheme *colorscheme_{nullptr};
  void setupShader();
};
