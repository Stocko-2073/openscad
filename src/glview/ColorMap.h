#pragma once

#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <list>
#include <vector>
#include <memory.h>

#include "geometry/linalg.h"

#include <filesystem>

namespace fs = std::filesystem;

enum class RenderColor {
  BACKGROUND_COLOR,
  BACKGROUND_STOP_COLOR,
  AXES_COLOR,
  OPENCSG_FACE_FRONT_COLOR,
  OPENCSG_FACE_BACK_COLOR,
  CGAL_FACE_FRONT_COLOR,
  CGAL_FACE_2D_COLOR,
  CGAL_FACE_BACK_COLOR,
  CGAL_EDGE_FRONT_COLOR,
  CGAL_EDGE_BACK_COLOR,
  CGAL_EDGE_2D_COLOR,
  CROSSHAIR_COLOR
};

using ColorScheme = std::map<RenderColor, Color4f>;

class RenderColorScheme
{
private:
  const fs::path _path;

  std::string _name;
  std::string _error;
  int _index;
  bool _show_in_gui;
  bool _user = false;

  ColorScheme _color_scheme;

public:
  /**
   * Constructor for the default color scheme Cornfield.
   */
  RenderColorScheme();
  /**
   * Constructor for reading external JSON files.
   */
  RenderColorScheme(const fs::path& path, bool user);
  /**
   * Constructor for a user scheme made at runtime.
   */
  RenderColorScheme(fs::path path, std::string name, int index, ColorScheme colors);
  virtual ~RenderColorScheme() = default;

  [[nodiscard]] const std::string& name() const;
  [[nodiscard]] int index() const;
  [[nodiscard]] bool valid() const;
  [[nodiscard]] bool showInGui() const;
  [[nodiscard]] bool isUser() const;
  [[nodiscard]] const ColorScheme& colorScheme() const;

private:
  [[nodiscard]] std::string path() const;
  [[nodiscard]] std::string error() const;

  friend class ColorMap;
};

class ColorMap
{
  using colorscheme_set_t =
    std::multimap<int, std::shared_ptr<const RenderColorScheme>, std::less<>>;

public:
  static ColorMap *inst(bool erase = false);

  [[nodiscard]] const char *defaultColorSchemeName() const;
  [[nodiscard]] const ColorScheme& defaultColorScheme() const;
  [[nodiscard]] const ColorScheme *findColorScheme(const std::string& name) const;
  [[nodiscard]] std::list<std::string> colorSchemeNames(bool guiOnly = false) const;

  // The scheme geometry takes its colors from when converted for display or export. An unknown
  // name selects the default.
  const ColorScheme& setActiveColorScheme(const std::string& name);
  [[nodiscard]] const ColorScheme& activeColorScheme() const;

  // User schemes are the files in the user config folder. These change them from the GUI thread,
  // returning an error message, empty on success.
  [[nodiscard]] bool isUserColorScheme(const std::string& name) const;
  [[nodiscard]] bool colorSchemeNameTaken(const std::string& name,
                                          const std::string& except = {}) const;
  std::string addUserColorScheme(const std::string& name, const ColorScheme& colors);
  std::string saveUserColorScheme(const std::string& name, const std::string& newName,
                                  const ColorScheme& colors);
  std::string removeUserColorScheme(const std::string& name);

  static Color4f getColor(const ColorScheme& cs, const RenderColor rc);
  static Color4f getContrastColor(const Color4f& col);
  static Color4f getColorHSV(const Color4f& col);

private:
  ColorMap();
  virtual ~ColorMap() = default;
  void dump() const;
  void enumerateColorSchemesInPath(const fs::path& path, bool user);
  // Callers hold mutex_.
  [[nodiscard]] std::shared_ptr<const RenderColorScheme> find(const std::string& name) const;
  void retire(const std::shared_ptr<const RenderColorScheme>& scheme,
              const std::shared_ptr<const RenderColorScheme>& replacement);

  mutable std::mutex mutex_;
  colorscheme_set_t colorSchemeSet;
  const std::shared_ptr<const RenderColorScheme> default_;
  std::shared_ptr<const RenderColorScheme> active_;
  // Views, renderers and worker threads keep raw pointers into schemes replaced or removed since.
  std::vector<std::shared_ptr<const RenderColorScheme>> retired_;
};
