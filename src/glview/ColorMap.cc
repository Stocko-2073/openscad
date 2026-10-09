#include "glview/ColorMap.h"
#include "core/ColorUtil.h"
#include "utils/printutils.h"
#include "platform/PlatformUtils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <list>
#include <utility>
#include <exception>
#include <memory>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/format.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <filesystem>
#include <cmath>

#include "json/json.hpp"

namespace fs = std::filesystem;

static const char *DEFAULT_COLOR_SCHEME_NAME = "Cornfield";

namespace {

struct ColorKey {
  RenderColor color;
  const char *key;
};

// In the order the bundled files list them.
const std::array<ColorKey, 12> COLOR_KEYS{{
  {RenderColor::BACKGROUND_COLOR, "background"},
  {RenderColor::BACKGROUND_STOP_COLOR, "background-stop"},
  {RenderColor::AXES_COLOR, "axes-color"},
  {RenderColor::OPENCSG_FACE_FRONT_COLOR, "opencsg-face-front"},
  {RenderColor::OPENCSG_FACE_BACK_COLOR, "opencsg-face-back"},
  {RenderColor::CGAL_FACE_FRONT_COLOR, "cgal-face-front"},
  {RenderColor::CGAL_FACE_BACK_COLOR, "cgal-face-back"},
  {RenderColor::CGAL_FACE_2D_COLOR, "cgal-face-2d"},
  {RenderColor::CGAL_EDGE_FRONT_COLOR, "cgal-edge-front"},
  {RenderColor::CGAL_EDGE_BACK_COLOR, "cgal-edge-back"},
  {RenderColor::CGAL_EDGE_2D_COLOR, "cgal-edge-2d"},
  {RenderColor::CROSSHAIR_COLOR, "crosshair"},
}};

Color4f readColor(const boost::property_tree::ptree& colors, const char *key)
{
  auto color = colors.get<std::string>(key);
  if ((color.length() == 7) && (color.at(0) == '#')) {
    char *endptr;
    unsigned int val = strtol(color.substr(1).c_str(), &endptr, 16);
    int r = (val >> 16) & 0xff;
    int g = (val >> 8) & 0xff;
    int b = val & 0xff;
    return {r, g, b};
  }
  throw std::invalid_argument(std::string("invalid color value for key '") + key + "': '" + color +
                              "'");
}

std::string hexColor(const Color4f& color)
{
  const auto channel = [](float v) {
    return std::clamp(static_cast<int>(std::lround(v * 255.0f)), 0, 255);
  };
  std::array<char, 8> hex{};
  std::snprintf(hex.data(), hex.size(), "#%02x%02x%02x", channel(color.r()), channel(color.g()),
                channel(color.b()));
  return hex.data();
}

std::string writeColorScheme(const fs::path& path, const std::string& name, int index,
                             const ColorScheme& scheme)
{
  nlohmann::ordered_json json;
  json["name"] = name;
  json["index"] = index;
  json["show-in-gui"] = true;
  json["edge-brightness"] = scheme.edge_brightness;
  auto& jsonColors = json["colors"];
  for (const auto& [color, key] : COLOR_KEYS) {
    jsonColors[key] = hexColor(ColorMap::getColor(scheme, color));
  }

  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream file(tmp);
    file << json.dump(4, ' ', false, nlohmann::ordered_json::error_handler_t::replace) << "\n";
    if (!file) return (boost::format(_("Can't write '%1$s'")) % tmp.generic_string()).str();
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(tmp, ignored);
    return (boost::format(_("Can't write '%1$s': %2$s")) % path.generic_string() % ec.message()).str();
  }
  return {};
}

// A file name from the scheme's name, kept to ASCII so it means the same on every platform.
fs::path userSchemePath(const fs::path& dir, const std::string& name)
{
  std::string stem;
  for (const unsigned char c : name) {
    if (std::isalnum(c) && c < 0x80) {
      stem += static_cast<char>(std::tolower(c));
    } else if (!stem.empty() && stem.back() != '-') {
      stem += '-';
    }
  }
  while (!stem.empty() && stem.back() == '-') stem.pop_back();
  if (stem.empty()) stem = "color-scheme";

  fs::path path = dir / (stem + ".json");
  for (int n = 2; fs::exists(path); ++n) {
    path = dir / (stem + "-" + std::to_string(n) + ".json");
  }
  return path;
}

template <typename Set>
bool nameTaken(const Set& set, const std::string& name, const std::string& except)
{
  return std::any_of(set.begin(), set.end(), [&](const auto& item) {
    return item.second->name() != except && boost::algorithm::iequals(item.second->name(), name);
  });
}

}  // namespace

RenderColorScheme::RenderColorScheme() : _path("")
{
  _name = DEFAULT_COLOR_SCHEME_NAME;
  _index = 1000;
  _show_in_gui = true;

  auto& colors = _color_scheme.colors;
  colors.emplace(RenderColor::BACKGROUND_COLOR, Color4f(0xff, 0xff, 0xe5));
  colors.emplace(RenderColor::BACKGROUND_STOP_COLOR, Color4f(0xff, 0xff, 0xe5));
  colors.emplace(RenderColor::AXES_COLOR, Color4f(0x00, 0x00, 0x00));
  colors.emplace(RenderColor::OPENCSG_FACE_FRONT_COLOR, Color4f(0xf9, 0xd7, 0x2c));
  colors.emplace(RenderColor::OPENCSG_FACE_BACK_COLOR, Color4f(0x9d, 0xcb, 0x51));
  colors.emplace(RenderColor::CGAL_FACE_FRONT_COLOR, Color4f(0xf9, 0xd7, 0x2c));
  colors.emplace(RenderColor::CGAL_FACE_2D_COLOR, Color4f(0x00, 0xbf, 0x99));
  colors.emplace(RenderColor::CGAL_FACE_BACK_COLOR, Color4f(0x9d, 0xcb, 0x51));
  colors.emplace(RenderColor::CGAL_EDGE_FRONT_COLOR, Color4f(0xff, 0xec, 0x5e));
  colors.emplace(RenderColor::CGAL_EDGE_BACK_COLOR, Color4f(0xab, 0xd8, 0x56));
  colors.emplace(RenderColor::CGAL_EDGE_2D_COLOR, Color4f(0xff, 0x00, 0x00));
  colors.emplace(RenderColor::CROSSHAIR_COLOR, Color4f(0x80, 0x00, 0x00));
}

RenderColorScheme::RenderColorScheme(const fs::path& path, bool user) : _path(path), _user(user)
{
  try {
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(path.generic_string().c_str(), pt);
    _name = pt.get<std::string>("name");
    _index = pt.get<int>("index");
    _show_in_gui = pt.get<bool>("show-in-gui");
    _color_scheme.edge_brightness =
      std::clamp(pt.get("edge-brightness", _color_scheme.edge_brightness), 0.0, 1.0);

    const boost::property_tree::ptree& colors = pt.get_child("colors");
    auto& schemeColors = _color_scheme.colors;
    for (const auto& [color, key] : COLOR_KEYS) {
      if (color != RenderColor::BACKGROUND_STOP_COLOR) schemeColors[color] = readColor(colors, key);
    }
    try {
      schemeColors[RenderColor::BACKGROUND_STOP_COLOR] = readColor(colors, "background-stop");
    } catch (const std::exception&) {
      schemeColors[RenderColor::BACKGROUND_STOP_COLOR] = schemeColors[RenderColor::BACKGROUND_COLOR];
    }
  } catch (const std::exception& e) {
    LOG("Error reading color scheme file: '%1$s': %2$s", path.generic_string().c_str(), e.what());
    _error = e.what();
    _name = "";
    _index = 0;
    _show_in_gui = false;
  }
}

RenderColorScheme::RenderColorScheme(fs::path path, std::string name, int index, ColorScheme colors)
  : _path(std::move(path)),
    _name(std::move(name)),
    _index(index),
    _show_in_gui(true),
    _user(true),
    _color_scheme(std::move(colors))
{
}

bool RenderColorScheme::valid() const
{
  return !_name.empty();
}

const std::string& RenderColorScheme::name() const
{
  return _name;
}

int RenderColorScheme::index() const
{
  return _index;
}

bool RenderColorScheme::showInGui() const
{
  return _show_in_gui;
}

bool RenderColorScheme::isUser() const
{
  return _user;
}

std::string RenderColorScheme::path() const
{
  return _path.string();
}

std::string RenderColorScheme::error() const
{
  return _error;
}

const ColorScheme& RenderColorScheme::colorScheme() const
{
  return _color_scheme;
}

ColorMap *ColorMap::inst(bool erase)
{
  static auto *instance = new ColorMap;
  if (erase) {
    delete instance;
    instance = nullptr;
  }
  return instance;
}

ColorMap::ColorMap() : default_(std::make_shared<const RenderColorScheme>()), active_(default_)
{
  colorSchemeSet.emplace(default_->index(), default_);
  enumerateColorSchemesInPath(PlatformUtils::resourceBasePath(), false);
  enumerateColorSchemesInPath(PlatformUtils::userConfigPath(), true);
  dump();
}

const char *ColorMap::defaultColorSchemeName() const
{
  return DEFAULT_COLOR_SCHEME_NAME;
}

const ColorScheme& ColorMap::defaultColorScheme() const
{
  return default_->colorScheme();
}

std::shared_ptr<const RenderColorScheme> ColorMap::find(const std::string& name) const
{
  for (const auto& item : colorSchemeSet) {
    if (name == item.second->name()) return item.second;
  }
  return nullptr;
}

const ColorScheme *ColorMap::findColorScheme(const std::string& name) const
{
  const std::lock_guard lock(mutex_);
  const auto scheme = find(name);
  return scheme ? &scheme->colorScheme() : nullptr;
}

const ColorScheme& ColorMap::setActiveColorScheme(const std::string& name)
{
  const std::lock_guard lock(mutex_);
  const auto scheme = find(name);
  active_ = scheme ? scheme : default_;
  return active_->colorScheme();
}

const ColorScheme& ColorMap::activeColorScheme() const
{
  const std::lock_guard lock(mutex_);
  return active_->colorScheme();
}

bool ColorMap::isUserColorScheme(const std::string& name) const
{
  const std::lock_guard lock(mutex_);
  const auto scheme = find(name);
  return scheme && scheme->isUser();
}

bool ColorMap::colorSchemeNameTaken(const std::string& name, const std::string& except) const
{
  const std::lock_guard lock(mutex_);
  return nameTaken(colorSchemeSet, name, except);
}

std::string ColorMap::addUserColorScheme(const std::string& name, const ColorScheme& colors)
{
  const std::string config = PlatformUtils::userConfigPath();
  if (config.empty()) return _("There is no user config folder to keep color schemes in.");
  const fs::path dir = fs::path(config) / "color-schemes" / "render";
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    return (boost::format(_("Can't create '%1$s': %2$s")) % dir.generic_string() % ec.message()).str();
  }

  int index;
  {
    const std::lock_guard lock(mutex_);
    if (nameTaken(colorSchemeSet, name, {})) {
      return (boost::format(_("The name '%1$s' is already taken.")) % name).str();
    }
    index = colorSchemeSet.rbegin()->first + 1;
  }
  const fs::path path = userSchemePath(dir, name);
  if (auto error = writeColorScheme(path, name, index, colors); !error.empty()) return error;

  auto scheme = std::make_shared<const RenderColorScheme>(path, name, index, colors);
  const std::lock_guard lock(mutex_);
  colorSchemeSet.emplace(index, std::move(scheme));
  return {};
}

std::string ColorMap::saveUserColorScheme(const std::string& name, const std::string& newName,
                                          const ColorScheme& colors)
{
  std::shared_ptr<const RenderColorScheme> scheme;
  {
    const std::lock_guard lock(mutex_);
    scheme = find(name);
    if (!scheme || !scheme->isUser()) {
      return (boost::format(_("'%1$s' is not a user color scheme.")) % name).str();
    }
    if (nameTaken(colorSchemeSet, newName, name)) {
      return (boost::format(_("The name '%1$s' is already taken.")) % newName).str();
    }
  }
  if (auto error = writeColorScheme(scheme->_path, newName, scheme->index(), colors); !error.empty()) {
    return error;
  }

  auto saved =
    std::make_shared<const RenderColorScheme>(scheme->_path, newName, scheme->index(), colors);
  const std::lock_guard lock(mutex_);
  retire(scheme, saved);
  return {};
}

std::string ColorMap::removeUserColorScheme(const std::string& name)
{
  std::shared_ptr<const RenderColorScheme> scheme;
  {
    const std::lock_guard lock(mutex_);
    scheme = find(name);
    if (!scheme || !scheme->isUser()) {
      return (boost::format(_("'%1$s' is not a user color scheme.")) % name).str();
    }
  }
  std::error_code ec;
  fs::remove(scheme->_path, ec);
  if (ec) {
    const std::string file = scheme->_path.generic_string();
    return (boost::format(_("Can't delete '%1$s': %2$s")) % file % ec.message()).str();
  }

  const std::lock_guard lock(mutex_);
  retire(scheme, nullptr);
  return {};
}

void ColorMap::retire(const std::shared_ptr<const RenderColorScheme>& scheme,
                      const std::shared_ptr<const RenderColorScheme>& replacement)
{
  for (auto it = colorSchemeSet.begin(); it != colorSchemeSet.end(); ++it) {
    if (it->second != scheme) continue;
    if (replacement) {
      it->second = replacement;
    } else {
      colorSchemeSet.erase(it);
    }
    break;
  }
  if (active_ == scheme) active_ = replacement ? replacement : default_;
  retired_.push_back(scheme);
}

void ColorMap::dump() const
{
  PRINTD("Listing available color schemes...");

  std::list<std::string> names = colorSchemeNames();
  unsigned int length = 0;
  for (const auto& name : names) {
    length = name.length() > length ? name.length() : length;
  }

  for (const auto& item : colorSchemeSet) {
    const RenderColorScheme *cs = item.second.get();
    const char gui = cs->showInGui() ? 'G' : '-';
    if (cs->path().empty()) {
      PRINTDB("%6d:%c: %s (built-in)",
              cs->index() % gui % boost::io::group(std::setw(length), cs->name()));
    } else {
      PRINTDB("%6d:%c: %s from %s",
              cs->index() % gui % boost::io::group(std::setw(length), cs->name()) % cs->path());
    }
  }
  PRINTD("done.");
}

std::list<std::string> ColorMap::colorSchemeNames(bool guiOnly) const
{
  const std::lock_guard lock(mutex_);
  std::list<std::string> colorSchemeNames;
  for (const auto& item : colorSchemeSet) {
    const RenderColorScheme *scheme = item.second.get();
    if (guiOnly && !scheme->showInGui()) {
      continue;
    }
    colorSchemeNames.push_back(scheme->name());
  }
  return colorSchemeNames;
}

Color4f ColorMap::getColor(const ColorScheme& cs, const RenderColor rc)
{
  if (cs.colors.count(rc)) return cs.colors.at(rc);
  const RenderColors& defaults = ColorMap::inst()->defaultColorScheme().colors;
  if (defaults.count(rc)) return defaults.at(rc);
  return {0, 0, 0, 127};
}

void ColorMap::enumerateColorSchemesInPath(const fs::path& basePath, bool user)
{
  if (basePath.empty()) return;
  const fs::path color_schemes = basePath / "color-schemes" / "render";

  PRINTDB("Enumerating color schemes from '%s'", color_schemes.generic_string().c_str());

  fs::directory_iterator end_iter;

  if (fs::exists(color_schemes) && fs::is_directory(color_schemes)) {
    for (fs::directory_iterator dir_iter(color_schemes); dir_iter != end_iter; ++dir_iter) {
      if (!fs::is_regular_file(dir_iter->status())) {
        continue;
      }

      const fs::path path = (*dir_iter).path();
      if (!(path.extension() == ".json")) {
        continue;
      }

      auto colorScheme = std::make_shared<const RenderColorScheme>(path, user);
      if (!colorScheme->valid()) {
        PRINTDB("Invalid file '%s': %s", colorScheme->path() % colorScheme->error());
      } else if (nameTaken(colorSchemeSet, colorScheme->name(), {})) {
        LOG(message_group::Warning, "Color scheme '%1$s' in '%2$s' is skipped: the name is taken.",
            colorScheme->name(), colorScheme->path());
      } else {
        colorSchemeSet.emplace(colorScheme->index(), colorScheme);
        PRINTDB("Found file '%s' with color scheme '%s' and index %d",
                colorScheme->path() % colorScheme->name() % colorScheme->index());
      }
    }
  }
}
