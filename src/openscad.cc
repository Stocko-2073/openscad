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

#include "openscad.h"

#include "version.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>

#include <cassert>
#endif
#include <libintl.h>

#include <array>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/lexical_cast/bad_lexical_cast.hpp>
#include <boost/optional/optional.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/positional_options.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/iterator_range_core.hpp>
#include <clocale>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <istream>
#include <iterator>
#include <map>
#include <memory>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef ENABLE_CGAL
#include <CGAL/assertions.h>
#include <CGAL/assertions_behaviour.h>
#endif

#include "Feature.h"
#include "LibraryInfo.h"
#include "RenderStatistic.h"
#include "core/AST.h"
#include "core/BOSL2Library.h"
#include "core/BuiltinContext.h"
#include "core/Builtins.h"
#include "core/Context.h"
#include "core/EvaluationSession.h"
#include "core/ModifierOverlays.h"
#include "core/RenderVariables.h"
#include "core/ScopeContext.h"
#include "core/ScriptProfile.h"
#include "core/Settings.h"
#include "core/customizer/CommentParser.h"
#include "core/customizer/ParameterObject.h"
#include "core/customizer/ParameterSet.h"
#include "core/node.h"
#include "core/parsersettings.h"
#include "geometry/Geometry.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/GeometryUtils.h"
#include "geometry/PolySet.h"
#ifdef ENABLE_MANIFOLD
#include "geometry/InterferenceCheck.h"
#endif
#include "glview/Camera.h"
#include "glview/ColorMap.h"
#include "glview/OffscreenView.h"
#include "glview/RenderSettings.h"
#include "handle_dep.h"
#include "io/export.h"
#include "memo_replay.h"
#include "openscad_gui.h"
#include "openscad_mimalloc.h"
#include "platform/PlatformUtils.h"
#include "utils/StackCheck.h"
#include "utils/exceptions.h"
#include "utils/printutils.h"

#ifdef ENABLE_PYTHON
#include "python/python_public.h"
#endif

namespace po = boost::program_options;
namespace fs = std::filesystem;

std::string commandline_commands;
std::string arg_colorscheme;

namespace {

bool arg_info = false;

}  // namespace

class Echostream
{
public:
  Echostream(std::ostream& stream) : stream(stream)
  {
    set_output_handler(&Echostream::output, nullptr, this);
  }
  Echostream(const std::string& filename) : fstream(std::filesystem::u8path(filename)), stream(fstream)
  {
    set_output_handler(&Echostream::output, nullptr, this);
  }
  static void output(const Message& msgObj, void *userdata)
  {
    auto self = static_cast<Echostream *>(userdata);
    if (msgObj.group != message_group::HtmlLink) {
      self->stream << msgObj.str() << "\n";
    }
  }
  ~Echostream()
  {
    if (fstream.is_open()) fstream.close();
  }

private:
  std::ofstream fstream;
  std::ostream& stream;
};

struct AnimateArgs {
  unsigned frames = 0;
  unsigned num_shards = 1;
  unsigned shard = 1;
};

struct CommandLine {
  const bool is_stdin;
  const std::string& filename;
  const bool is_stdout;
  std::string output_file;
  const fs::path& original_path;
  const std::string& parameterFile;
  const std::string& setName;
  const ViewOptions& viewOptions;
  const Camera& camera;
  const boost::optional<FileFormat> export_format;
  const CmdLineExportOptions& exportOptions;
  const AnimateArgs animate;
  const std::vector<std::string> summaryOptions;
  const std::string summaryFile;
  const bool interferenceCheck;
  const std::string interferenceFile;  // "-" means stdout
};

namespace {

#ifndef OPENSCAD_NOGUI
bool useGUI()
{
#ifdef Q_OS_X11
  // see <http://qt.nokia.com/doc/4.5/qapplication.html#QApplication-2>:
  // On X11, the window system is initialized if GUIenabled is true. If GUIenabled
  // is false, the application does not connect to the X server. On Windows and
  // Macintosh, currently the window system is always initialized, regardless of the
  // value of GUIenabled. This may change in future versions of Qt.
  return getenv("DISPLAY") != 0;
#else
  return true;
#endif
}
#endif  // OPENSCAD_NOGUI

bool checkAndExport(const std::shared_ptr<const Geometry>& root_geom, unsigned dimensions,
                    ExportInfo& exportInfo, const bool is_stdout, const std::string& filename)
{
  if (root_geom->getDimension() != dimensions) {
    LOG("Current top level object is not a %1$dD object.", dimensions);
    return false;
  }
  if (root_geom->isEmpty()) {
    LOG("Current top level object is empty.");
    return false;
  }

  if (is_stdout) {
    exportFileStdOut(root_geom, exportInfo);
  } else {
    exportFileByName(root_geom, filename, exportInfo);
  }
  return true;
}

void help(const char *arg0, const po::options_description& desc, bool failure = false)
{
  const fs::path progpath(arg0);
  LOG("Usage: %1$s [options] file.scad\n%2$s", progpath.filename().string(), desc);
  exit(failure ? 1 : 0);
}

template <std::size_t size>
void help_export(const std::array<const Settings::SettingsEntryBase *, size>& options)
{
  LOG("Section '%1$s':", options.at(0)->category());

  for (const auto option : options) {
    const auto [type, values] = option->help();
    LOG("  - %1$s (%2$s): %3$s", option->name(), type, values);
  }
}

void help_export()
{
  LOG("OpenSCAD version %1$s\n", openscad_versionnumber);
  LOG("List of settings that can be given using the -O option using the");
  LOG("format '<section>/<key>=value', e.g.:");
  LOG("openscad -O export-pdf/paper-size=a6 -O export-pdf/show-grid=false\n");
  help_export(Settings::SettingsExportPdf::cmdline);
  help_export(Settings::SettingsExport3mf::cmdline);
  help_export(Settings::SettingsExportSvg::cmdline);
  exit(0);
}

void version()
{
  LOG("OpenSCAD version %1$s", openscad_versionnumber);
  exit(0);
}

int info()
{
  std::cout << LibraryInfo::info() << "\n\n";

  try {
    OffscreenView const glview(512, 512);
    std::cout << glview.getRendererInfo() << "\n";
  } catch (const OffscreenViewException& ex) {
    LOG("Can't create OpenGL OffscreenView: %1$s. Exiting.\n", ex.what());
    return 1;
  }

  return 0;
}

template <typename F>
bool with_output(const bool is_stdout, const std::string& filename, const F& f,
                 std::ios::openmode mode = std::ios::out)
{
  if (is_stdout) {
#ifdef _WIN32
    if ((mode & std::ios::binary) != 0) {
      _setmode(_fileno(stdout), _O_BINARY);
    }
#endif
    f(std::cout);
    return true;
  }
  std::ofstream fstream(std::filesystem::u8path(filename), mode);
  if (!fstream.is_open()) {
    LOG("Can't open file \"%1$s\" for export", filename);
    return false;
  } else {
    f(fstream);
    return true;
  }
}

AnimateArgs get_animate(const po::variables_map& vm)
{
  AnimateArgs animate;
  if (vm.count("animate")) {
    animate.frames = vm["animate"].as<unsigned>();
  }
  if (vm.count("animate_sharding")) {
    std::vector<std::string> strs;
    boost::split(strs, vm["animate_sharding"].as<std::string>(), boost::is_any_of("/"));
    if (strs.size() != 2) {
      LOG("--animate_sharding requires <shard>/<num_shards>");
      exit(1);
    }
    try {
      animate.shard = boost::lexical_cast<unsigned>(strs[0]);
      animate.num_shards = boost::lexical_cast<unsigned>(strs[1]);
    } catch (const boost::bad_lexical_cast&) {
      LOG("--animate_sharding parameters need to be positive integers");
      exit(1);
    }
    if (animate.shard > animate.num_shards || animate.shard == 0) {
      LOG("--animate_sharding: shard needs to be in range <1..num_shards>");
      exit(1);
    }
  }
  return animate;
}

Camera get_camera(const po::variables_map& vm)
{
  Camera camera;

  if (vm.count("camera")) {
    std::vector<std::string> strs;
    std::vector<double> cam_parameters;
    boost::split(strs, vm["camera"].as<std::string>(), boost::is_any_of(","));
    if (strs.size() == 6 || strs.size() == 7) {
      try {
        for (const auto& s : strs) {
          cam_parameters.push_back(boost::lexical_cast<double>(s));
        }
        camera.setup(cam_parameters);
      } catch (boost::bad_lexical_cast&) {
        LOG("Camera setup requires numbers as parameters");
      }
    } else {
      LOG("Camera setup requires either 7 numbers for Gimbal Camera or 6 numbers for Vector Camera");
      exit(1);
    }
  } else {
    camera.viewall = true;
    camera.autocenter = true;
  }

  if (vm.count("viewall")) {
    camera.viewall = true;
  }

  if (vm.count("autocenter")) {
    camera.autocenter = true;
  }

  if (vm.count("projection")) {
    auto proj = vm["projection"].as<std::string>();
    if (proj == "o" || proj == "ortho" || proj == "orthogonal") {
      camera.projection = Camera::ProjectionType::ORTHOGONAL;
    } else if (proj == "p" || proj == "perspective") {
      camera.projection = Camera::ProjectionType::PERSPECTIVE;
    } else {
      LOG("projection needs to be 'o' or 'p' for ortho or perspective\n");
      exit(1);
    }
  }

  if (vm.count("imgsize")) {
    std::vector<std::string> strs;
    boost::split(strs, vm["imgsize"].as<std::string>(), boost::is_any_of(","));
    if (strs.size() != 2) {
      LOG("Need 2 numbers for imgsize");
      exit(1);
    } else {
      try {
        int const w = boost::lexical_cast<int>(strs[0]);
        int const h = boost::lexical_cast<int>(strs[1]);
        camera.pixel_width = w;
        camera.pixel_height = h;
      } catch (boost::bad_lexical_cast&) {
        LOG("Need 2 numbers for imgsize");
      }
    }
  }

  return camera;
}

int do_export(const CommandLine& cmd, const RenderVariables& render_variables, FileFormat export_format,
              SourceFile *root_file, RenderStatistic& renderStatistic)
{
  auto filename_str = fs::path(cmd.output_file).generic_string();
  // Avoid possibility of fs::absolute throwing when passed an empty path
  auto fpath = cmd.filename.empty() ? fs::current_path() : fs::absolute(fs::path(cmd.filename));
  auto fparent = fpath.parent_path();

  // set CWD relative to source file
  fs::current_path(fparent);

  EvaluationSession session{fparent.string()};
  ContextHandle<BuiltinContext> builtin_context{Context::create<BuiltinContext>(&session)};
  render_variables.applyToContext(builtin_context);

#ifdef DEBUG
  PRINTDB("BuiltinContext:\n%s", builtin_context->dump());
#endif

  AbstractNode::resetIndexCounter();
  std::shared_ptr<const FileContext> file_context;
  std::shared_ptr<AbstractNode> absolute_root_node;

  {
    const RenderStatistic::ScopedPhase phase(renderStatistic, RenderStatistic::PHASE_EVALUATION);
#ifdef ENABLE_PYTHON
    if (python_result_node != NULL && python_active) {
      absolute_root_node = python_result_node;
    } else {
#endif
      absolute_root_node = root_file->instantiate(*builtin_context, &file_context);
#ifdef ENABLE_PYTHON
    }
#endif
  }

  Camera camera = cmd.camera;
  if (file_context) {
    camera.updateView(file_context, true);
  }

  // restore CWD after module instantiation finished
  fs::current_path(cmd.original_path);

  // Do we have an explicit root node (! modifier)?
  std::shared_ptr<const AbstractNode> root_node;
  // Declared out here only so the single printAll below can see it; the
  // non-geometry export formats leave it null.
  std::shared_ptr<const Geometry> root_geom;
  bool evaluated_geometry = false;
  const Location *nextLocation = nullptr;
  if (!(root_node = find_root_tag(absolute_root_node, &nextLocation))) {
    root_node = absolute_root_node;
  }
  if (nextLocation) {
    LOG(message_group::Warning, *nextLocation, builtin_context->documentRoot(),
        "More than one Root Modifier (!)");
  }
  Tree tree(root_node, fparent.string());

#ifdef ENABLE_MANIFOLD
  // Runs ahead of the export so the report is written for every format, and so
  // it still lands on disk when --hardwarnings turns the first Warning below
  // into an exception. The leaf geometry it evaluates stays cached for the
  // export that follows.
  if (cmd.interferenceCheck) {
    const RenderStatistic::ScopedPhase phase(renderStatistic, RenderStatistic::PHASE_INTERFERENCE);
    interference::Options opts;
    // Same expression the parser uses for Location::fileName(), so chain steps
    // from the main file compare equal.
    opts.currentFile = fpath.generic_string();
    const interference::Report report = interference::run(tree, opts);
    const bool toStdout = cmd.interferenceFile == "-";
    if (!with_output(toStdout, cmd.interferenceFile, [&](std::ostream& stream) {
          interference::writeJson(report, tree, opts, stream);
        })) {
      return 1;
    }
    interference::logReport(report, tree);
  }
#endif

  if (export_format == FileFormat::CSG) {
    // https://github.com/openscad/openscad/issues/128
    // When I use the csg ouptput from the command line the paths in 'import'
    // statements become relative. But unfortunately they become relative to
    // the current working dir and neither to the location of the input nor
    // the output.
    fs::current_path(fparent);  // Force exported filenames to be relative to document path
    with_output(cmd.is_stdout, filename_str, [&tree, root_node](std::ostream& stream) {
      stream << tree.getString(*root_node, "\t") << "\n";
    });
    fs::current_path(cmd.original_path);
  } else if (export_format == FileFormat::AST) {
    fs::current_path(fparent);  // Force exported filenames to be relative to document path
    with_output(cmd.is_stdout, filename_str,
                [root_file](std::ostream& stream) { stream << root_file->dump(""); });
    fs::current_path(cmd.original_path);
  } else if (export_format == FileFormat::PARAM) {
    with_output(cmd.is_stdout, filename_str,
                [&root_file, &fpath](std::ostream& stream) { export_param(root_file, fpath, stream); });
  } else if (export_format == FileFormat::ECHO) {
    // echo -> don't need to evaluate any geometry
  } else {
    evaluated_geometry = true;
    GeometryEvaluator geomevaluator(tree);
    const RenderStatistic::ScopedPhase geometryPhase(renderStatistic,
                                                     RenderStatistic::PHASE_GEOMETRY);
    // FIXME: Consider adding MANIFOLD as a valid --render argument and ViewOption, to be able to
    // distinguish from CGAL
    constexpr bool allownef = true;
    root_geom = geomevaluator.evaluateGeometry(*tree.root(), allownef);
    if (!root_geom) root_geom = std::make_shared<PolySet>(3);
    // Force creation of concrete geometry (mostly for testing)
    if (cmd.viewOptions.renderer == RenderType::BACKEND_SPECIFIC && root_geom->getDimension() == 3) {
      if (auto geomlist = std::dynamic_pointer_cast<const GeometryList>(root_geom)) {
        auto flatlist = geomlist->flatten();
        for (auto& child : flatlist) {
          if (child.second->getDimension() == 3) {
            child.second = GeometryUtils::getBackendSpecificGeometry(child.second);
          }
        }
        root_geom = std::make_shared<GeometryList>(flatlist);
      } else {
        root_geom = GeometryUtils::getBackendSpecificGeometry(root_geom);
        assert(root_geom != nullptr);
      }
      LOG("Converted to backend-specific geometry");
    }
    // A picture shows the # and % subtrees over the result, as the 3D view does.
    std::vector<overlay::Mesh> overlays;
    if (export_format == FileFormat::PNG) overlays = overlay::collect(tree, *tree.root());
    renderStatistic.endPhase(RenderStatistic::PHASE_GEOMETRY);

    const std::string input_filename = cmd.is_stdin ? "<stdin>" : cmd.filename;
    const int dim = fileformat::is3D(export_format) ? 3 : fileformat::is2D(export_format) ? 2 : 0;
    ExportInfo exportInfo = createExportInfo(export_format, fileformat::info(export_format),
                                             input_filename, &cmd.camera, cmd.exportOptions);
    const RenderStatistic::ScopedPhase exportPhase(renderStatistic, RenderStatistic::PHASE_EXPORT);
    if (dim > 0 && !checkAndExport(root_geom, dim, exportInfo, cmd.is_stdout, filename_str)) {
      return 1;
    }

    if (export_format == FileFormat::PNG) {
      bool success = true;
      bool const wrote = with_output(
        cmd.is_stdout, filename_str,
        [&success, &root_geom, &overlays, &cmd, &camera](std::ostream& stream) {
          success = export_png(root_geom, overlays, cmd.viewOptions, camera, stream);
        },
        std::ios::out | std::ios::binary);
      if (!success || !wrote) {
        return 1;
      }
    }
    renderStatistic.endPhase(RenderStatistic::PHASE_EXPORT);
  }

  /*
   * Outside the export_format chain above, so that the formats which need no
   * geometry -- echo, csg, ast, param -- can report their phase times
   * too: `--summary time -o x.echo` used to print nothing at all, and the echo
   * path is the cheapest way to time script evaluation on its own, with no
   * geometry stage and no export write to add noise. printAll already tolerates
   * a null geometry.
   *
   * But only when a summary was actually asked for. With no --summary and no
   * --summary-file, printAll still logs the cache statistics and the rendering
   * time, which for a geometry export is the familiar default output and for an
   * echo export would be new console noise -- and the echo regression tests
   * compare the console output verbatim.
   */
  if (evaluated_geometry || !cmd.summaryOptions.empty() || !cmd.summaryFile.empty()) {
    renderStatistic.printAll(root_geom, camera, cmd.summaryOptions, cmd.summaryFile);
  }
  return 0;
}

int cmdline(const CommandLine& cmd)
{
  FileFormat export_format;

  // Determine output file format and assign it to formatName
  if (cmd.export_format.is_initialized()) {
    export_format = cmd.export_format.get();
  } else {
    // else extract format from file extension
    const auto path = fs::path(cmd.output_file);
    std::string suffix = path.has_extension() ? path.extension().generic_string().substr(1) : "";
    boost::algorithm::to_lower(suffix);

    if (!fileformat::fromIdentifier(suffix, export_format)) {
      LOG(
        "Invalid suffix %1$s. Either add a valid suffix or specify one using the --export-format "
        "option.",
        suffix);
      return 1;
    }
  }

  if (cmd.interferenceCheck && cmd.interferenceFile == "-" &&
      (cmd.is_stdout || cmd.summaryFile == "-")) {
    LOG("--interference-check writes its JSON report to stdout, which '-o -' or '--summary-file -' "
        "already uses. Pass --interference-file <path> instead.");
    return 1;
  }

  // Do some minimal checking of output directory before rendering (issue #432)
  auto output_dir = fs::path(cmd.output_file).parent_path();
  if (output_dir.empty()) {
    // If output_file_str has no directory prefix, set output directory to current directory.
    output_dir = fs::current_path();
  }
  if (!fs::is_directory(output_dir)) {
    LOG("\n'%1$s' is not a directory for output file %2$s - Skipping\n", output_dir.generic_string(),
        cmd.output_file);
    return 1;
  }

  set_render_color_scheme(arg_colorscheme, true);

  std::shared_ptr<Echostream> echostream;
  if (export_format == FileFormat::ECHO) {
    echostream.reset(cmd.is_stdout ? new Echostream(std::cout) : new Echostream(cmd.output_file));
  }

  std::string text;
  if (cmd.is_stdin) {
    text = std::string((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  } else {
    std::ifstream ifs(std::filesystem::u8path(cmd.filename));
    if (!ifs.is_open()) {
      LOG("Can't open input file '%1$s'!\n", cmd.filename);
      return 1;
    }
    handle_dep(cmd.filename);
    text = std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  }

#ifdef ENABLE_PYTHON
  python_active = false;
  if (cmd.filename.c_str() != NULL) {
    if (boost::algorithm::ends_with(cmd.filename, ".py")) {
      if (python_trusted == true) python_active = true;
      else LOG("Python is not enabled");
    }
  }

  if (python_active) {
    auto fulltext_py = text;
    initPython("", 0.0);
    auto error = evaluatePython(fulltext_py, false);
    if (error.size() > 0) LOG(error.c_str());
    text = "\n";
  }
#endif  // ifdef ENABLE_PYTHON
  text += "\n\x03\n" + commandline_commands;

  // Spans parsing through export, so the itemized phases add up to the total.
  RenderStatistic renderStatistic;

  SourceFile *root_file = nullptr;
  {
    const RenderStatistic::ScopedPhase phase(renderStatistic, RenderStatistic::PHASE_PARSING);
    if (!parse(root_file, text, cmd.filename, cmd.filename, false)) {
      delete root_file;  // parse failed
      root_file = nullptr;
    }
  }
  if (!root_file) {
    LOG("Can't parse file '%1$s'!\n", cmd.filename);
    return 1;
  }

  // add parameter to AST
  CommentParser::collectParameters(text.c_str(), root_file);
  if (!cmd.parameterFile.empty() && !cmd.setName.empty()) {
    ParameterObjects parameters = ParameterObjects::fromSourceFile(root_file);
    ParameterSets sets;
    sets.readFile(cmd.parameterFile);
    for (const auto& set : sets) {
      if (set.name() == cmd.setName) {
        parameters.importValues(set);
        parameters.apply(root_file);
        break;
      }
    }
  }

  root_file->handleDependencies();

  RenderVariables render_variables = {
    .camera = cmd.camera,
  };

  if (cmd.animate.frames == 0) {
    render_variables.time = 0;
    return do_export(cmd, render_variables, export_format, root_file, renderStatistic);
  } else {
    // export the requested number of animated frames
    const unsigned start_frame = ((cmd.animate.shard - 1) * cmd.animate.frames) / cmd.animate.num_shards;
    const unsigned limit_frame = (cmd.animate.shard * cmd.animate.frames) / cmd.animate.num_shards;
    for (unsigned frame = start_frame; frame < limit_frame; ++frame) {
      render_variables.time = frame * (1.0 / cmd.animate.frames);

      std::ostringstream oss;
      oss << std::setw(5) << std::setfill('0') << frame;

      auto frame_file = fs::path(cmd.output_file);
      auto extension = frame_file.extension();
      frame_file.replace_extension();
      frame_file += oss.str();
      frame_file.replace_extension(extension);
      std::string const frame_str = frame_file.generic_string();

      LOG("Exporting %1$s...", cmd.filename);

      CommandLine frame_cmd = cmd;
      frame_cmd.output_file = frame_str;

      // Each frame is reported separately; parsing happened once, before the loop.
      renderStatistic.start();
      int const r = do_export(frame_cmd, render_variables, export_format, root_file, renderStatistic);
      if (r != 0) {
        return r;
      }
    }

    return 0;
  }
}

template <class Seq, typename ToString>
static std::string str_join(const Seq& seq, const std::string& sep, const ToString& toString)
{
  return boost::algorithm::join(boost::adaptors::transform(seq, toString), sep);
}

static bool flagConvert(const std::string& str)
{
  if (str == "1" || boost::iequals(str, "on") || boost::iequals(str, "true")) {
    return true;
  }
  if (str == "0" || boost::iequals(str, "off") || boost::iequals(str, "false")) {
    return false;
  }
  throw std::runtime_error("");
  return false;
}

static std::tuple<std::string, std::string> simple_split(const std::string& str, const char c)
{
  const auto idx = str.find_first_of(c);
  if (idx == std::string::npos) return {};
  const auto first = str.substr(0, idx);
  const auto second = str.substr(idx + 1);
  return {first, second};
}

static CmdLineExportOptions convert_export_options(const po::variables_map& vm)
{
  if (vm.count("O") == 0) {
    return {};
  }

  CmdLineExportOptions map;
  const auto& options = vm["O"].as<std::vector<std::string>>();
  for (const auto& option : options) {
    const auto [key, value] = simple_split(option, '=');
    const auto [section, name] = simple_split(key, '/');
    map[section][name] = value;
  }
  return map;
}

}  // namespace

void set_render_color_scheme(const std::string& color_scheme, const bool exit_if_not_found)
{
  if (color_scheme.empty()) {
    return;
  }

  if (ColorMap::inst()->findColorScheme(color_scheme)) {
    RenderSettings::inst()->colorscheme = color_scheme;
    return;
  }

  if (exit_if_not_found) {
    LOG((boost::algorithm::join(ColorMap::inst()->colorSchemeNames(), "\n")));

    exit(1);
  } else {
    LOG("Unknown color scheme '%1$s', using default '%2$s'.", arg_colorscheme,
        ColorMap::inst()->defaultColorSchemeName());
  }
}

/**
 * Initialize gettext. This must be called after the application path was
 * determined so we can lookup the resource path for the language translation
 * files.
 */
void localization_init()
{
  fs::path const po_dir(PlatformUtils::resourcePath("locale"));
  const std::string& locale_path(po_dir.string());

  if (fs::is_directory(locale_path)) {
    setlocale(LC_ALL, "");
    bindtextdomain("openscad", locale_path.c_str());
    bind_textdomain_codeset("openscad", "UTF-8");
    textdomain("openscad");
  } else {
    LOG("Could not initialize localization (application path is '%1$s').",
        PlatformUtils::applicationPath());
  }
}

#ifdef Q_OS_MACOS
std::pair<std::string, std::string> customSyntax(const std::string& s)
{
  if (s.find("-psn_") == 0) return {"psn", s.substr(5)};
#else
std::pair<std::string, std::string> customSyntax(const std::string&)
{
#endif

  return {};
}
/*!
   This makes boost::program_option parse comma-separated values
 */
struct CommaSeparatedVector {
  std::vector<std::string> values;

  friend std::istream& operator>>(std::istream& in, CommaSeparatedVector& value)
  {
    std::string token;
    in >> token;
    // NOLINTNEXTLINE(*NewDeleteLeaks) LLVM bug https://github.com/llvm/llvm-project/issues/40486
    boost::split(value.values, token, boost::is_any_of(","));
    return in;
  }
};

// OpenSCAD
int openscad_main(int argc, char **argv)
{
#if defined(ENABLE_CGAL) && defined(USE_MIMALLOC)
  // call init_mimalloc before any GMP variables are initialized. (defined in src/openscad_mimalloc.h)
  init_mimalloc();
#endif

  int rc = 0;
  StackCheck::inst();

#ifdef Q_OS_MACOS
  bool isGuiLaunched = getenv("GUI_LAUNCHED") != nullptr;
  auto nslog = [](const Message& msg, void *userdata) { CocoaUtils::nslog(msg.msg, userdata); };
  if (isGuiLaunched) set_output_handler(nslog, nullptr, nullptr);
#else
  PlatformUtils::ensureStdIO();
#endif

#ifndef __EMSCRIPTEN__
  const auto applicationPath =
    weakly_canonical(boost::dll::program_location()).parent_path().generic_string();
#else
  const auto applicationPath = boost::dll::fs::current_path();
#endif
  PlatformUtils::registerApplicationPath(applicationPath);
  // Before the banner, which reports the BOSL2 that include <BOSL2/...> resolves to.
  parser_init();

  // Launch banner for every invocation (GUI or command line). Written straight
  // to stderr rather than through LOG() so it never lands in an .echo export,
  // is not silenced by --quiet, and cannot mix into a report on stdout.
  std::cerr << "OpenSCAD for AI Agents, by Stocko.  See --help for additional AI friendly tools\n"
            << BOSL2Library::describe() << std::endl;

#ifdef ENABLE_PYTHON
  // The original name as called, not resolving links and so on. This will
  // just forward everything to the python main.
  const auto applicationName = fs::path(argv[0]).filename().generic_string();
  if (applicationName == "python" || applicationName == "python3" ||
      applicationName.rfind("python3.", 0) == 0 || applicationName == "openscad-python") {
    return pythonRunArgs(argc, argv);
  }
#endif

#ifdef ENABLE_CGAL
  // Always throw exceptions from CGAL, so we can catch instead of crashing on bad geometry.
  CGAL::set_error_behaviour(CGAL::THROW_EXCEPTION);
  CGAL::set_warning_behaviour(CGAL::THROW_EXCEPTION);
#endif
  Builtins::instance()->initialize();

  auto original_path = fs::current_path();

  std::vector<std::string> output_files;
  const char *deps_output_file = nullptr;
  boost::optional<FileFormat> export_format;

  ViewOptions viewOptions{};
  po::options_description desc("Allowed options");
  // clang-format off
  desc.add_options()
    ("export-format", po::value<std::string>(),
      "overrides format of exported scad file when using option '-o', arg can be any of its supported "
      "file extensions.  For ASCII stl export, specify 'asciistl', and for binary stl export, specify "
      "'binstl'.  ASCII export is the current stl default, but binary stl is planned as the future "
      "default so asciistl should be explicitly specified in scripts when needed.\n")
    ("o,o", po::value<std::vector<std::string>>(),
      "output specified file instead of running the GUI. The file extension specifies the type: stl, "
      "off, wrl, amf, 3mf, csg, dxf, svg, pdf, png, echo, ast, nef3, nefdbg, param, pov. May be "
      "used multiple times for different exports. Use '-' for stdout.\n")
    ("O,O", po::value<std::vector<std::string>>(),
      "pass settings value to the file export using the format section/key=value, e.g "
      "export-pdf/paper-size=a3. Use --help-export to list all available settings.")
    ("D,D", po::value<std::vector<std::string>>(), "var=val -pre-define variables")
    ("p,p", po::value<std::string>(), "customizer parameter file")
    ("P,P", po::value<std::string>(), "customizer parameter set")
#ifdef ENABLE_EXPERIMENTAL
    ("enable", po::value<std::vector<std::string>>(),
      ("enable experimental features (specify 'all' for enabling all available features): " +
      str_join(boost::make_iterator_range(Feature::begin(), Feature::end()), " | ",
               [](const Feature *feature) { return feature->get_name(); }) +
      "\n")
      .c_str())
#endif
    ("help,h", "print this help message and exit")
    ("help-export", "print list of export parameters and values that can be set via -O")
    ("version,v", "print the version")
    ("info", "print information about the build process\n")
    ("camera", po::value<std::string>(),
      "camera parameters when exporting png: =translate_x,y,z,rot_x,y,z,dist or "
      "=eye_x,y,z,center_x,y,z")("autocenter", "adjust camera to look at object's center")
    ("viewall", "adjust camera to fit object")
    ("backend", po::value<std::string>(),
      "3D rendering backend to use: 'CGAL' (old/slow) or 'Manifold' (new/fast) [default]")
    ("imgsize", po::value<std::string>(), "=width,height of exported png")
    ("render", po::value<std::string>()->implicit_value(""),
      "[=force] -convert the result to the 3D backend's own geometry before exporting png "
      "(a png is always rendered)")
    ("animate", po::value<unsigned>(), "export N animated frames")
    ("animate_sharding", po::value<std::string>(),
      "Parameter <shard>/<num_shards> - Divide work into <num_shards> and only output frames for "
      "<shard>. E.g. 2/5 only outputs the second 1/5 of frames. Use to parallelize work on multiple "
      "cores or machines.")
    ("view", po::value<CommaSeparatedVector>(),
      ("=view options: " + boost::algorithm::join(viewOptions.names(), " | ")).c_str())
    ("projection", po::value<std::string>(), "=(o)rtho or (p)erspective when exporting png")
    ("summary", po::value<std::vector<std::string>>(),
      "enable additional render summary and statistics: all | cache | time | camera | geometry | "
      "bounding-box | area")
    ("summary-file", po::value<std::string>(),
      "output summary information in JSON format to the given file, using '-' outputs to stdout")
    ("colorscheme", po::value<std::string>(),
          ("=colorscheme: " +
           str_join(ColorMap::inst()->colorSchemeNames(), " | ",
                    [](const std::string& colorScheme) {
                      return (colorScheme == ColorMap::inst()->defaultColorSchemeName() ? "*" : "") +
                             colorScheme;
                    }) +
           "\n")
            .c_str())
    ("d,d", po::value<std::string>(), "deps_file -generate a dependency file for make")
    ("m,m", po::value<std::string>(), "make_cmd -runs make_cmd file if file is missing")
    ("quiet,q", "quiet mode (don't print anything *except* errors)")
    ("reset-window-settings", "Reset GUI settings for window placement and fonts.")
    ("hardwarnings", "Stop on the first warning")
    ("trace-depth", po::value<unsigned int>(), "=n, maximum number of trace messages")
    ("trace-usermodule-parameters", po::value<std::string>(),
      "=true/false, configure the output of user module parameters in a trace")
    ("check-parameters", po::value<std::string>(),
      "=true/false, configure the parameter check for user modules and functions")
    ("check-parameter-ranges", po::value<std::string>(),
      "=true/false, configure the parameter range check for builtin modules")
    ("debug", po::value<std::string>(),
      "special debug info - specify 'all' or a set of source file names")
    ("profile", "count script-level function calls, module instantiations and loop iterations "
      "per source location, and report them after evaluation")
    ("profile-file", po::value<std::string>(),
      "write the full per-location profile to the given file as TSV (implies --profile)")
    ("memo-replay", po::value<std::vector<std::string>>()->multitoken(),
      "incremental evaluation harness: evaluate the given .scad files in order with one memo "
      "table, as a series of saves would, and report the reuse at each step; FILE@T evaluates "
      "FILE at $t = T, as an animation frame")
    ("memo-verify", "with --memo-replay, also evaluate each step from scratch and compare the "
      "node trees and messages; exit nonzero on any difference")
    ("memo-geometry", "with --memo-replay, also time each step's geometry evaluation, keeping the "
      "geometry caches across steps as the GUI does")
    ("memo-keep", po::value<int>(),
      "with --memo-replay, evict after each step what the last N steps did not use, as the GUI "
      "does after each render (by default nothing is evicted)")
    ("memo-selftest", po::value<std::string>(),
      "evaluate a .scad file twice with incremental evaluation and compare each run with a "
      "fresh evaluation (--memo-replay FILE FILE --memo-verify)")
    ("interference-check",
      "AI-agent tool: detect parts that overlap (interfere) and report exactly which source "
      "lines produced the overlapping material. Runs alongside any export, e.g.\n"
      "  openscad model.scad --interference-check -o model.stl\n"
      "How it works: every top-level object in the file (each direct child of the root, "
      "after the ! modifier) is one 'part', numbered from 1 in source order. Parts whose "
      "bounding boxes touch are intersected exactly (Manifold); a pair interferes when the "
      "shared volume exceeds 1e-5, so flush mating faces do not count. % background parts, "
      "2D objects and empty geometry are skipped and listed with a status.\n"
      "For each interfering pair the report lists the primitives (cube, cylinder, ...) that "
      "actually contribute material to the overlap, with the overlap volume each one adds, "
      "and for each primitive an ancestor chain: the module calls and transforms leading from "
      "the top-level part down to that primitive, outermost first, each with file, line and "
      "column. The chain keeps only steps located in the input file (steps inside libraries "
      "or other files are dropped), so every chain entry is a line the caller can edit.\n"
      "Output: one pretty-printed JSON document on stdout (or see --interference-file). Keys: "
      "summary.has_interference (bool), summary.collisions (count), parts[] "
      "{number,status,name,description,location,bbox}, collisions[] {parts:[a,b], volume, "
      "primitives[] {part,node_index,name,description,location,volume, chain[] "
      "{node_index,name,label,location}}}. location = {file (relative to the input's "
      "directory), path (absolute), line, column, end_line, end_column}, 1-based, or null.\n"
      "Exit status is 0 whether or not collisions are found; non-zero means the run itself "
      "failed. The human-readable WARNING/ECHO lines the GUI shows are still printed to "
      "stderr. Cannot be combined with --animate, or with '-o -' / '--summary-file -' while "
      "the report goes to stdout. Requires the Manifold backend.")
    ("interference-file", po::value<std::string>(),
      "write the --interference-check JSON report to the given file instead of stdout; '-' "
      "means stdout. Implies --interference-check. Use this when stdout is already taken by "
      "'-o -' or '--summary-file -', or when the report should sit next to the exported "
      "model")
#ifdef ENABLE_PYTHON
    ("trust-python", "Trust python")
    ("python-module", po::value<std::string>(), "=module Call pip python module")
#endif
    ;
  // clang-format on

#ifdef ENABLE_GUI_TESTS
  // clang-format off
  desc.add_options()("run-all-gui-tests", "special gui testing mode - run all the tests");
  // clang-format on
#endif

  po::options_description hidden("Hidden options");
  // clang-format off
  hidden.add_options()
#ifdef Q_OS_MACOS
    ("psn", po::value<std::string>(), "process serial number")
#endif
    ("input-file", po::value<std::vector<std::string>>(), "input file");
  // clang-format on

  po::positional_options_description p;
  p.add("input-file", -1);

  po::options_description all_options;
  all_options.add(desc).add(hidden);

  po::variables_map vm;
  try {
    po::store(po::command_line_parser(argc, argv)
                .options(all_options)
                .positional(p)
                .extra_parser(customSyntax)
                .run(),
              vm);
  } catch (const std::exception& e) {  // Catches e.g. unknown options
    LOG("%1$s\n", e.what());
    help(argv[0], desc, true);
  }

  OpenSCAD::debug = "";
  if (vm.count("debug")) {
    OpenSCAD::debug = vm["debug"].as<std::string>();
    LOG("Debug on. --debug=%1$s", OpenSCAD::debug);
  }
#ifdef ENABLE_PYTHON
  if (vm.count("trust-python")) {
    LOG("Python Engine enabled", OpenSCAD::debug);
    python_trusted = true;
  }

  const auto pymod = "python-module";
  if (vm.count(pymod)) {
    PRINTDB("Running Python Module %s", pymod);
    std::vector<std::string> args;
    if (vm.count("input-file")) {
      args = vm["input-file"].as<std::vector<std::string>>();
    }
    return pythonRunModule(applicationPath, vm[pymod].as<std::string>(), args);
  }
#endif  // ifdef ENABLE_PYTHON
  if (vm.count("quiet")) {
    OpenSCAD::quiet = true;
  }

  if (vm.count("hardwarnings")) {
    OpenSCAD::hardwarnings = true;
  }

  if (vm.count("profile-file")) {
    ScriptProfile::reportFile = vm["profile-file"].as<std::string>();
  }
  if (vm.count("profile") || !ScriptProfile::reportFile.empty()) {
    ScriptProfile::enabled = true;
  }

  const bool interferenceCheck = vm.count("interference-check") || vm.count("interference-file");
  const std::string interferenceFile =
    vm.count("interference-file") ? vm["interference-file"].as<std::string>() : "-";
#ifndef ENABLE_MANIFOLD
  if (interferenceCheck) {
    LOG("--interference-check requires the Manifold backend (build with ENABLE_MANIFOLD).");
    return 1;
  }
#endif

  if (vm.count("traceDepth")) {
    OpenSCAD::traceDepth = vm["traceDepth"].as<unsigned int>();
  }
  std::map<std::string, bool *> flags;
  flags.insert(std::make_pair("trace-usermodule-parameters", &OpenSCAD::traceUsermoduleParameters));
  flags.insert(std::make_pair("check-parameters", &OpenSCAD::parameterCheck));
  flags.insert(std::make_pair("check-parameter-ranges", &OpenSCAD::rangeCheck));
  for (const auto& flag : flags) {
    std::string name = flag.first;
    if (vm.count(name)) {
      std::string opt = vm[name].as<std::string>();
      try {
        (*(flag.second) = flagConvert(opt));
      } catch (const std::runtime_error& e) {
        LOG("Could not parse '--%1$s %2$s' as flag", name, opt);
      }
    }
  }

  if (vm.count("help")) help(argv[0], desc);
  if (vm.count("help-export")) help_export();
  if (vm.count("version")) version();
  if (vm.count("info")) arg_info = true;
  if (vm.count("backend")) {
    auto backend_string = vm["backend"].as<std::string>();
    auto backend = renderBackend3DFromString(backend_string);
    if (!backend) {
      LOG(message_group::Error, "Unknown rendering backend '%1$s'.", backend_string.c_str());
      return 1;
    }
    RenderSettings::inst()->backend3D = backend.value();
  }

  if (vm.count("render")) {
    // Note: "cgal" is here for backwards compatibility, can probably be removed soon
    if (vm["render"].as<std::string>() == "cgal" || vm["render"].as<std::string>() == "force") {
      viewOptions.renderer = RenderType::BACKEND_SPECIFIC;
    } else {
      viewOptions.renderer = RenderType::GEOMETRY;
    }
  }

  if (vm.count("view")) {
    const auto& viewOptionValues = vm["view"].as<CommaSeparatedVector>();

    for (const auto& option : viewOptionValues.values) {
      try {
        viewOptions[option] = true;
      } catch (const std::out_of_range& e) {
        LOG("Unknown --view option '%1$s' ignored. Use -h to list available options.", option);
      }
    }
  }

  if (vm.count("o")) {
    output_files = vm["o"].as<std::vector<std::string>>();
  }
  if (vm.count("d")) {
    if (deps_output_file) help(argv[0], desc, true);
    deps_output_file = vm["d"].as<std::string>().c_str();
  }
  if (vm.count("m")) {
    if (make_command) help(argv[0], desc, true);
    make_command = vm["m"].as<std::string>().c_str();
  }

  if (vm.count("D")) {
    for (const auto& cmd : vm["D"].as<std::vector<std::string>>()) {
      commandline_commands += cmd;
      commandline_commands += ";\n";
    }
  }
  if (vm.count("enable")) {
    for (const auto& feature : vm["enable"].as<std::vector<std::string>>()) {
      if (feature == "all") {
        Feature::enable_all();
        break;
      }
      Feature::enable_feature(feature);
    }
  }

  std::string parameterFile;
  if (vm.count("p")) {
    if (!parameterFile.empty()) {
      help(argv[0], desc, true);
    }
    parameterFile = vm["p"].as<std::string>().c_str();
  }

  std::string parameterSet;
  if (vm.count("P")) {
    if (!parameterSet.empty()) {
      help(argv[0], desc, true);
    }
    parameterSet = vm["P"].as<std::string>().c_str();
  }

  std::vector<std::string> inputFiles;
  if (vm.count("input-file")) {
    inputFiles = vm["input-file"].as<std::vector<std::string>>();
  }

  if (vm.count("colorscheme")) {
    arg_colorscheme = vm["colorscheme"].as<std::string>();
  }

  if (vm.count("export-format")) {
    const auto format_str = vm["export-format"].as<std::string>();
    FileFormat format;
    if (fileformat::fromIdentifier(format_str, format)) {
      export_format.emplace(format);

    } else {
      LOG("Unknown --export-format option '%1$s'.  Use -h to list available options.", format_str);
      return 1;
    }
  }

  AnimateArgs const animate = get_animate(vm);
  const Camera camera = get_camera(vm);

  if (animate.frames) {
    for (const auto& filename : output_files) {
      if (filename == "-") {
        LOG("Option --animate is not supported when exporting to stdout.");
        return 1;
      }
    }
    if (interferenceCheck) {
      LOG("Option --interference-check cannot be combined with --animate (one report per run).");
      return 1;
    }
    if (output_files.empty()) {
      output_files.emplace_back("frame.png");
    }
  }

  PRINTDB("Application location detected as %s", applicationPath);

  if (vm.count("memo-replay") || vm.count("memo-selftest")) {
    std::vector<std::string> files;
    if (vm.count("memo-selftest")) {
      const auto file = vm["memo-selftest"].as<std::string>();
      files = {file, file};
    } else {
      files = vm["memo-replay"].as<std::vector<std::string>>();
    }
    const bool verify = vm.count("memo-verify") || vm.count("memo-selftest");
    try {
      const int keep = vm.count("memo-keep") ? vm["memo-keep"].as<int>() : -1;
      return memo_replay(files, commandline_commands, verify, vm.count("memo-geometry") > 0, keep);
    } catch (const HardWarningException&) {
      return 1;
    }
  }

  auto cmdlinemode = false;
  if (!output_files.empty()) {  // cmd-line mode
    cmdlinemode = true;
    if (!inputFiles.size()) help(argv[0], desc, true);
  }

  if (arg_info || cmdlinemode) {
    if (inputFiles.size() > 1) help(argv[0], desc, true);
    try {
      localization_init();
      if (arg_info) {
        rc = info();
      } else {
        for (const auto& filename : output_files) {
          const bool is_stdin = inputFiles[0] == "-";
          const std::string input_file = is_stdin ? "<stdin>" : inputFiles[0];
          const bool is_stdout = filename == "-";
          const std::string output_file = is_stdout ? "<stdout>" : filename;
          const auto export_options = convert_export_options(vm);
          const CommandLine cmd{is_stdin,
                                input_file,
                                is_stdout,
                                output_file,
                                original_path,
                                parameterFile,
                                parameterSet,
                                viewOptions,
                                camera,
                                export_format,
                                export_options,
                                animate,
                                vm.count("summary") ? vm["summary"].as<std::vector<std::string>>()
                                                    : std::vector<std::string>{},
                                vm.count("summary-file") ? vm["summary-file"].as<std::string>() : "",
                                interferenceCheck,
                                interferenceFile};
          rc |= cmdline(cmd);
        }
      }
    } catch (const HardWarningException&) {
      rc = 1;
    }

    if (deps_output_file) {
      std::string const deps_out(deps_output_file);
      const std::vector<std::string>& geom_out(output_files);
      if (!write_deps(deps_out, geom_out)) {
        LOG("Error writing deps");
        return 1;
      }
    }
#ifndef OPENSCAD_NOGUI
  } else if (useGUI()) {
    if (vm.count("export-format")) {
      LOG("Ignoring --export-format option");
    }
    std::string gui_test = "none";
    if (vm.count("run-all-gui-tests")) {
      gui_test = "all";
    }
    auto reset_window_settings = vm.count("reset-window-settings") > 0;
    rc = gui(inputFiles, original_path, argc, argv, gui_test, reset_window_settings);
#endif
  } else {
    LOG("Requested GUI mode but can't open display!\n");
    return 1;
  }

  Builtins::instance(true);

  return rc;
}
