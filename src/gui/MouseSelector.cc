#include "gui/MouseSelector.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>

#include "glview/ShaderUtils.h"
#include "glview/fbo.h"
#include "glview/system-gl.h"
#include "utils/printutils.h"
/**
 * The view is drawn with a flat, unlit shader into an offscreen framebuffer, and the depth under
 * the cursor is read back. The picker turns that into a point on the rendered surface.
 */

MouseSelector::MouseSelector(GLView *view)
{
  this->view = view;
  if (view) this->reset(view);
}

/**
 * Resize the framebuffer whenever it changed
 */
void MouseSelector::reset(GLView *view)
{
  this->view = view;
  this->setupFramebuffer(view->cam.pixel_width, view->cam.pixel_height);
}

/**
 * Initialize the used shaders and setup the ShaderInfo struct
 */
void MouseSelector::initShader()
{
  // Attributes:
  // frag_idcolor - (uniform) the flat color to draw with
  const auto selectshader =
    ShaderUtils::compileShaderProgram(ShaderUtils::loadShaderSource("MouseSelector.vert"),
                                      ShaderUtils::loadShaderSource("MouseSelector.frag"));

  const GLint frag_idcolor = glGetUniformLocation(selectshader.shader_program, "frag_idcolor");
  if (frag_idcolor < 0) {
    // TODO: Surface error better
    fprintf(stderr, __FILE__ ": OpenGL symbol retrieval went wrong, id is %i\n\n", frag_idcolor);
  }
  this->shaderinfo = {
    .resource = selectshader,
    .type = ShaderUtils::ShaderType::SELECT_RENDERING,
    .uniforms =
      {
        {"frag_idcolor", glGetUniformLocation(selectshader.shader_program, "frag_idcolor")},
      },
  };
}

/**
 * Resize or create the framebuffer
 */
void MouseSelector::setupFramebuffer(int width, int height)
{
  if (!this->framebuffer || this->framebuffer->width() != width ||
      this->framebuffer->height() != height) {
    this->framebuffer = createFBO(width, height);
    if (!this->framebuffer) {
      LOG(message_group::Error,
          "MouseSelector: Failed to create framebuffer; disabling mouse selection.");
      return;
    }
    // We bind the framebuffer before initializing shaders since
    // shader validation requires a valid framebuffer.
    this->framebuffer->bind();
    this->initShader();
    this->framebuffer->unbind();
  }
}

/**
 * Setup the shaders, Projection and Model matrix and call the given renderer, then read back the
 * depth at (x, y).
 */
std::optional<float> MouseSelector::depthAt(const Renderer *renderer, int x, int y)
{
  if (!this->framebuffer) return std::nullopt;

  // This function should render a frame, as usual, with the following changes:
  // * Render to as custom framebuffer
  // * The shader should be the selector shader
  // * Only depth is read back, so no color setup is needed
  // * No lighting
  // * No decorations, like axes

  // TODO: Ideally, we should make the above configurable and reduce duplicate render code in this
  // function.

  const int width = this->view->cam.pixel_width;
  const int height = this->view->cam.pixel_height;
  if (x >= width || x < 0 || y >= height || y < 0) {
    return std::nullopt;
  }

  // Initialize GL to draw to texture
  // Ideally a texture of only 1x1 or 2x2 pixels as a subset of the viewing frustrum
  // of the currently selected frame.
  // For now, i will use a texture the same size as the normal viewport
  // and read the depth at the mouse coordinates
  GL_CHECKD(this->framebuffer->bind());

  glClearColor(0, 0, 0, 1.0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

  glViewport(0, 0, width, height);
  this->view->setupCamera();
  glTranslated(this->view->cam.object_trans.x(), this->view->cam.object_trans.y(),
               this->view->cam.object_trans.z());

  glDisable(GL_LIGHTING);
  glDepthFunc(GL_LESS);
  glCullFace(GL_BACK);
  glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);

  // call the renderer with the selector shader
  GL_CHECKD(renderer->draw(false, &this->shaderinfo));

  // Not strictly necessary, but a nop if not required.
  glFlush();
  glFinish();

  // Before unbinding: the depth lives in this framebuffer, not the widget's.
  // Qt counts rows from the top, GL from the bottom.
  float depth = 1.0f;
  GL_CHECKD(glReadPixels(x, height - 1 - y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth));

  // Switch the active framebuffer back to the default
  this->framebuffer->unbind();

  if (depth >= 1.0f) return std::nullopt;  // background
  return depth;
}
