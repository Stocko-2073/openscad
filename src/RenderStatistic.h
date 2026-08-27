/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright (C) 2020 Golubev Alexander <fatzer2@gmail.com>
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

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "geometry/Geometry.h"
#include "glview/Camera.h"

/**
 * An utility class to collect and print rendering statistics for the given
 * geometry
 */
class RenderStatistic
{
public:
  constexpr static auto CACHE = "cache";
  constexpr static auto TIME = "time";
  constexpr static auto CAMERA = "camera";
  constexpr static auto GEOMETRY = "geometry";
  constexpr static auto BOUNDING_BOX = "bounding-box";
  constexpr static auto AREA = "area";

  // Names of the pipeline phases itemized underneath the total render time.
  // Kept in one place so the GUI and the command line agree, and so a phase
  // that is timed from more than one call site accumulates into one entry.
  //
  // Each phase is named for what it produces, since the stages are easy to
  // confuse: the script is *evaluated* into a tree of AbstractNodes, and only
  // the stage after that builds the CSGNode tree which normalization then turns
  // into CSG products.
  constexpr static auto PHASE_PARSING = "Parsing";               // source -> AST
  constexpr static auto PHASE_EVALUATION = "Script evaluation";  // AST -> node tree
  constexpr static auto PHASE_CSG_BUILD = "CSG tree build";      // node tree -> CSG tree
  constexpr static auto PHASE_CSG_NORMALIZATION = "CSG normalization";  // CSG tree -> products
  constexpr static auto PHASE_INTERFERENCE = "Interference check";
  constexpr static auto PHASE_RENDERERS = "Renderer construction";
  constexpr static auto PHASE_GEOMETRY = "Geometry evaluation";  // node tree -> geometry
  constexpr static auto PHASE_EXPORT = "Export";

  /**
   * Construct a statistic printer for the given geometry with current
   * time as start time.
   */
  RenderStatistic();

  /**
   * Set start time when reusing a RenderStatistic instance.
   */
  void start();

  /**
   * Return render time in milliseconds.
   */
  std::chrono::milliseconds ms();

  /**
   * A named pipeline phase and the time accumulated in it.
   */
  struct PhaseTime {
    std::string name;
    std::chrono::milliseconds ms;
  };

  /**
   * Start timing the named pipeline phase. Beginning a phase that is already
   * running is a no-op, and a phase begun again after it ended accumulates into
   * the same entry, keeping the position of its first use. That way a phase
   * spread over several callbacks still reads as a single line.
   */
  void beginPhase(const std::string& name);

  /**
   * Stop timing the named phase. Ending a phase that isn't running is a no-op,
   * so cancellation and error paths can call this without checking.
   */
  void endPhase(const std::string& name);

  /**
   * The phases recorded since @ref start, in the order they were first begun.
   * A phase still running is reported with the time accumulated so far.
   */
  [[nodiscard]] std::vector<PhaseTime> phaseTimes() const;

  /**
   * Times a pipeline phase for the duration of the enclosing scope.
   */
  class ScopedPhase
  {
public:
    ScopedPhase(RenderStatistic& statistic, std::string name)
      : statistic(statistic), name(std::move(name))
    {
      statistic.beginPhase(this->name);
    }
    ~ScopedPhase() { statistic.endPhase(name); }
    ScopedPhase(const ScopedPhase&) = delete;
    ScopedPhase& operator=(const ScopedPhase&) = delete;

private:
    RenderStatistic& statistic;
    std::string name;
  };

  /**
   * Print some statistic on cache usage. Namely, stats on the @ref GeometryCache
   * and @ref CGALCache (if enabled).
   */
  void printCacheStatistic();

  /**
   * Format and print time elapsed by rendering.
   */
  void printRenderingTime();

  /**
   * Print all available statistic information.
   */
  void printAll(const std::shared_ptr<const Geometry>& geom, const Camera& camera,
                const std::vector<std::string>& options = {}, const std::string& filename = {});

private:
  struct Phase {
    std::string name;
    std::chrono::steady_clock::time_point begin;
    std::chrono::steady_clock::duration elapsed{0};
    bool running{false};
  };

  Phase *findPhase(const std::string& name);

  std::chrono::steady_clock::time_point begin;
  std::vector<Phase> phases;
};
