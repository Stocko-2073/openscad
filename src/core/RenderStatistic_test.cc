#include "RenderStatistic.h"

#include <catch2/catch_all.hpp>
#include <chrono>
#include <string>
#include <vector>

#include "utils/printutils.h"

TEST_CASE("A phase's note follows its time", "[statistic]")
{
  RenderStatistic statistic;
  statistic.addPhaseTime(RenderStatistic::PHASE_PARSING, std::chrono::milliseconds(5));
  statistic.addPhaseTime(RenderStatistic::PHASE_EVALUATION, std::chrono::milliseconds(20));
  statistic.setPhaseNote(RenderStatistic::PHASE_EVALUATION, "reused 3 of 4 module calls");

  const auto phases = statistic.phaseTimes();
  REQUIRE(phases.size() == 2);
  CHECK(phases[0].note.empty());
  CHECK(phases[1].ms == std::chrono::milliseconds(20));
  CHECK(phases[1].note == "reused 3 of 4 module calls");

  std::vector<Message> printed;
  g_message_capture.push_back(&printed);
  {
    const PrintSuppressGuard quiet;
    statistic.printRenderingTime();
  }
  g_message_capture.pop_back();
  // The total, then a line per phase.
  REQUIRE(printed.size() == 3);
  CHECK(printed[1].msg.find("Parsing:") != std::string::npos);
  CHECK(printed[1].msg.back() == ')');
  CHECK(printed[2].msg.find("Script evaluation:") != std::string::npos);
  CHECK(printed[2].msg.find("%), reused 3 of 4 module calls") != std::string::npos);

  // start() begins the next render's statistic, without the notes of the last.
  statistic.start();
  statistic.beginPhase(RenderStatistic::PHASE_EVALUATION);
  statistic.endPhase(RenderStatistic::PHASE_EVALUATION);
  REQUIRE(statistic.phaseTimes().size() == 1);
  CHECK(statistic.phaseTimes()[0].note.empty());
}
