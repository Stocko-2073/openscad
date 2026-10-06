#pragma once

#include <memory>
#include <optional>

#include "glview/GLView.h"
#include "glview/Renderer.h"
#include "glview/ShaderUtils.h"
#include "glview/fbo.h"

/**
 * Finds what was drawn at a location of the view: draws it offscreen and reads the depth back.
 */
class MouseSelector
{
public:
  MouseSelector(GLView *view);

  /// Resize the renderbuffer
  void reset(GLView *view);

  // The window depth drawn at (x, y), or nothing when (x, y) is outside the view or shows background.
  std::optional<float> depthAt(const Renderer *renderer, int x, int y);

  ShaderUtils::ShaderInfo shaderinfo;

private:
  void initShader();
  void setupFramebuffer(int width, int height);

  std::unique_ptr<FBO> framebuffer;

  GLView *view;
};
