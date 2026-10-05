#pragma once

#include <memory>

#include "glview/GLView.h"
#include "glview/Renderer.h"
#include "glview/ShaderUtils.h"
#include "glview/fbo.h"

/**
 * Grab the of the Tree element that was rendered at a specific location
 */
class MouseSelector
{
public:
  MouseSelector(GLView *view);

  /// Resize the renderbuffer
  void reset(GLView *view);

  // `depth`, when given, receives the window depth drawn at (x, y): 1 where nothing was drawn.
  int select(const Renderer *renderer, int x, int y, float *depth = nullptr);

  ShaderUtils::ShaderInfo shaderinfo;

private:
  void initShader();
  void setupFramebuffer(int width, int height);

  std::unique_ptr<FBO> framebuffer;

  GLView *view;
};
