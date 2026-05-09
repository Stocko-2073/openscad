#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/AST.h"
#include "core/Arguments.h"
#include "core/Builtins.h"
#include "core/EvaluationSession.h"
#include "core/Parameters.h"
#include "core/SolutionType.h"
#include "core/Value.h"
#include "core/function.h"
#include "utils/printutils.h"

#include <slvs.h>

namespace {

constexpr const char *KIND_KEY = "_kind";

ObjectType make_kind_obj(EvaluationSession *session, const char *kind)
{
  ObjectType obj(session);
  obj.set(KIND_KEY, Value(std::string(kind)));
  return obj;
}

bool object_kind(const Value& v, std::string& out)
{
  if (v.type() != Value::Type::OBJECT) return false;
  const ObjectType& obj = v.toObject();
  if (!obj.contains(KIND_KEY)) return false;
  const Value& k = obj.get(KIND_KEY);
  if (k.type() != Value::Type::STRING) return false;
  out = k.toString();
  return true;
}

bool field_string(const ObjectType& obj, const std::string& key, std::string& out)
{
  if (!obj.contains(key)) return false;
  const Value& v = obj.get(key);
  if (v.type() != Value::Type::STRING) return false;
  out = v.toString();
  return true;
}

bool field_double(const ObjectType& obj, const std::string& key, double& out)
{
  if (!obj.contains(key)) return false;
  const Value& v = obj.get(key);
  if (v.type() != Value::Type::NUMBER) return false;
  out = v.toDouble();
  return true;
}

bool field_xy(const ObjectType& obj, const std::string& key, double& x, double& y)
{
  if (!obj.contains(key)) return false;
  return obj.get(key).getVec2(x, y);
}

// Deterministic initial coordinates for a point with no `at=`. The base
// pattern (attempt 0) is a golden-angle spiral around the origin: non-
// collinear, non-coincident, all distinct distances. Higher attempts perturb
// the base with a Mersenne Twister seeded from (idx, attempt) so multi-start
// retries escape bad basins of attraction (a flat staircase like (1, 0.5),
// (2, 1.0), ... lands Newton on symmetry axes for problems like a square
// anchored at a single corner). Seeding is deterministic, so the same
// (idx, attempt, length_scale) yields the same coordinates across runs.
// length_scale stretches the spiral so unseeded points start in the same
// neighbourhood as seeded points / length-bearing constraints; without this
// a kilometre-scale sketch starts unseeded points at unit distance and
// Newton has to traverse three orders of magnitude on its first step.
std::pair<double, double> default_unseeded_seed(size_t idx, int attempt,
                                                double length_scale = 1.0)
{
  constexpr double GOLDEN_ANGLE = 2.39996322972865332;  // π(3 - √5)
  double base_radius = (1.0 + static_cast<double>(idx) * 0.5) * length_scale;
  double base_angle  = static_cast<double>(idx + 1) * GOLDEN_ANGLE;
  if (attempt <= 0) {
    return {base_radius * std::cos(base_angle), base_radius * std::sin(base_angle)};
  }
  std::seed_seq seq{static_cast<uint32_t>(idx),
                    static_cast<uint32_t>(attempt)};
  std::mt19937 rng(seq);
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  double angle  = base_angle + 2.0 * M_PI * u01(rng);
  double radius = base_radius * (0.25 + 3.75 * u01(rng));
  return {radius * std::cos(angle), radius * std::sin(angle)};
}

// Apply a jitter to a user-provided seed on retry attempts (>0). First
// attempt returns the seed unchanged so a sketch that converges does so
// without disturbing user intent. Subsequent attempts perturb by a fraction
// of length_scale so that a fully-seeded sketch with inequalities (which has
// no other degree of freedom in the multi-start loop) can still escape
// branch-cut traps where a directed-angle decomposition or same-side
// inequality lands on the wrong half-plane.
//
// Jitter radius grows with attempt: small at first (1% of length_scale) so
// almost-correct seeds don't move much, doubling each attempt up to the full
// length_scale. By the last attempt the perturbation is large enough to
// commonly cross a half-plane boundary in a typical sketch. Deterministic
// per (idx, attempt) — same input gives same output across runs.
std::pair<double, double> jittered_seed(double u, double v,
                                        size_t idx, int attempt,
                                        double length_scale)
{
  if (attempt <= 0) return {u, v};
  std::seed_seq seq{static_cast<uint32_t>(idx),
                    static_cast<uint32_t>(attempt),
                    0x53454544u};  // 'SEED' marker; differs from default_unseeded_seed
  std::mt19937 rng(seq);
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  double angle  = 2.0 * M_PI * u01(rng);
  double max_radius = length_scale *
      std::min(1.0, 0.01 * std::pow(2.0, static_cast<double>(attempt - 1)));
  double radius = max_radius * u01(rng);
  return {u + radius * std::cos(angle),
          v + radius * std::sin(angle)};
}

}  // namespace

// ---------------------------------------------------------------------------
// Entity factories
// ---------------------------------------------------------------------------

Value builtin_point(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  Parameters params = Parameters::parse(std::move(arguments), loc, {"name"}, {"at"});
  if (params["name"].type() != Value::Type::STRING) {
    LOG(message_group::Warning, loc, params.documentRoot(),
        "point() requires a string name as the first argument");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "point");
  obj.set("name", params["name"].clone());
  if (params["at"].type() == Value::Type::VECTOR) {
    double x, y;
    if (params["at"].getVec2(x, y)) {
      obj.set("at", params["at"].clone());
    } else {
      LOG(message_group::Warning, loc, params.documentRoot(),
          "point() at= must be a 2-element vector");
    }
  }
  return obj;
}

// ---------------------------------------------------------------------------
// Constraint factories
// ---------------------------------------------------------------------------

Value make_two_point_constraint(const char *user_name, const char *kind, Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 2 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "%1$s() expects two string point names", user_name);
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, kind);
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  return obj;
}

Value builtin_con_coincident(Arguments arguments, const Location& loc)
{
  return make_two_point_constraint("con_coincident", "coincident", std::move(arguments), loc);
}

Value builtin_con_horizontal(Arguments arguments, const Location& loc)
{
  return make_two_point_constraint("con_horizontal", "horizontal", std::move(arguments), loc);
}

Value builtin_con_vertical(Arguments arguments, const Location& loc)
{
  return make_two_point_constraint("con_vertical", "vertical", std::move(arguments), loc);
}

Value builtin_con_distance(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 3 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::NUMBER) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_distance() expects (point_name, point_name, number)");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "distance");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("d", arguments[2]->clone());
  return obj;
}

Value builtin_con_le_distance(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 3 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::NUMBER) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_le_distance() expects (point_name, point_name, number)");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "le_distance");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("d", arguments[2]->clone());
  return obj;
}

Value builtin_con_ge_distance(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 3 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::NUMBER) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_ge_distance() expects (point_name, point_name, number)");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "ge_distance");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("d", arguments[2]->clone());
  return obj;
}

Value builtin_con_same_side(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 4 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::STRING ||
      arguments[3]->type() != Value::Type::STRING) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_same_side() expects four point names: (a, b, p, q)");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "same_side");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("p", arguments[2]->clone());
  obj.set("q", arguments[3]->clone());
  return obj;
}

Value builtin_con_opposite_side(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 4 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::STRING ||
      arguments[3]->type() != Value::Type::STRING) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_opposite_side() expects four point names: (a, b, p, q)");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "opposite_side");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("p", arguments[2]->clone());
  obj.set("q", arguments[3]->clone());
  return obj;
}

Value builtin_con_perpendicular(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 3 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::STRING) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_perpendicular() expects three point names: (p1, vertex, p2)");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "perpendicular");
  obj.set("a", arguments[0]->clone());
  obj.set("vertex", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  return obj;
}

Value builtin_con_parallel(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 4 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::STRING ||
      arguments[3]->type() != Value::Type::STRING) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_parallel() expects four point names");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "parallel");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  return obj;
}

Value builtin_con_angle(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 5 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::STRING ||
      arguments[3]->type() != Value::Type::STRING ||
      arguments[4]->type() != Value::Type::NUMBER) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_angle() expects four point names and a degree value");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "angle");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  obj.set("deg", arguments[4]->clone());
  return obj;
}

Value builtin_con_le_angle(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 5 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::STRING ||
      arguments[3]->type() != Value::Type::STRING ||
      arguments[4]->type() != Value::Type::NUMBER) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_le_angle() expects four point names and a degree value");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "le_angle");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  obj.set("deg", arguments[4]->clone());
  return obj;
}

Value builtin_con_ge_angle(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 5 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::STRING ||
      arguments[3]->type() != Value::Type::STRING ||
      arguments[4]->type() != Value::Type::NUMBER) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_ge_angle() expects four point names and a degree value");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "ge_angle");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  obj.set("deg", arguments[4]->clone());
  return obj;
}

Value builtin_con_directed_angle(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 5 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::STRING ||
      arguments[3]->type() != Value::Type::STRING ||
      arguments[4]->type() != Value::Type::NUMBER) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_directed_angle() expects (p1, p2, p3, p4, deg) — four "
        "point names and a degree value in [0, 360)");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "directed_angle");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  obj.set("deg", arguments[4]->clone());
  return obj;
}

Value builtin_con_fixed(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 1 || arguments[0]->type() != Value::Type::STRING) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_fixed() expects a single point name");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "fixed");
  obj.set("a", arguments[0]->clone());
  return obj;
}

namespace {

bool require_strings(const char *fn, const Arguments& arguments, const Location& loc, size_t n)
{
  if (arguments.size() != n) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "%1$s() expects %2$d string point name(s)", fn, static_cast<int>(n));
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    if (arguments[i]->type() != Value::Type::STRING) {
      LOG(message_group::Warning, loc, arguments.documentRoot(),
          "%1$s() expects string point names", fn);
      return false;
    }
  }
  return true;
}

bool require_strings_then_number(const char *fn, const Arguments& arguments, const Location& loc, size_t nstr)
{
  if (arguments.size() != nstr + 1) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "%1$s() expects %2$d string(s) and a number", fn, static_cast<int>(nstr));
    return false;
  }
  for (size_t i = 0; i < nstr; ++i) {
    if (arguments[i]->type() != Value::Type::STRING) {
      LOG(message_group::Warning, loc, arguments.documentRoot(),
          "%1$s() expects %2$d string point names followed by a number",
          fn, static_cast<int>(nstr));
      return false;
    }
  }
  if (arguments[nstr]->type() != Value::Type::NUMBER) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "%1$s() expects the final argument to be a number", fn);
    return false;
  }
  return true;
}

}  // namespace

Value builtin_con_pt_on_line(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("con_pt_on_line", arguments, loc, 3)) return Value::undefined.clone();
  ObjectType obj = make_kind_obj(session, "pt_on_line");
  obj.set("p", arguments[0]->clone());
  obj.set("a", arguments[1]->clone());
  obj.set("b", arguments[2]->clone());
  return obj;
}

Value builtin_con_pt_on_segment(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("con_pt_on_segment", arguments, loc, 3)) return Value::undefined.clone();
  ObjectType obj = make_kind_obj(session, "pt_on_segment");
  obj.set("p", arguments[0]->clone());
  obj.set("a", arguments[1]->clone());
  obj.set("b", arguments[2]->clone());
  return obj;
}

Value builtin_con_pt_line_distance(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings_then_number("con_pt_line_distance", arguments, loc, 3)) {
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "pt_line_distance");
  obj.set("p", arguments[0]->clone());
  obj.set("a", arguments[1]->clone());
  obj.set("b", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  return obj;
}

Value builtin_con_le_pt_line_distance(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings_then_number("con_le_pt_line_distance", arguments, loc, 3)) {
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "le_pt_line_distance");
  obj.set("p", arguments[0]->clone());
  obj.set("a", arguments[1]->clone());
  obj.set("b", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  return obj;
}

Value builtin_con_ge_pt_line_distance(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings_then_number("con_ge_pt_line_distance", arguments, loc, 3)) {
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "ge_pt_line_distance");
  obj.set("p", arguments[0]->clone());
  obj.set("a", arguments[1]->clone());
  obj.set("b", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  return obj;
}

Value builtin_con_at_midpoint(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("con_at_midpoint", arguments, loc, 3)) return Value::undefined.clone();
  ObjectType obj = make_kind_obj(session, "at_midpoint");
  obj.set("p", arguments[0]->clone());
  obj.set("a", arguments[1]->clone());
  obj.set("b", arguments[2]->clone());
  return obj;
}

Value builtin_con_equal_length(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("con_equal_length", arguments, loc, 4)) return Value::undefined.clone();
  ObjectType obj = make_kind_obj(session, "equal_length");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  return obj;
}

Value builtin_con_length_ratio(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings_then_number("con_length_ratio", arguments, loc, 4)) {
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "length_ratio");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  obj.set("r", arguments[4]->clone());
  return obj;
}

Value builtin_con_length_difference(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings_then_number("con_length_difference", arguments, loc, 4)) {
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "length_difference");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  obj.set("diff", arguments[4]->clone());
  return obj;
}

Value builtin_con_le_length_difference(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings_then_number("con_le_length_difference", arguments, loc, 4)) {
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "le_length_difference");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  obj.set("diff", arguments[4]->clone());
  return obj;
}

Value builtin_con_ge_length_difference(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings_then_number("con_ge_length_difference", arguments, loc, 4)) {
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "ge_length_difference");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  obj.set("diff", arguments[4]->clone());
  return obj;
}

Value builtin_con_eq_len_pt_line_d(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("con_eq_len_pt_line_d", arguments, loc, 5)) {
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "eq_len_pt_line_d");
  obj.set("p", arguments[0]->clone());
  obj.set("a", arguments[1]->clone());
  obj.set("b", arguments[2]->clone());
  obj.set("c", arguments[3]->clone());
  obj.set("d", arguments[4]->clone());
  return obj;
}

Value builtin_con_eq_pt_ln_distances(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("con_eq_pt_ln_distances", arguments, loc, 6)) {
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "eq_pt_ln_distances");
  obj.set("p1", arguments[0]->clone());
  obj.set("a1", arguments[1]->clone());
  obj.set("b1", arguments[2]->clone());
  obj.set("p2", arguments[3]->clone());
  obj.set("a2", arguments[4]->clone());
  obj.set("b2", arguments[5]->clone());
  return obj;
}

Value builtin_con_equal_angle(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("con_equal_angle", arguments, loc, 8)) {
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "equal_angle");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  obj.set("e", arguments[4]->clone());
  obj.set("f", arguments[5]->clone());
  obj.set("g", arguments[6]->clone());
  obj.set("h", arguments[7]->clone());
  return obj;
}

Value builtin_con_symmetric_horiz(Arguments arguments, const Location& loc)
{
  return make_two_point_constraint("con_symmetric_horiz", "symmetric_horiz", std::move(arguments), loc);
}

Value builtin_con_symmetric_vert(Arguments arguments, const Location& loc)
{
  return make_two_point_constraint("con_symmetric_vert", "symmetric_vert", std::move(arguments), loc);
}

Value builtin_con_symmetric_line(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("con_symmetric_line", arguments, loc, 4)) {
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "symmetric_line");
  obj.set("p1", arguments[0]->clone());
  obj.set("p2", arguments[1]->clone());
  obj.set("a", arguments[2]->clone());
  obj.set("b", arguments[3]->clone());
  return obj;
}

// ---------------------------------------------------------------------------
// solve2d
// ---------------------------------------------------------------------------

namespace {

struct PointDecl {
  std::string name;
  bool has_seed = false;
  double u = 0.0;
  double v = 0.0;
  // Filled during build phase:
  Slvs_hParam u_param = 0;
  Slvs_hParam v_param = 0;
  Slvs_hEntity entity = 0;
};

struct ConstraintDecl {
  std::string kind;
  std::string name;  // for failed-constraint reporting; here we use the constraint index
  std::vector<std::string> points;
  double valA = 0.0;
  // If non-empty, indices into inequalities[] whose presence in the active set
  // suppresses this equality constraint. Used by composite expansions (e.g.
  // con_pt_on_segment) where the equality becomes redundant once a bound is
  // tight, and emitting both would make the system rank-deficient.
  std::vector<size_t> suppress_when_active;
};

struct InequalityDecl {
  std::string kind;                  // "le_distance", "le_pt_line_distance", ...
  std::string name;                  // user-visible summary for active_inequalities
  std::vector<std::string> points;
  double valA = 0.0;                 // d / diff / deg, depending on kind
};

// Per-directed_angle metadata used by retry seeding. con_directed_angle
// expands into a magnitude angle + (sometimes) a half-plane inequality, but
// neither carries the directed (CCW) target, so the seed retry logic also
// stores the original deg here. On attempts > 0 the seed for p4 is rotated
// onto the angle ray from p3 (using p1->p2 as the reference direction);
// see build_and_solve_once. Without this, libslvs's Newton settles into a
// wrong-angle local minimum (REDUNDANT_OKAY → reported as INCONSISTENT)
// when the user's seed for p4 happens to be at a far-off angle from the
// target, and Cartesian-only jitter rarely escapes that basin.
struct DirectedAngleSeed {
  size_t p1_idx = 0;
  size_t p2_idx = 0;
  size_t p3_idx = 0;
  size_t p4_idx = 0;
  double deg = 0.0;                  // wrapped into [0, 360)
};

// Tolerances that scale with the sketch. All length-unit residuals and
// violations are compared against (TOL * length_scale) so the same geometry
// behaves identically at mm, m, or μm scales. Dimensionless and degree
// residuals use scale-independent tolerances. See compute_length_scale().
constexpr double RESIDUAL_TOL_REL = 1e-2;     // length-unit residuals (relative)
constexpr double RESIDUAL_TOL_DIM = 1e-2;     // dimensionless residuals
constexpr double RESIDUAL_TOL_DEG = 1e-2;     // degree residuals (1/100 degree)
constexpr double VIOLATION_TOL_REL = 1e-6;    // length-unit violations (relative)
constexpr double DEGENERATE_REL = 1e-12;      // length / length_scale below which a vector is zero

enum class ResidualUnit { Length, Dimensionless, Degree };

// Classify a constraint's residual by physical units. Drives the unit-aware
// REDUNDANT_OKAY recovery threshold so a 1% slop on a 1000-unit distance
// constraint isn't held to the same absolute bound as a 1° angle constraint.
inline ResidualUnit residual_unit_for(const std::string& kind)
{
  if (kind == "parallel" || kind == "perpendicular" ||
      kind == "length_ratio") {
    return ResidualUnit::Dimensionless;
  }
  if (kind == "angle" || kind == "equal_angle") {
    return ResidualUnit::Degree;
  }
  return ResidualUnit::Length;
}

inline double residual_threshold(ResidualUnit u, double length_scale)
{
  switch (u) {
    case ResidualUnit::Length:        return RESIDUAL_TOL_REL * length_scale;
    case ResidualUnit::Dimensionless: return RESIDUAL_TOL_DIM;
    case ResidualUnit::Degree:        return RESIDUAL_TOL_DEG;
  }
  return RESIDUAL_TOL_REL * length_scale;
}

// Median magnitude across seeds and length-bearing constraint values.
// Median (not max) so an outlier — e.g. one large bound — doesn't dominate
// scale derived from a cluster of unit-magnitude seeds. Returns 1.0 if there
// is nothing length-bearing to sample, in which case all callers fall back to
// scale-1 behaviour matching pre-change examples.
inline double compute_length_scale(const std::vector<PointDecl>& points,
                                   const std::vector<ConstraintDecl>& constraints,
                                   const std::vector<InequalityDecl>& inequalities)
{
  std::vector<double> samples;
  for (const auto& p : points) {
    if (!p.has_seed) continue;
    double m = std::sqrt(p.u * p.u + p.v * p.v);
    if (m > 0.0) samples.push_back(m);
  }
  for (const auto& c : constraints) {
    if (c.kind == "distance" || c.kind == "pt_line_distance" ||
        c.kind == "length_difference") {
      double m = std::abs(c.valA);
      if (m > 0.0) samples.push_back(m);
    }
  }
  for (const auto& ineq : inequalities) {
    if (ineq.kind == "le_distance" || ineq.kind == "ge_distance" ||
        ineq.kind == "le_pt_line_distance" || ineq.kind == "ge_pt_line_distance" ||
        ineq.kind == "le_length_difference" || ineq.kind == "ge_length_difference") {
      double m = std::abs(ineq.valA);
      if (m > 0.0) samples.push_back(m);
    }
  }
  if (samples.empty()) return 1.0;
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

bool collect_point_ref(const ConstraintDecl& c, size_t idx, const std::string& field,
                       const std::map<std::string, size_t>& name_to_idx,
                       const std::vector<PointDecl>& points,
                       const Location& loc, const std::string& doc_root,
                       Slvs_hEntity& out_entity)
{
  if (idx >= c.points.size()) return false;
  const std::string& pname = c.points[idx];
  auto it = name_to_idx.find(pname);
  if (it == name_to_idx.end()) {
    LOG(message_group::Warning, loc, doc_root,
        "%1$s constraint references unknown point '%2$s'", c.kind, pname);
    return false;
  }
  out_entity = points[it->second].entity;
  (void)field;
  return true;
}

// Compute a residual for `c` given the solved point coordinates. Returns
// the absolute error of the constraint as a non-negative number; lower is
// better, zero means satisfied. Returns 0 for unknown kinds, missing
// points, and degenerate inputs (zero-length lines) so that we don't
// generate spurious failures.
//
// Used to recover from a SolveSpace quirk: when the Jacobian is rank-
// deficient at the solution (REDUNDANT_OKAY internally) the C wrapper
// reports SLVS_RESULT_INCONSISTENT regardless of whether Newton actually
// converged. Recomputing residuals lets us tell "redundant but solved"
// from "actually broken" without patching libslvs.
double constraint_residual(const ConstraintDecl& c,
                           const std::map<std::string, SolutionType::Point2d>& pts)
{
  using P = SolutionType::Point2d;
  auto get = [&](const std::string& n, P& out) -> bool {
    auto it = pts.find(n);
    if (it == pts.end()) return false;
    out = it->second;
    return true;
  };
  auto sub = [](const P& a, const P& b) -> P { return {a[0]-b[0], a[1]-b[1]}; };
  auto norm = [](const P& v) { return std::sqrt(v[0]*v[0] + v[1]*v[1]); };
  auto cross_z = [](const P& a, const P& b) { return a[0]*b[1] - a[1]*b[0]; };
  auto dot = [](const P& a, const P& b) { return a[0]*b[0] + a[1]*b[1]; };
  constexpr double DEGENERATE = 1e-12;

  P a, b, p, q, r, s;

  if ((c.kind == "coincident" || c.kind == "horizontal" ||
       c.kind == "vertical" || c.kind == "distance" ||
       c.kind == "symmetric_horiz" || c.kind == "symmetric_vert") &&
      c.points.size() == 2) {
    if (!get(c.points[0], a) || !get(c.points[1], b)) return 0.0;
    if (c.kind == "coincident")      return norm(sub(a, b));
    if (c.kind == "horizontal")      return std::abs(a[1] - b[1]);
    if (c.kind == "vertical")        return std::abs(a[0] - b[0]);
    if (c.kind == "distance")        return std::abs(norm(sub(a,b)) - c.valA);
    if (c.kind == "symmetric_horiz") return std::sqrt((a[1]-b[1])*(a[1]-b[1]) + (a[0]+b[0])*(a[0]+b[0]));
    if (c.kind == "symmetric_vert")  return std::sqrt((a[0]-b[0])*(a[0]-b[0]) + (a[1]+b[1])*(a[1]+b[1]));
  }
  if (c.kind == "fixed") {
    return 0.0;
  }
  if (c.kind == "perpendicular" && c.points.size() == 3) {
    if (!get(c.points[0], p) || !get(c.points[1], a) || !get(c.points[2], b)) return 0.0;
    P v1 = sub(p, a), v2 = sub(b, a);
    double m = norm(v1) * norm(v2);
    return m < DEGENERATE ? 0.0 : std::abs(dot(v1, v2)) / m;
  }
  if ((c.kind == "pt_on_line" || c.kind == "at_midpoint") && c.points.size() == 3) {
    if (!get(c.points[0], p) || !get(c.points[1], a) || !get(c.points[2], b)) return 0.0;
    if (c.kind == "at_midpoint") {
      P mid{(a[0]+b[0])/2, (a[1]+b[1])/2};
      return norm(sub(p, mid));
    }
    P v = sub(b, a);
    double m = norm(v);
    return m < DEGENERATE ? 0.0 : std::abs(cross_z(sub(p, a), v)) / m;
  }
  if (c.kind == "pt_line_distance" && c.points.size() == 3) {
    if (!get(c.points[0], p) || !get(c.points[1], a) || !get(c.points[2], b)) return 0.0;
    P v = sub(b, a);
    double m = norm(v);
    if (m < DEGENERATE) return 0.0;
    double signed_dist = cross_z(sub(p, a), v) / m;
    return std::abs(signed_dist - c.valA);
  }
  if ((c.kind == "parallel" || c.kind == "angle" ||
       c.kind == "equal_length" || c.kind == "length_ratio" ||
       c.kind == "length_difference") && c.points.size() == 4) {
    if (!get(c.points[0], a) || !get(c.points[1], b) ||
        !get(c.points[2], p) || !get(c.points[3], q)) return 0.0;
    P v1 = sub(b, a), v2 = sub(q, p);
    double m1 = norm(v1), m2 = norm(v2);
    if (c.kind == "parallel") {
      double mm = m1 * m2;
      return mm < DEGENERATE ? 0.0 : std::abs(cross_z(v1, v2)) / mm;
    }
    if (c.kind == "angle") {
      if (m1 < DEGENERATE || m2 < DEGENERATE) return 0.0;
      double cosA = std::max(-1.0, std::min(1.0, dot(v1, v2) / (m1 * m2)));
      double angle_deg = std::acos(cosA) * 180.0 / M_PI;
      // libslvs's SLVS_C_ANGLE residual is cos(actual)-cos(valA·π/180), which
      // is symmetric in valA → -valA and periodic mod 360°, and an angle of
      // 270° is equivalent to 90° (acos returns the magnitude in [0,180]).
      // Reduce target to [0,180] before differencing so any libslvs-converged
      // solution registers a small residual; otherwise valA outside [-180,180]
      // produces a residual ≥ 90° even when the geometry is correct, defeating
      // the REDUNDANT_OKAY recovery path.
      double target = std::fmod(std::abs(c.valA), 360.0);
      if (target > 180.0) target = 360.0 - target;
      return std::abs(angle_deg - target);
    }
    if (c.kind == "equal_length")      return std::abs(m1 - m2);
    if (c.kind == "length_ratio")      return m2 < DEGENERATE ? 0.0 : std::abs(m1/m2 - c.valA);
    if (c.kind == "length_difference") return std::abs((m1 - m2) - c.valA);
  }
  if (c.kind == "symmetric_line" && c.points.size() == 4) {
    P p1, p2, la, lb;
    if (!get(c.points[0], p1) || !get(c.points[1], p2) ||
        !get(c.points[2], la) || !get(c.points[3], lb)) return 0.0;
    P v = sub(lb, la);
    double m = norm(v);
    if (m < DEGENERATE) return 0.0;
    double perp = std::abs(dot(sub(p2, p1), v)) / m;
    double d_sum = (cross_z(sub(p1, la), v) + cross_z(sub(p2, la), v)) / m;
    return std::max(perp, std::abs(d_sum));
  }
  if (c.kind == "eq_len_pt_line_d" && c.points.size() == 5) {
    if (!get(c.points[0], p) || !get(c.points[1], a) || !get(c.points[2], b) ||
        !get(c.points[3], r) || !get(c.points[4], s)) return 0.0;
    double len = norm(sub(a, b));
    P v = sub(s, r);
    double m = norm(v);
    if (m < DEGENERATE) return 0.0;
    double dist = std::abs(cross_z(sub(p, r), v)) / m;
    return std::abs(len - dist);
  }
  if (c.kind == "eq_pt_ln_distances" && c.points.size() == 6) {
    P p1, p2, a1, b1, a2, b2;
    if (!get(c.points[0], p1) || !get(c.points[1], a1) || !get(c.points[2], b1) ||
        !get(c.points[3], p2) || !get(c.points[4], a2) || !get(c.points[5], b2)) return 0.0;
    P v1 = sub(b1, a1), v2 = sub(b2, a2);
    double m1 = norm(v1), m2 = norm(v2);
    if (m1 < DEGENERATE || m2 < DEGENERATE) return 0.0;
    return std::abs(std::abs(cross_z(sub(p1, a1), v1))/m1 -
                    std::abs(cross_z(sub(p2, a2), v2))/m2);
  }
  if (c.kind == "equal_angle" && c.points.size() == 8) {
    P endpts[4][2];
    for (int i = 0; i < 4; ++i) {
      if (!get(c.points[i*2], endpts[i][0]) ||
          !get(c.points[i*2+1], endpts[i][1])) return 0.0;
    }
    auto angle = [&](const P& x, const P& y) {
      P u = sub(y, x);
      double m = norm(u);
      return std::pair{u, m};
    };
    auto [u1, m1] = angle(endpts[0][0], endpts[0][1]);
    auto [u2, m2] = angle(endpts[1][0], endpts[1][1]);
    auto [u3, m3] = angle(endpts[2][0], endpts[2][1]);
    auto [u4, m4] = angle(endpts[3][0], endpts[3][1]);
    if (m1 < DEGENERATE || m2 < DEGENERATE || m3 < DEGENERATE || m4 < DEGENERATE) return 0.0;
    double c1 = std::max(-1.0, std::min(1.0, dot(u1, u2) / (m1*m2)));
    double c2 = std::max(-1.0, std::min(1.0, dot(u3, u4) / (m3*m4)));
    return std::abs(std::acos(c1) - std::acos(c2));
  }

  return 0.0;
}

// Signed violation for inequality g(x) <= rhs:
//   positive ⇒ violated by that amount
//   non-positive ⇒ slack (constraint satisfied)
// Returns 0 for unknown kinds and degenerate inputs (no false positives).
double inequality_violation(const InequalityDecl& ineq,
                            const std::map<std::string, SolutionType::Point2d>& pts)
{
  using P = SolutionType::Point2d;
  auto get = [&](const std::string& n, P& out) -> bool {
    auto it = pts.find(n);
    if (it == pts.end()) return false;
    out = it->second;
    return true;
  };
  auto sub = [](const P& a, const P& b) -> P { return {a[0]-b[0], a[1]-b[1]}; };
  auto norm = [](const P& v) { return std::sqrt(v[0]*v[0] + v[1]*v[1]); };

  // ge_* variants share the same g(x) - rhs computation but flip the sign:
  // g(x) >= rhs is violated when g(x) < rhs ⇒ violation = rhs - g(x).
  const double sense = (ineq.kind.rfind("ge_", 0) == 0) ? -1.0 : 1.0;

  P a, b;
  if ((ineq.kind == "le_distance" || ineq.kind == "ge_distance") && ineq.points.size() == 2) {
    if (!get(ineq.points[0], a) || !get(ineq.points[1], b)) return 0.0;
    return sense * (norm(sub(a, b)) - ineq.valA);
  }
  if ((ineq.kind == "le_pt_line_distance" || ineq.kind == "ge_pt_line_distance") &&
      ineq.points.size() == 3) {
    P p_, a_, b_;
    if (!get(ineq.points[0], p_) || !get(ineq.points[1], a_) ||
        !get(ineq.points[2], b_)) return 0.0;
    P v = sub(b_, a_);
    double m = norm(v);
    if (m < 1e-12) return 0.0;
    // signed distance: cross_z(p-a, v) / |v|
    double signed_dist = ((p_[0]-a_[0])*v[1] - (p_[1]-a_[1])*v[0]) / m;
    return sense * (signed_dist - ineq.valA);
  }
  if ((ineq.kind == "le_length_difference" || ineq.kind == "ge_length_difference") &&
      ineq.points.size() == 4) {
    P a_, b_, c_, d_;
    if (!get(ineq.points[0], a_) || !get(ineq.points[1], b_) ||
        !get(ineq.points[2], c_) || !get(ineq.points[3], d_)) return 0.0;
    return sense * ((norm(sub(b_, a_)) - norm(sub(d_, c_))) - ineq.valA);
  }
  if ((ineq.kind == "le_angle" || ineq.kind == "ge_angle") && ineq.points.size() == 4) {
    P a_, b_, c_, d_;
    if (!get(ineq.points[0], a_) || !get(ineq.points[1], b_) ||
        !get(ineq.points[2], c_) || !get(ineq.points[3], d_)) return 0.0;
    P v1 = sub(b_, a_), v2 = sub(d_, c_);
    double m1 = norm(v1), m2 = norm(v2);
    if (m1 < 1e-12 || m2 < 1e-12) return 0.0;
    double cosA = std::max(-1.0, std::min(1.0, (v1[0]*v2[0] + v1[1]*v2[1]) / (m1*m2)));
    double angle_deg = std::acos(cosA) * 180.0 / M_PI;
    return sense * (angle_deg - std::abs(ineq.valA));
  }
  if ((ineq.kind == "pt_on_segment_lower" || ineq.kind == "pt_on_segment_upper")
      && ineq.points.size() == 3) {
    P p_, a_, b_;
    if (!get(ineq.points[0], p_) || !get(ineq.points[1], a_) ||
        !get(ineq.points[2], b_)) return 0.0;
    P v = sub(b_, a_);
    double m = norm(v);
    if (m < 1e-12) return 0.0;  // degenerate segment
    // Project p onto the directed segment a→b and report the signed distance
    // outside the segment (length units). Lower bound violated ⇒ p is `s` units
    // before a; upper bound violated ⇒ p is `s` units past b. The previous
    // dot-product form returned length² and made the active-set tolerance
    // scale-dependent.
    if (ineq.kind == "pt_on_segment_lower") {
      P pa = sub(p_, a_);
      double s = (pa[0]*v[0] + pa[1]*v[1]) / m;  // signed distance along v from a
      return -s;
    } else {
      P pb = sub(p_, b_);
      double s = (pb[0]*v[0] + pb[1]*v[1]) / m;  // signed distance along v from b
      return s;
    }
  }
  if ((ineq.kind == "same_side" || ineq.kind == "opposite_side")
      && ineq.points.size() == 4) {
    P a_, b_, p_, q_;
    if (!get(ineq.points[0], a_) || !get(ineq.points[1], b_) ||
        !get(ineq.points[2], p_) || !get(ineq.points[3], q_)) return 0.0;
    P v = sub(b_, a_);
    double m = norm(v);
    if (m < 1e-12) return 0.0;  // degenerate line a==b
    // Signed perpendicular distance from line a→b: positive = left, negative = right,
    // zero = on the line. Length units; previously the cross product alone gave
    // length² and the product gave length⁴, both scale-dependent against a fixed
    // VIOLATION threshold.
    auto signed_dist = [&](const P& x) {
      return ((b_[0]-a_[0])*(x[1]-a_[1]) - (b_[1]-a_[1])*(x[0]-a_[0])) / m;
    };
    double dp = signed_dist(p_);
    double dq = signed_dist(q_);
    // Magnitude of violation = perpendicular distance of the closer point to
    // the line. If satisfied, return -mag as slack so the threshold check
    // (v > tol) sees a clearly-non-violated value.
    double mag = std::min(std::abs(dp), std::abs(dq));
    bool violated = (ineq.kind == "same_side") ? (dp * dq < 0.0) : (dp * dq > 0.0);
    return violated ? mag : -mag;
  }
  if ((ineq.kind == "oriented_left" || ineq.kind == "oriented_right")
      && ineq.points.size() == 3) {
    P a_, b_, p_;
    if (!get(ineq.points[0], a_) || !get(ineq.points[1], b_) ||
        !get(ineq.points[2], p_)) return 0.0;
    P v = sub(b_, a_);
    double m = norm(v);
    if (m < 1e-12) return 0.0;  // degenerate line a==b
    // Signed perpendicular distance from line a→b. Length units (previously
    // length² from the bare cross product, which made the violation threshold
    // scale-dependent).
    double signed_dist = ((b_[0]-a_[0])*(p_[1]-a_[1]) - (b_[1]-a_[1])*(p_[0]-a_[0])) / m;
    // oriented_left wants signed_dist >= 0; violated when signed_dist < 0.
    // oriented_right wants signed_dist <= 0; violated when signed_dist > 0.
    return (ineq.kind == "oriented_left") ? -signed_dist : signed_dist;
  }
  return 0.0;
}

struct SolveOnceResult {
  int slvs_result = -1;
  bool solved = false;
  int dof = 0;
  double residual = 0.0;
  std::map<std::string, SolutionType::Point2d> points;
  std::vector<std::string> failed_constraint_names;

  // For active-set loop: handle assigned to each active inequality, so the
  // outer loop can intersect this with sys.failed to know which actives to
  // drop. Keys are indices into the inequalities vector.
  std::map<size_t, Slvs_hConstraint> active_ineq_handles;
  // Indices of active inequalities found in sys.failed.
  std::vector<size_t> failed_active_ineqs;
};

// One Slvs_Solve cycle: build the Slvs_System from `points`, `constraints`,
// and any active inequalities, run the solver, apply REDUNDANT_OKAY residual
// recovery, return results.
// `points` is non-const because each PointDecl receives assigned u_param /
// v_param / entity handles (used here to read back coordinates).
SolveOnceResult build_and_solve_once(
    std::vector<PointDecl>& points,
    const std::map<std::string, size_t>& name_to_idx,
    const std::vector<ConstraintDecl>& constraints,
    const std::vector<InequalityDecl>& inequalities,
    const std::vector<DirectedAngleSeed>& directed_angle_seeds,
    const std::set<size_t>& active_set,
    const Location& loc,
    const std::string& doc_root,
    int attempt = 0,
    double length_scale = 1.0)
{
  SolveOnceResult out;

  // Build phase: assemble Slvs_System
  // Group 1: workplane params (origin + normal). Always fixed.
  // Group 2: all point params and all constraints. Anchoring of individual
  //          points is expressed via con_fixed (SLVS_C_WHERE_DRAGGED), not
  //          group membership.
  const Slvs_hGroup g_fixed = 1;
  const Slvs_hGroup g_solve = 2;

  std::vector<Slvs_Param> sparams;
  std::vector<Slvs_Entity> sentities;
  std::vector<Slvs_Constraint> sconstraints;
  Slvs_hParam next_param = 1;
  Slvs_hEntity next_entity = 200;
  Slvs_hConstraint next_constraint = 1;

  // Workplane: origin at (0,0,0), oriented to XY plane.
  Slvs_hParam ox = next_param++;
  Slvs_hParam oy = next_param++;
  Slvs_hParam oz = next_param++;
  sparams.push_back(Slvs_MakeParam(ox, g_fixed, 0.0));
  sparams.push_back(Slvs_MakeParam(oy, g_fixed, 0.0));
  sparams.push_back(Slvs_MakeParam(oz, g_fixed, 0.0));
  Slvs_hEntity origin_h = next_entity++;
  sentities.push_back(Slvs_MakePoint3d(origin_h, g_fixed, ox, oy, oz));

  double qw, qx_, qy_, qz_;
  Slvs_MakeQuaternion(1, 0, 0, 0, 1, 0, &qw, &qx_, &qy_, &qz_);
  Slvs_hParam qwh = next_param++;
  Slvs_hParam qxh = next_param++;
  Slvs_hParam qyh = next_param++;
  Slvs_hParam qzh = next_param++;
  sparams.push_back(Slvs_MakeParam(qwh, g_fixed, qw));
  sparams.push_back(Slvs_MakeParam(qxh, g_fixed, qx_));
  sparams.push_back(Slvs_MakeParam(qyh, g_fixed, qy_));
  sparams.push_back(Slvs_MakeParam(qzh, g_fixed, qz_));
  Slvs_hEntity normal_h = next_entity++;
  sentities.push_back(Slvs_MakeNormal3d(normal_h, g_fixed, qwh, qxh, qyh, qzh));

  Slvs_hEntity wrkpl = next_entity++;
  sentities.push_back(Slvs_MakeWorkplane(wrkpl, g_fixed, origin_h, normal_h));

  // Points. `at=` supplies a seed (initial guess); points without a seed get
  // a deterministic non-collinear default from default_unseeded_seed (scaled by
  // length_scale so the spiral starts in the same neighbourhood as user seeds
  // and length-bearing constraint values). The `attempt` counter perturbs both
  // the spiral default and any user-provided seeds (jittered_seed); attempt 0
  // honours the user's seed exactly, attempts 1..N-1 jitter by a growing
  // fraction of length_scale so a fully-seeded sketch with inequalities can
  // still escape branch-cut traps (e.g. a directed-angle landed on the wrong
  // half-plane). Points constrained by con_fixed are exempt from jitter
  // because libslvs's SLVS_C_WHERE_DRAGGED pins to the *initial* parameter
  // value — perturbing it would silently un-anchor the user's reference frame.
  // All point params live in g_solve — pinning is the job of con_fixed.
  std::vector<bool> is_fixed(points.size(), false);
  for (const auto& c : constraints) {
    if (c.kind == "fixed" && c.points.size() == 1) {
      auto it = name_to_idx.find(c.points[0]);
      if (it != name_to_idx.end()) is_fixed[it->second] = true;
    }
  }

  // Pass 1: pick a base initial position for each point — verbatim seed at
  // attempt 0, jittered_seed at later attempts (unseeded points get the
  // golden-angle spiral). Store in init_xy so Pass 2 can override the seed
  // for directed_angle target points using the now-known positions of their
  // pivots.
  std::vector<std::pair<double, double>> init_xy(points.size());
  size_t unseeded_idx = 0;
  for (size_t i = 0; i < points.size(); ++i) {
    const auto& p = points[i];
    if (!p.has_seed) {
      init_xy[i] = default_unseeded_seed(unseeded_idx, attempt, length_scale);
      ++unseeded_idx;
    } else if (attempt > 0 && !is_fixed[i]) {
      init_xy[i] = jittered_seed(p.u, p.v, i, attempt, length_scale);
    } else {
      init_xy[i] = {p.u, p.v};
    }
  }

  // Pass 2: rotate each con_directed_angle target onto the angle ray, but
  // only when the current seed is far enough from the target angle that
  // libslvs's Newton can't be expected to traverse the gap. Near 0° or 180°
  // the SLVS_C_ANGLE residual cos(actual)-cos(target) is locally flat in
  // the actual angle, so a 100°+ swing through that ridge regularly traps
  // Newton in a wrong-angle local minimum (REDUNDANT_OKAY → reported as
  // INCONSISTENT) that pure Cartesian jitter doesn't escape. When the seed
  // is already within a 90° wedge of the target the user's seed usually
  // encodes near-correct distances to other anchors — projecting then
  // would preserve the angle but smash those approximations and land
  // Newton in a worse basin. Only run on retry attempts; attempt 0 honours
  // the user's verbatim seed. The radius |p4 - p3| is preserved so any
  // sibling con_distance(p3, p4) converges in one step. Skips: fixed p4
  // (would un-anchor the user's frame), zero-length reference line p1->p2
  // (degenerate).
  if (attempt > 0) {
    constexpr double PROJECT_THRESHOLD_RAD = M_PI / 2.0;  // 90°
    for (const auto& das : directed_angle_seeds) {
      if (das.p4_idx >= points.size() || is_fixed[das.p4_idx]) continue;
      const auto& [p1u, p1v] = init_xy[das.p1_idx];
      const auto& [p2u, p2v] = init_xy[das.p2_idx];
      const auto& [p3u, p3v] = init_xy[das.p3_idx];
      const auto& [p4u, p4v] = init_xy[das.p4_idx];
      const double rx = p2u - p1u, ry = p2v - p1v;
      const double ref_len = std::hypot(rx, ry);
      if (ref_len < 1e-12) continue;
      const double ref_angle = std::atan2(ry, rx);
      const double tgt_angle = ref_angle + das.deg * M_PI / 180.0;
      double radius = std::hypot(p4u - p3u, p4v - p3v);
      if (radius < 1e-12) radius = length_scale;
      const double cur_angle = std::atan2(p4v - p3v, p4u - p3u);
      double diff = std::fmod(tgt_angle - cur_angle, 2.0 * M_PI);
      if (diff < -M_PI) diff += 2.0 * M_PI;
      if (diff > M_PI) diff -= 2.0 * M_PI;
      if (std::abs(diff) < PROJECT_THRESHOLD_RAD) continue;
      init_xy[das.p4_idx] = {p3u + radius * std::cos(tgt_angle),
                             p3v + radius * std::sin(tgt_angle)};
    }
  }

  // Pass 3: emit Slvs_MakeParam / Slvs_MakePoint2d using the chosen seeds.
  for (size_t i = 0; i < points.size(); ++i) {
    auto& p = points[i];
    p.u_param = next_param++;
    p.v_param = next_param++;
    sparams.push_back(Slvs_MakeParam(p.u_param, g_solve, init_xy[i].first));
    sparams.push_back(Slvs_MakeParam(p.v_param, g_solve, init_xy[i].second));
    p.entity = next_entity++;
    sentities.push_back(Slvs_MakePoint2d(p.entity, g_solve, wrkpl, p.u_param, p.v_param));
  }

  // Helper to create line entities on the fly for constraints that need them.
  auto make_line = [&](Slvs_hEntity ptA, Slvs_hEntity ptB) -> Slvs_hEntity {
    Slvs_hEntity h = next_entity++;
    sentities.push_back(Slvs_MakeLineSegment(h, g_solve, wrkpl, ptA, ptB));
    return h;
  };

  // Map a point name to an entity handle, warning and returning 0 on miss.
  auto pt_entity = [&](const std::string& kind, const std::string& name) -> Slvs_hEntity {
    auto it = name_to_idx.find(name);
    if (it == name_to_idx.end()) {
      LOG(message_group::Warning, loc, doc_root,
          "solve2d: con_%1$s constraint references unknown point '%2$s'", kind, name);
      return 0;
    }
    return points[it->second].entity;
  };

  std::vector<std::string> failed_names;
  std::map<Slvs_hConstraint, std::string> constraint_name_by_h;

  for (const auto& c : constraints) {
    // Skip constraints that are suppressed by an active inequality (used by
    // composite expansions like con_pt_on_segment where the on-line equality
    // becomes rank-redundant once a segment endpoint coincidence is enforced).
    bool suppressed = false;
    for (size_t idx : c.suppress_when_active) {
      if (active_set.count(idx)) { suppressed = true; break; }
    }
    if (suppressed) continue;

    Slvs_hConstraint ch = next_constraint++;
    auto register_name = [&](const std::string& summary) {
      constraint_name_by_h[ch] = c.name.empty() ? summary : c.name;
    };

    if (c.kind == "coincident" && c.points.size() == 2) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      if (!a || !b) continue;
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_POINTS_COINCIDENT,
                                                 wrkpl, 0.0, a, b, 0, 0));
      register_name("con_coincident(" + c.points[0] + "," + c.points[1] + ")");
    } else if (c.kind == "distance" && c.points.size() == 2) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      if (!a || !b) continue;
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PT_PT_DISTANCE,
                                                 wrkpl, c.valA, a, b, 0, 0));
      register_name("con_distance(" + c.points[0] + "," + c.points[1] + ")");
    } else if (c.kind == "horizontal" && c.points.size() == 2) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      if (!a || !b) continue;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_HORIZONTAL,
                                                 wrkpl, 0.0, 0, 0, line, 0));
      register_name("con_horizontal(" + c.points[0] + "," + c.points[1] + ")");
    } else if (c.kind == "vertical" && c.points.size() == 2) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      if (!a || !b) continue;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_VERTICAL,
                                                 wrkpl, 0.0, 0, 0, line, 0));
      register_name("con_vertical(" + c.points[0] + "," + c.points[1] + ")");
    } else if (c.kind == "perpendicular" && c.points.size() == 3) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity v = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity ce = pt_entity(c.kind, c.points[2]);
      if (!a || !v || !ce) continue;
      Slvs_hEntity l1 = make_line(a, v);
      Slvs_hEntity l2 = make_line(v, ce);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PERPENDICULAR,
                                                 wrkpl, 0.0, 0, 0, l1, l2));
      register_name("con_perpendicular(" + c.points[0] + "," + c.points[1] + "," + c.points[2] + ")");
    } else if (c.kind == "parallel" && c.points.size() == 4) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity ce = pt_entity(c.kind, c.points[2]);
      Slvs_hEntity d = pt_entity(c.kind, c.points[3]);
      if (!a || !b || !ce || !d) continue;
      Slvs_hEntity l1 = make_line(a, b);
      Slvs_hEntity l2 = make_line(ce, d);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PARALLEL,
                                                 wrkpl, 0.0, 0, 0, l1, l2));
      register_name("con_parallel(" + c.points[0] + "," + c.points[1] + "," +
                    c.points[2] + "," + c.points[3] + ")");
    } else if (c.kind == "angle" && c.points.size() == 4) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity ce = pt_entity(c.kind, c.points[2]);
      Slvs_hEntity d = pt_entity(c.kind, c.points[3]);
      if (!a || !b || !ce || !d) continue;
      Slvs_hEntity l1 = make_line(a, b);
      Slvs_hEntity l2 = make_line(ce, d);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_ANGLE,
                                                 wrkpl, c.valA, 0, 0, l1, l2));
      register_name("con_angle(...)");
    } else if (c.kind == "fixed" && c.points.size() == 1) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      if (!a) continue;
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_WHERE_DRAGGED,
                                                 wrkpl, 0.0, a, 0, 0, 0));
      register_name("con_fixed(" + c.points[0] + ")");
    } else if (c.kind == "pt_on_line" && c.points.size() == 3) {
      Slvs_hEntity p = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity a = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[2]);
      if (!p || !a || !b) continue;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PT_ON_LINE,
                                                 wrkpl, 0.0, p, 0, line, 0));
      register_name("con_pt_on_line(" + c.points[0] + "," + c.points[1] + "," + c.points[2] + ")");
    } else if (c.kind == "pt_line_distance" && c.points.size() == 3) {
      Slvs_hEntity p = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity a = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[2]);
      if (!p || !a || !b) continue;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PT_LINE_DISTANCE,
                                                 wrkpl, c.valA, p, 0, line, 0));
      register_name("con_pt_line_distance(" + c.points[0] + "," + c.points[1] + "," + c.points[2] + ")");
    } else if (c.kind == "at_midpoint" && c.points.size() == 3) {
      Slvs_hEntity p = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity a = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[2]);
      if (!p || !a || !b) continue;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_AT_MIDPOINT,
                                                 wrkpl, 0.0, p, 0, line, 0));
      register_name("con_at_midpoint(" + c.points[0] + "," + c.points[1] + "," + c.points[2] + ")");
    } else if (c.kind == "equal_length" && c.points.size() == 4) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity ce = pt_entity(c.kind, c.points[2]);
      Slvs_hEntity d = pt_entity(c.kind, c.points[3]);
      if (!a || !b || !ce || !d) continue;
      Slvs_hEntity l1 = make_line(a, b);
      Slvs_hEntity l2 = make_line(ce, d);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_EQUAL_LENGTH_LINES,
                                                 wrkpl, 0.0, 0, 0, l1, l2));
      register_name("con_equal_length(" + c.points[0] + "," + c.points[1] + "," +
                    c.points[2] + "," + c.points[3] + ")");
    } else if (c.kind == "length_ratio" && c.points.size() == 4) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity ce = pt_entity(c.kind, c.points[2]);
      Slvs_hEntity d = pt_entity(c.kind, c.points[3]);
      if (!a || !b || !ce || !d) continue;
      Slvs_hEntity l1 = make_line(a, b);
      Slvs_hEntity l2 = make_line(ce, d);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_LENGTH_RATIO,
                                                 wrkpl, c.valA, 0, 0, l1, l2));
      register_name("con_length_ratio(...)");
    } else if (c.kind == "length_difference" && c.points.size() == 4) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity ce = pt_entity(c.kind, c.points[2]);
      Slvs_hEntity d = pt_entity(c.kind, c.points[3]);
      if (!a || !b || !ce || !d) continue;
      Slvs_hEntity l1 = make_line(a, b);
      Slvs_hEntity l2 = make_line(ce, d);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_LENGTH_DIFFERENCE,
                                                 wrkpl, c.valA, 0, 0, l1, l2));
      register_name("con_length_difference(...)");
    } else if (c.kind == "eq_len_pt_line_d" && c.points.size() == 5) {
      Slvs_hEntity p = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity a = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[2]);
      Slvs_hEntity ce = pt_entity(c.kind, c.points[3]);
      Slvs_hEntity d = pt_entity(c.kind, c.points[4]);
      if (!p || !a || !b || !ce || !d) continue;
      Slvs_hEntity lenLine = make_line(a, b);
      Slvs_hEntity distLine = make_line(ce, d);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_EQ_LEN_PT_LINE_D,
                                                 wrkpl, 0.0, p, 0, lenLine, distLine));
      register_name("con_eq_len_pt_line_d(...)");
    } else if (c.kind == "eq_pt_ln_distances" && c.points.size() == 6) {
      Slvs_hEntity p1 = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity a1 = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity b1 = pt_entity(c.kind, c.points[2]);
      Slvs_hEntity p2 = pt_entity(c.kind, c.points[3]);
      Slvs_hEntity a2 = pt_entity(c.kind, c.points[4]);
      Slvs_hEntity b2 = pt_entity(c.kind, c.points[5]);
      if (!p1 || !a1 || !b1 || !p2 || !a2 || !b2) continue;
      Slvs_hEntity l1 = make_line(a1, b1);
      Slvs_hEntity l2 = make_line(a2, b2);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_EQ_PT_LN_DISTANCES,
                                                 wrkpl, 0.0, p1, p2, l1, l2));
      register_name("con_eq_pt_ln_distances(...)");
    } else if (c.kind == "equal_angle" && c.points.size() == 8) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity ce = pt_entity(c.kind, c.points[2]);
      Slvs_hEntity d = pt_entity(c.kind, c.points[3]);
      Slvs_hEntity e = pt_entity(c.kind, c.points[4]);
      Slvs_hEntity f = pt_entity(c.kind, c.points[5]);
      Slvs_hEntity g = pt_entity(c.kind, c.points[6]);
      Slvs_hEntity h = pt_entity(c.kind, c.points[7]);
      if (!a || !b || !ce || !d || !e || !f || !g || !h) continue;
      Slvs_hEntity l1 = make_line(a, b);
      Slvs_hEntity l2 = make_line(ce, d);
      Slvs_hEntity l3 = make_line(e, f);
      Slvs_hEntity l4 = make_line(g, h);
      Slvs_Constraint sc = Slvs_MakeConstraint(ch, g_solve, SLVS_C_EQUAL_ANGLE,
                                               wrkpl, 0.0, 0, 0, l1, l2);
      sc.entityC = l3;
      sc.entityD = l4;
      sconstraints.push_back(sc);
      register_name("con_equal_angle(...)");
    } else if ((c.kind == "symmetric_horiz" || c.kind == "symmetric_vert") &&
               c.points.size() == 2) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      if (!a || !b) continue;
      int type = (c.kind == "symmetric_horiz") ? SLVS_C_SYMMETRIC_HORIZ
                                               : SLVS_C_SYMMETRIC_VERT;
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve, type,
                                                 wrkpl, 0.0, a, b, 0, 0));
      register_name("con_" + c.kind + "(" + c.points[0] + "," + c.points[1] + ")");
    } else if (c.kind == "symmetric_line" && c.points.size() == 4) {
      Slvs_hEntity p1 = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity p2 = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity a = pt_entity(c.kind, c.points[2]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[3]);
      if (!p1 || !p2 || !a || !b) continue;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_SYMMETRIC_LINE,
                                                 wrkpl, 0.0, p1, p2, line, 0));
      register_name("con_symmetric_line(...)");
    } else {
      LOG(message_group::Warning, loc, doc_root,
          "solve2d: malformed con_%1$s constraint", c.kind);
    }
  }

  // Active inequalities: treated as equalities by the solver for this iteration.
  for (size_t idx : active_set) {
    const auto& ineq = inequalities[idx];

    if ((ineq.kind == "le_distance" || ineq.kind == "ge_distance") &&
        ineq.points.size() == 2) {
      Slvs_hEntity a = pt_entity(ineq.kind, ineq.points[0]);
      Slvs_hEntity b = pt_entity(ineq.kind, ineq.points[1]);
      if (!a || !b) continue;
      Slvs_hConstraint ch = next_constraint++;
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PT_PT_DISTANCE,
                                                 wrkpl, ineq.valA, a, b, 0, 0));
      constraint_name_by_h[ch] = ineq.name;
      out.active_ineq_handles[idx] = ch;
    } else if ((ineq.kind == "le_pt_line_distance" ||
                ineq.kind == "ge_pt_line_distance") && ineq.points.size() == 3) {
      Slvs_hEntity p = pt_entity(ineq.kind, ineq.points[0]);
      Slvs_hEntity a = pt_entity(ineq.kind, ineq.points[1]);
      Slvs_hEntity b = pt_entity(ineq.kind, ineq.points[2]);
      if (!p || !a || !b) continue;
      Slvs_hConstraint ch = next_constraint++;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PT_LINE_DISTANCE,
                                                 wrkpl, ineq.valA, p, 0, line, 0));
      constraint_name_by_h[ch] = ineq.name;
      out.active_ineq_handles[idx] = ch;
    } else if ((ineq.kind == "le_length_difference" ||
                ineq.kind == "ge_length_difference") && ineq.points.size() == 4) {
      Slvs_hEntity a = pt_entity(ineq.kind, ineq.points[0]);
      Slvs_hEntity b = pt_entity(ineq.kind, ineq.points[1]);
      Slvs_hEntity c = pt_entity(ineq.kind, ineq.points[2]);
      Slvs_hEntity d = pt_entity(ineq.kind, ineq.points[3]);
      if (!a || !b || !c || !d) continue;
      Slvs_hConstraint ch = next_constraint++;
      Slvs_hEntity l1 = make_line(a, b);
      Slvs_hEntity l2 = make_line(c, d);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_LENGTH_DIFFERENCE,
                                                 wrkpl, ineq.valA, 0, 0, l1, l2));
      constraint_name_by_h[ch] = ineq.name;
      out.active_ineq_handles[idx] = ch;
    } else if ((ineq.kind == "le_angle" || ineq.kind == "ge_angle") &&
               ineq.points.size() == 4) {
      Slvs_hEntity a = pt_entity(ineq.kind, ineq.points[0]);
      Slvs_hEntity b = pt_entity(ineq.kind, ineq.points[1]);
      Slvs_hEntity c = pt_entity(ineq.kind, ineq.points[2]);
      Slvs_hEntity d = pt_entity(ineq.kind, ineq.points[3]);
      if (!a || !b || !c || !d) continue;
      Slvs_hConstraint ch = next_constraint++;
      Slvs_hEntity l1 = make_line(a, b);
      Slvs_hEntity l2 = make_line(c, d);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_ANGLE,
                                                 wrkpl, ineq.valA, 0, 0, l1, l2));
      constraint_name_by_h[ch] = ineq.name;
      out.active_ineq_handles[idx] = ch;
    } else if ((ineq.kind == "pt_on_segment_lower" ||
                ineq.kind == "pt_on_segment_upper") && ineq.points.size() == 3) {
      // When this bound is active, p coincides with the corresponding endpoint.
      // Lower bound active ⇒ p == a; upper bound active ⇒ p == b.
      Slvs_hEntity p = pt_entity(ineq.kind, ineq.points[0]);
      Slvs_hEntity endpoint = pt_entity(
          ineq.kind,
          ineq.kind == "pt_on_segment_lower" ? ineq.points[1] : ineq.points[2]);
      if (!p || !endpoint) continue;
      Slvs_hConstraint ch = next_constraint++;
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_POINTS_COINCIDENT,
                                                 wrkpl, 0.0, p, endpoint, 0, 0));
      constraint_name_by_h[ch] = ineq.name;
      out.active_ineq_handles[idx] = ch;
    } else if ((ineq.kind == "same_side" || ineq.kind == "opposite_side")
               && ineq.points.size() == 4) {
      // Active ⇒ pin p to the line through a and b. Both kinds share this
      // binding form: the dividing line between the two half-planes is the
      // same line ab. q is a sign reference only and never gets pinned.
      Slvs_hEntity a = pt_entity(ineq.kind, ineq.points[0]);
      Slvs_hEntity b = pt_entity(ineq.kind, ineq.points[1]);
      Slvs_hEntity p = pt_entity(ineq.kind, ineq.points[2]);
      // q (ineq.points[3]) is intentionally not fetched: sign reference, not bound.
      if (!a || !b || !p) continue;
      Slvs_hConstraint ch = next_constraint++;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PT_ON_LINE,
                                                 wrkpl, 0.0, p, 0, line, 0));
      constraint_name_by_h[ch] = ineq.name;
      out.active_ineq_handles[idx] = ch;
    } else if ((ineq.kind == "oriented_left" ||
                ineq.kind == "oriented_right") && ineq.points.size() == 3) {
      // Active ⇒ pin p to the line through a and b. Same binding form as
      // same_side; the half-plane sign is implicit in the kind name.
      Slvs_hEntity a = pt_entity(ineq.kind, ineq.points[0]);
      Slvs_hEntity b = pt_entity(ineq.kind, ineq.points[1]);
      Slvs_hEntity p = pt_entity(ineq.kind, ineq.points[2]);
      if (!a || !b || !p) continue;
      Slvs_hConstraint ch = next_constraint++;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PT_ON_LINE,
                                                 wrkpl, 0.0, p, 0, line, 0));
      constraint_name_by_h[ch] = ineq.name;
      out.active_ineq_handles[idx] = ch;
    }
  }

  // Solve
  Slvs_System sys;
  std::memset(&sys, 0, sizeof(sys));
  sys.param = sparams.data();
  sys.params = static_cast<int>(sparams.size());
  sys.entity = sentities.data();
  sys.entities = static_cast<int>(sentities.size());
  sys.constraint = sconstraints.data();
  sys.constraints = static_cast<int>(sconstraints.size());
  std::vector<Slvs_hConstraint> failed_buf(sconstraints.size() ? sconstraints.size() : 1);
  sys.failed = failed_buf.data();
  sys.faileds = static_cast<int>(failed_buf.size());
  sys.calculateFaileds = 1;

  Slvs_Solve(&sys, g_solve);

  out.slvs_result = sys.result;
  out.solved = (sys.result == SLVS_RESULT_OKAY);
  out.dof = sys.dof;

  // Read back point coords by looking up each point's u_param and v_param.
  std::map<Slvs_hParam, double> param_value;
  for (const auto& p : sparams) {
    param_value[p.h] = p.val;
  }
  // sparams data was modified in-place by Slvs_Solve, so re-read from the underlying buffer.
  for (int i = 0; i < sys.params; ++i) {
    param_value[sys.param[i].h] = sys.param[i].val;
  }
  for (const auto& p : points) {
    out.points[p.name] = {param_value[p.u_param], param_value[p.v_param]};
  }

  for (int i = 0; i < sys.faileds; ++i) {
    Slvs_hConstraint h = sys.failed[i];
    auto it = constraint_name_by_h.find(h);
    if (it != constraint_name_by_h.end()) {
      out.failed_constraint_names.push_back(it->second);
    } else {
      out.failed_constraint_names.push_back("constraint #" + std::to_string(h));
    }
    // Check if this failed handle is one of our active inequalities.
    for (const auto& [idx, hh] : out.active_ineq_handles) {
      if (hh == h) out.failed_active_ineqs.push_back(idx);
    }
  }

  // Walk every constraint's residual and judge it against a unit-appropriate
  // tolerance scaled by length_scale. This recovers from libslvs's
  // REDUNDANT_OKAY → INCONSISTENT mapping (see
  // submodules/SolveSpaceLib/libslvs/lib.cpp:234-237) when Newton actually
  // converged but the Jacobian was rank-deficient: every residual is small, so
  // we promote to solved. The per-unit threshold keeps a 1% slop on a 1000-unit
  // distance from being held to the same absolute bound as a 1° angle, and
  // makes mm/μm sketches behave the same as unit-scale ones.
  double max_residual = 0.0;
  bool all_within_threshold = true;
  auto judge = [&](const ConstraintDecl& c) {
    double r = constraint_residual(c, out.points);
    if (r > max_residual) max_residual = r;
    if (r > residual_threshold(residual_unit_for(c.kind), length_scale)) {
      all_within_threshold = false;
    }
  };
  for (const auto& c : constraints) {
    judge(c);
  }
  // Active inequalities are treated as equalities by the solver, so include
  // their residuals in the gate too. Same pseudo-constraint mapping as before.
  for (size_t idx : active_set) {
    const auto& ineq = inequalities[idx];
    ConstraintDecl pseudo;
    pseudo.valA = ineq.valA;
    if (ineq.kind == "le_distance" || ineq.kind == "ge_distance") {
      pseudo.kind = "distance";
      pseudo.points = ineq.points;
    } else if (ineq.kind == "le_pt_line_distance" || ineq.kind == "ge_pt_line_distance") {
      pseudo.kind = "pt_line_distance";
      pseudo.points = ineq.points;
    } else if (ineq.kind == "le_length_difference" || ineq.kind == "ge_length_difference") {
      pseudo.kind = "length_difference";
      pseudo.points = ineq.points;
    } else if (ineq.kind == "le_angle" || ineq.kind == "ge_angle") {
      pseudo.kind = "angle";
      pseudo.points = ineq.points;
    } else if (ineq.kind == "pt_on_segment_lower" && ineq.points.size() == 3) {
      // Active ⇒ p coincides with a. Pseudo: coincident{p, a}.
      pseudo.kind = "coincident";
      pseudo.points = {ineq.points[0], ineq.points[1]};
    } else if (ineq.kind == "pt_on_segment_upper" && ineq.points.size() == 3) {
      // Active ⇒ p coincides with b. Pseudo: coincident{p, b}.
      pseudo.kind = "coincident";
      pseudo.points = {ineq.points[0], ineq.points[2]};
    } else if ((ineq.kind == "same_side" || ineq.kind == "opposite_side")
               && ineq.points.size() == 4) {
      // Active ⇒ p lies on line ab. Pseudo: pt_on_line{p, a, b}.
      pseudo.kind = "pt_on_line";
      pseudo.points = {ineq.points[2], ineq.points[0], ineq.points[1]};
    } else {
      continue;
    }
    judge(pseudo);
  }
  out.residual = max_residual;

  if (sys.result == SLVS_RESULT_INCONSISTENT && all_within_threshold) {
    out.solved = true;
    out.failed_constraint_names.clear();
    out.failed_active_ineqs.clear();   // don't punish a recovered solve
  }

  return out;
}

constexpr int MAX_OUTER_ITERATIONS = 50;

struct SolveLoopResult {
  SolveOnceResult last;          // final solve's results
  int iterations = 0;            // outer-loop iteration count
  std::set<size_t> active_set;   // final active set
  bool converged = false;        // true ⇒ feasible solution found
  std::string failure_reason;    // "cycle" / "max_iter" / "infeasible_base" / ""
};

SolveLoopResult solve_with_inequalities(
    std::vector<PointDecl>& points,
    const std::map<std::string, size_t>& name_to_idx,
    const std::vector<ConstraintDecl>& constraints,
    const std::vector<InequalityDecl>& inequalities,
    const std::vector<DirectedAngleSeed>& directed_angle_seeds,
    const Location& loc,
    const std::string& doc_root,
    int attempt = 0,
    double length_scale = 1.0)
{
  SolveLoopResult result;
  std::set<std::set<size_t>> visited;
  // All violations are now in length units (signed distance). Threshold scales
  // with sketch so a μm sketch doesn't mistake unavoidable rounding for a
  // 1nm-deep violation, and a km sketch isn't allowed to drift 1nm off a line
  // before the active-set kicks in.
  const double violation_tol = VIOLATION_TOL_REL * length_scale;

  for (int iter = 0; iter < MAX_OUTER_ITERATIONS; ++iter) {
    if (visited.count(result.active_set)) {
      result.failure_reason = "cycle";
      return result;
    }
    visited.insert(result.active_set);

    result.iterations = iter + 1;
    result.last = build_and_solve_once(points, name_to_idx, constraints,
                                       inequalities, directed_angle_seeds,
                                       result.active_set,
                                       loc, doc_root, attempt, length_scale);

    if (result.last.solved) {
      // Check inactive inequalities for violations.
      std::vector<size_t> violators;
      for (size_t i = 0; i < inequalities.size(); ++i) {
        if (result.active_set.count(i)) continue;
        double v = inequality_violation(inequalities[i], result.last.points);
        if (v > violation_tol) violators.push_back(i);
      }
      if (violators.empty()) {
        result.converged = true;
        return result;
      }
      for (size_t i : violators) result.active_set.insert(i);
    } else {
      // Slvs failed; drop SolveSpace-flagged active inequalities.
      if (result.last.failed_active_ineqs.empty()) {
        result.failure_reason = "infeasible_base";
        return result;
      }
      for (size_t i : result.last.failed_active_ineqs) result.active_set.erase(i);
    }
  }

  result.failure_reason = "max_iter";
  return result;
}

}  // namespace

Value builtin_solve2d(Arguments arguments, const Location& loc)
{
  // libslvs (SolveSpace) keeps internal global state across Slvs_Solve calls
  // (e.g. SolveSpace::Param IdList allocator, Expr children pool). Concurrent
  // calls — possible now that animation pre-fetch workers re-evaluate the
  // script in parallel — corrupt that state and abort. Serialise the whole
  // builtin behind a single mutex; this is safe and the cost is negligible
  // because solve2d is already the heavy operation in any frame that uses it.
  static std::mutex slvs_mutex;
  const std::lock_guard<std::mutex> slvs_lock(slvs_mutex);

  EvaluationSession *session = arguments.session();
  const std::string doc_root = arguments.documentRoot();

  Parameters params = Parameters::parse(std::move(arguments), loc, {"items"}, {"solve"});
  if (params["items"].type() != Value::Type::VECTOR) {
    LOG(message_group::Warning, loc, params.documentRoot(),
        "solve2d() expects a single vector of entities and constraints");
    return Value::undefined.clone();
  }

  bool do_solve = true;
  if (params["solve"].type() == Value::Type::BOOL) {
    do_solve = params["solve"].toBool();
  } else if (params["solve"].type() != Value::Type::UNDEFINED) {
    LOG(message_group::Warning, loc, params.documentRoot(),
        "solve2d() solve= must be a bool");
  }

  std::vector<PointDecl> points;
  std::map<std::string, size_t> name_to_idx;
  std::vector<ConstraintDecl> constraints;
  std::vector<InequalityDecl> inequalities;
  std::vector<DirectedAngleSeed> directed_angle_seeds;

  // Parse phase
  const VectorType& items = params["items"].toVector();
  for (const auto& item : items) {
    std::string kind;
    if (!object_kind(item, kind)) {
      LOG(message_group::Warning, loc, doc_root,
          "solve2d: input contains a non-entity, non-constraint item");
      continue;
    }
    const ObjectType& obj = item.toObject();
    if (kind == "point") {
      std::string name;
      if (!field_string(obj, "name", name)) continue;
      if (name_to_idx.count(name)) {
        LOG(message_group::Warning, loc, doc_root,
            "solve2d: duplicate point name '%1$s'", name);
        continue;
      }
      PointDecl p;
      p.name = name;
      double x, y;
      if (field_xy(obj, "at", x, y)) {
        p.has_seed = true;
        p.u = x;
        p.v = y;
      }
      name_to_idx[name] = points.size();
      points.push_back(std::move(p));
    } else {
      ConstraintDecl c;
      c.kind = kind;
      auto str_field = [&](const char *f) {
        std::string s;
        if (field_string(obj, f, s)) c.points.push_back(s);
      };
      if (kind == "coincident" || kind == "horizontal" || kind == "vertical") {
        str_field("a");
        str_field("b");
      } else if (kind == "distance") {
        str_field("a");
        str_field("b");
        field_double(obj, "d", c.valA);
      } else if (kind == "perpendicular") {
        str_field("a");
        str_field("vertex");
        str_field("c");
      } else if (kind == "parallel") {
        str_field("a");
        str_field("b");
        str_field("c");
        str_field("d");
      } else if (kind == "angle") {
        str_field("a");
        str_field("b");
        str_field("c");
        str_field("d");
        field_double(obj, "deg", c.valA);
      } else if (kind == "fixed") {
        str_field("a");
      } else if (kind == "pt_on_line" || kind == "at_midpoint") {
        str_field("p");
        str_field("a");
        str_field("b");
      } else if (kind == "pt_line_distance") {
        str_field("p");
        str_field("a");
        str_field("b");
        field_double(obj, "d", c.valA);
      } else if (kind == "equal_length") {
        str_field("a");
        str_field("b");
        str_field("c");
        str_field("d");
      } else if (kind == "length_ratio") {
        str_field("a");
        str_field("b");
        str_field("c");
        str_field("d");
        field_double(obj, "r", c.valA);
      } else if (kind == "length_difference") {
        str_field("a");
        str_field("b");
        str_field("c");
        str_field("d");
        field_double(obj, "diff", c.valA);
      } else if (kind == "eq_len_pt_line_d") {
        str_field("p");
        str_field("a");
        str_field("b");
        str_field("c");
        str_field("d");
      } else if (kind == "eq_pt_ln_distances") {
        str_field("p1");
        str_field("a1");
        str_field("b1");
        str_field("p2");
        str_field("a2");
        str_field("b2");
      } else if (kind == "equal_angle") {
        str_field("a");
        str_field("b");
        str_field("c");
        str_field("d");
        str_field("e");
        str_field("f");
        str_field("g");
        str_field("h");
      } else if (kind == "symmetric_horiz" || kind == "symmetric_vert") {
        str_field("a");
        str_field("b");
      } else if (kind == "symmetric_line") {
        str_field("p1");
        str_field("p2");
        str_field("a");
        str_field("b");
      } else if (kind == "le_distance" || kind == "ge_distance") {
        InequalityDecl ineq;
        ineq.kind = kind;
        std::string a, b;
        field_string(obj, "a", a);
        field_string(obj, "b", b);
        ineq.points.push_back(a);
        ineq.points.push_back(b);
        field_double(obj, "d", ineq.valA);
        const char *fn = (kind == "le_distance") ? "con_le_distance" : "con_ge_distance";
        ineq.name = std::string(fn) + "(" + a + "," + b + "," +
                    std::to_string(ineq.valA) + ")";
        inequalities.push_back(std::move(ineq));
        continue;  // skip the constraints.push_back below
      } else if (kind == "le_pt_line_distance" || kind == "ge_pt_line_distance") {
        InequalityDecl ineq;
        ineq.kind = kind;
        std::string p, a, b;
        field_string(obj, "p", p);
        field_string(obj, "a", a);
        field_string(obj, "b", b);
        ineq.points.push_back(p);
        ineq.points.push_back(a);
        ineq.points.push_back(b);
        field_double(obj, "d", ineq.valA);
        const char *fn = (kind == "le_pt_line_distance") ? "con_le_pt_line_distance"
                                                         : "con_ge_pt_line_distance";
        ineq.name = std::string(fn) + "(" + p + "," + a + "," + b + "," +
                    std::to_string(ineq.valA) + ")";
        inequalities.push_back(std::move(ineq));
        continue;
      } else if (kind == "le_length_difference" || kind == "ge_length_difference") {
        InequalityDecl ineq;
        ineq.kind = kind;
        std::string a, b, c_, d_;
        field_string(obj, "a", a);
        field_string(obj, "b", b);
        field_string(obj, "c", c_);
        field_string(obj, "d", d_);
        ineq.points.push_back(a);
        ineq.points.push_back(b);
        ineq.points.push_back(c_);
        ineq.points.push_back(d_);
        field_double(obj, "diff", ineq.valA);
        const char *fn = (kind == "le_length_difference") ? "con_le_length_difference"
                                                          : "con_ge_length_difference";
        ineq.name = std::string(fn) + "(" + a + "," + b + "," + c_ + "," + d_ + "," +
                    std::to_string(ineq.valA) + ")";
        inequalities.push_back(std::move(ineq));
        continue;
      } else if (kind == "le_angle" || kind == "ge_angle") {
        InequalityDecl ineq;
        ineq.kind = kind;
        std::string a, b, c_, d_;
        field_string(obj, "a", a);
        field_string(obj, "b", b);
        field_string(obj, "c", c_);
        field_string(obj, "d", d_);
        ineq.points.push_back(a);
        ineq.points.push_back(b);
        ineq.points.push_back(c_);
        ineq.points.push_back(d_);
        field_double(obj, "deg", ineq.valA);
        const char *fn = (kind == "le_angle") ? "con_le_angle" : "con_ge_angle";
        ineq.name = std::string(fn) + "(" + a + "," + b + "," + c_ + "," + d_ + "," +
                    std::to_string(ineq.valA) + ")";
        inequalities.push_back(std::move(ineq));
        continue;
      } else if (kind == "same_side" || kind == "opposite_side") {
        InequalityDecl ineq;
        ineq.kind = kind;
        std::string a, b, p, q;
        field_string(obj, "a", a);
        field_string(obj, "b", b);
        field_string(obj, "p", p);
        field_string(obj, "q", q);
        ineq.points.push_back(a);
        ineq.points.push_back(b);
        ineq.points.push_back(p);
        ineq.points.push_back(q);
        ineq.name = (kind == "same_side" ? "con_same_side(" : "con_opposite_side(") +
                    a + "," + b + "," + p + "," + q + ")";
        inequalities.push_back(std::move(ineq));
        continue;
      } else if (kind == "pt_on_segment") {
        std::string p, a, b;
        field_string(obj, "p", p);
        field_string(obj, "a", a);
        field_string(obj, "b", b);
        const std::string prefix = "con_pt_on_segment(" + p + "," + a + "," + b + ")";

        // The lower/upper inequalities will be appended next, so their indices
        // are known up front. They're recorded on the on_line equality so
        // build_and_solve_once can suppress on_line whenever a bound is active
        // (POINTS_COINCIDENT(p, endpoint) implies p on line — emitting both
        // makes SolveSpace bail with rank-deficiency before Newton runs).
        const size_t lower_idx = inequalities.size();
        const size_t upper_idx = inequalities.size() + 1;

        // 1. Always-on equality: p lies on the infinite line through a, b.
        ConstraintDecl on_line;
        on_line.kind = "pt_on_line";
        on_line.points = {p, a, b};
        on_line.name = prefix + ":on_line";
        on_line.suppress_when_active = {lower_idx, upper_idx};
        constraints.push_back(std::move(on_line));

        // 2. Lower bound: g_lower = -dot(p - a, b - a) <= 0  (i.e. t >= 0).
        InequalityDecl lower;
        lower.kind = "pt_on_segment_lower";
        lower.points = {p, a, b};
        lower.name = prefix + ":start";
        inequalities.push_back(std::move(lower));

        // 3. Upper bound: g_upper = dot(p - b, b - a) <= 0  (i.e. t <= 1).
        InequalityDecl upper;
        upper.kind = "pt_on_segment_upper";
        upper.points = {p, a, b};
        upper.name = prefix + ":end";
        inequalities.push_back(std::move(upper));

        continue;
      } else if (kind == "directed_angle") {
        std::string a, b, c_, d_;
        field_string(obj, "a", a);
        field_string(obj, "b", b);
        field_string(obj, "c", c_);
        field_string(obj, "d", d_);
        double deg = 0.0;
        field_double(obj, "deg", deg);
        const std::string prefix = "con_directed_angle(" + a + "," + b + "," +
                                   c_ + "," + d_ + "," + std::to_string(deg) +
                                   ")";

        // Wrap into [0, 360) so users passing 360-ε (rounded from a fraction
        // like 360 * (1 - 1e-16)), -90, or 720 don't get silently dropped.
        // fmod(360.0, 360.0) is exactly 0.0 so the wrap collapses cleanly.
        deg = std::fmod(deg, 360.0);
        if (deg < 0.0) deg += 360.0;

        // Magnitude in [0, 180] for the line-line equality. The half-plane
        // disambiguates which of the two solutions Newton converges to.
        const double magnitude = (deg <= 180.0) ? deg : 360.0 - deg;

        ConstraintDecl angle_c;
        angle_c.kind = "angle";
        angle_c.points = {a, b, c_, d_};
        angle_c.valA = magnitude;
        angle_c.name = prefix + ":angle";
        constraints.push_back(std::move(angle_c));

        // At deg == 0 (parallel same direction) and deg == 180 (parallel
        // opposite direction) the half-plane is undefined — the two rays
        // are colinear and there's no left/right. con_angle alone with
        // magnitude 0 or 180 already enforces both orientations correctly.
        // The window is widened to 1e-3° (was 1e-9°) so floating-point wobble
        // near the parallel branches doesn't add a side inequality whose
        // cross-product violation magnitude is dominated by sin(ε)·|v|² noise.
        constexpr double PARALLEL_EPS = 1e-3;
        if (std::abs(deg) > PARALLEL_EPS &&
            std::abs(deg - 180.0) > PARALLEL_EPS) {
          // CCW angle from line p1→p2 to line p3→p4: deg ∈ (0, 180) lands
          // p4 on the LEFT of directed line p1→p2 (positive cross
          // product); deg ∈ (180, 360) lands it on the RIGHT (negative
          // cross product). When activated, the inequality pins p4 to
          // line p1p2, which conflicts with the magnitude angle equality
          // and forces the active-set loop to bail; multi-start (with at
          // least one unseeded point) then perturbs the seeds to land on
          // the correct branch.
          InequalityDecl side;
          side.kind = (deg < 180.0) ? "oriented_left" : "oriented_right";
          side.points = {a, b, d_};
          side.name = prefix + ":side";
          inequalities.push_back(std::move(side));
        }

        // Record the directed angle so retry attempts can rotate p4's seed
        // onto the angle ray. Resolved point names → indices are needed at
        // seed time; skip silently if any name is unknown (the angle/side
        // expansion above will already have queued a useful warning).
        auto p1_it = name_to_idx.find(a);
        auto p2_it = name_to_idx.find(b);
        auto p3_it = name_to_idx.find(c_);
        auto p4_it = name_to_idx.find(d_);
        if (p1_it != name_to_idx.end() && p2_it != name_to_idx.end() &&
            p3_it != name_to_idx.end() && p4_it != name_to_idx.end()) {
          DirectedAngleSeed das;
          das.p1_idx = p1_it->second;
          das.p2_idx = p2_it->second;
          das.p3_idx = p3_it->second;
          das.p4_idx = p4_it->second;
          das.deg = deg;
          directed_angle_seeds.push_back(das);
        }

        continue;
      } else {
        LOG(message_group::Warning, loc, doc_root,
            "solve2d: unknown item kind '%1$s'", kind);
        continue;
      }
      constraints.push_back(std::move(c));
    }
  }

  auto data = std::make_shared<SolutionType::Data>();

  // Median magnitude across user seeds and length-bearing constraint values.
  // Used to scale residual/violation tolerances and the unseeded-seed spiral
  // so a sketch in mm vs. m vs. μm produces identical solver behaviour.
  const double length_scale = compute_length_scale(points, constraints, inequalities);

  // solve=false short-circuit: report each point at its seed (the `at=`
  // value, or the deterministic golden-angle default for unseeded points)
  // without invoking the solver. Useful for previewing a sketch's initial
  // layout before constraints take effect. solved(sol) is false because no
  // solve has happened.
  if (!do_solve) {
    size_t unseeded_idx = 0;
    for (const auto& p : points) {
      double u = p.u, v = p.v;
      if (!p.has_seed) {
        auto [u0, v0] = default_unseeded_seed(unseeded_idx++, /*attempt=*/0, length_scale);
        u = u0; v = v0;
      }
      data->points[p.name] = {u, v};
      data->ordered_names.push_back(p.name);
    }
    data->result_code = -1;
    data->solved = false;
    data->dof = 0;
    data->residual = 0.0;
    data->iterations = 0;
    return Value(SolutionPtr(SolutionType(std::move(data))));
  }

  // Multi-start: attempt 0 uses the user's seeds verbatim (and the
  // deterministic golden-angle default for unseeded points). Attempts 1..N-1
  // perturb both — unseeded points get a different basin, seeded points get a
  // small jitter (~0.1% of length_scale, see jittered_seed). This second
  // behaviour is what rescues fully-seeded sketches with inequalities (e.g.
  // a directed-angle decomposition that needs to flip half-planes); without
  // it, a fully-seeded sketch would have no degree of freedom to escape a
  // wrong-branch first attempt. Retry attempts also rotate the target point
  // of each con_directed_angle onto its angle ray (see build_and_solve_once
  // Pass 2), so the libslvs Newton doesn't have to rediscover a 100°+ swing
  // through a cosine residual that is locally flat near 0° and 180°.
  bool any_unseeded = false;
  for (const auto& p : points) if (!p.has_seed) { any_unseeded = true; break; }
  constexpr int MAX_SEED_ATTEMPTS = 8;
  const bool has_inequalities = !inequalities.empty();
  const int max_attempts = (any_unseeded || has_inequalities) ? MAX_SEED_ATTEMPTS : 1;

  if (inequalities.empty()) {
    SolveOnceResult once;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
      once = build_and_solve_once(points, name_to_idx, constraints,
                                  inequalities, directed_angle_seeds,
                                  /*active_set=*/{},
                                  loc, doc_root, attempt, length_scale);
      if (once.solved) break;
    }
    data->result_code = once.slvs_result;
    data->solved = once.solved;
    data->dof = once.dof;
    data->residual = once.residual;
    data->points = once.points;
    for (const auto& p : points) data->ordered_names.push_back(p.name);
    data->failed_constraints = once.failed_constraint_names;
    data->iterations = 0;
  } else {
    SolveLoopResult loop;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
      loop = solve_with_inequalities(points, name_to_idx, constraints,
                                     inequalities, directed_angle_seeds,
                                     loc, doc_root, attempt, length_scale);
      if (loop.converged) break;
    }
    data->result_code = loop.last.slvs_result;
    data->solved = loop.converged;
    data->dof = loop.last.dof;
    data->residual = loop.last.residual;
    data->points = loop.last.points;
    for (const auto& p : points) data->ordered_names.push_back(p.name);
    data->failed_constraints = loop.last.failed_constraint_names;
    data->iterations = loop.iterations;
    for (size_t idx : loop.active_set) {
      data->active_inequalities.push_back(inequalities[idx].name);
    }
    if (!loop.converged && data->failed_constraints.empty()) {
      data->failed_constraints.push_back("solve2d: " + loop.failure_reason);
    }
  }

  return Value(SolutionPtr(SolutionType(std::move(data))));
}

// ---------------------------------------------------------------------------
// Solution accessors
// ---------------------------------------------------------------------------

namespace {

bool require_solution(const char *fn, const Arguments& arguments, const Location& loc, size_t expected_count)
{
  if (arguments.size() != expected_count) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "%1$s() expects %2$d argument(s)", fn, static_cast<int>(expected_count));
    return false;
  }
  if (arguments[0]->type() != Value::Type::SOLUTION) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "%1$s() expects a Solution as the first argument", fn);
    return false;
  }
  return true;
}

}  // namespace

Value builtin_solved(Arguments arguments, const Location& loc)
{
  if (!require_solution("solved", arguments, loc, 1)) return Value::undefined.clone();
  return Value(arguments[0]->toSolution().solved());
}

Value builtin_residual(Arguments arguments, const Location& loc)
{
  if (!require_solution("residual", arguments, loc, 1)) return Value::undefined.clone();
  return Value(arguments[0]->toSolution().residual());
}

Value builtin_dof(Arguments arguments, const Location& loc)
{
  if (!require_solution("dof", arguments, loc, 1)) return Value::undefined.clone();
  return Value(static_cast<double>(arguments[0]->toSolution().dof()));
}

Value builtin_pt(Arguments arguments, const Location& loc)
{
  if (!require_solution("pt", arguments, loc, 2)) return Value::undefined.clone();
  if (arguments[1]->type() != Value::Type::STRING) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "pt() expects a string point name as the second argument");
    return Value::undefined.clone();
  }
  const SolutionType& sol = arguments[0]->toSolution();
  std::string name = arguments[1]->toString();
  auto p = sol.point(name);
  if (!p) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "pt(): solution has no point named '%1$s'", name);
    return Value::undefined.clone();
  }
  VectorType vec(arguments.session());
  vec.reserve(2);
  vec.emplace_back((*p)[0]);
  vec.emplace_back((*p)[1]);
  return Value(std::move(vec));
}

Value builtin_pts(Arguments arguments, const Location& loc)
{
  if (!require_solution("pts", arguments, loc, 1)) return Value::undefined.clone();
  const SolutionType& sol = arguments[0]->toSolution();
  VectorType result(arguments.session());
  result.reserve(sol.ordered_names().size());
  for (const auto& name : sol.ordered_names()) {
    auto p = sol.point(name);
    if (!p) continue;
    VectorType pt(arguments.session());
    pt.reserve(2);
    pt.emplace_back((*p)[0]);
    pt.emplace_back((*p)[1]);
    result.emplace_back(std::move(pt));
  }
  return Value(std::move(result));
}

Value builtin_poly(Arguments arguments, const Location& loc)
{
  if (!require_solution("poly", arguments, loc, 2)) return Value::undefined.clone();
  if (arguments[1]->type() != Value::Type::VECTOR) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "poly() expects a vector of point names as the second argument");
    return Value::undefined.clone();
  }
  const SolutionType& sol = arguments[0]->toSolution();
  const VectorType& names = arguments[1]->toVector();
  VectorType result(arguments.session());
  for (const auto& n : names) {
    if (n.type() != Value::Type::STRING) {
      LOG(message_group::Warning, loc, arguments.documentRoot(),
          "poly(): point name list must contain only strings");
      return Value::undefined.clone();
    }
    auto p = sol.point(n.toString());
    if (!p) {
      LOG(message_group::Warning, loc, arguments.documentRoot(),
          "poly(): solution has no point named '%1$s'", n.toString());
      return Value::undefined.clone();
    }
    VectorType pt(arguments.session());
    pt.reserve(2);
    pt.emplace_back((*p)[0]);
    pt.emplace_back((*p)[1]);
    result.emplace_back(std::move(pt));
  }
  return Value(std::move(result));
}

Value builtin_failed_constraints(Arguments arguments, const Location& loc)
{
  if (!require_solution("failed_constraints", arguments, loc, 1)) {
    return Value::undefined.clone();
  }
  const auto& failed = arguments[0]->toSolution().failed_constraints();
  VectorType result(arguments.session());
  result.reserve(failed.size());
  for (const auto& s : failed) {
    result.emplace_back(s);
  }
  return Value(std::move(result));
}

Value builtin_iterations(Arguments arguments, const Location& loc)
{
  if (!require_solution("iterations", arguments, loc, 1)) return Value::undefined.clone();
  return Value(static_cast<double>(arguments[0]->toSolution().iterations()));
}

Value builtin_active_inequalities(Arguments arguments, const Location& loc)
{
  if (!require_solution("active_inequalities", arguments, loc, 1)) {
    return Value::undefined.clone();
  }
  const auto& items = arguments[0]->toSolution().active_inequalities();
  VectorType result(arguments.session());
  result.reserve(items.size());
  for (const auto& s : items) result.emplace_back(s);
  return Value(std::move(result));
}

Value builtin_angle(Arguments arguments, const Location& loc)
{
  if (!require_solution("angle", arguments, loc, 2)) return Value::undefined.clone();
  if (arguments[1]->type() != Value::Type::VECTOR) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "angle() expects [a, b, c] as the second argument");
    return Value::undefined.clone();
  }
  const VectorType& names = arguments[1]->toVector();
  if (names.size() != 3) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "angle() expects exactly three point names");
    return Value::undefined.clone();
  }
  std::string n[3];
  for (int i = 0; i < 3; ++i) {
    if (names[i].type() != Value::Type::STRING) {
      LOG(message_group::Warning, loc, arguments.documentRoot(),
          "angle() point name list must contain only strings");
      return Value::undefined.clone();
    }
    n[i] = names[i].toString();
  }
  const SolutionType& sol = arguments[0]->toSolution();
  SolutionType::Point2d p[3];
  for (int i = 0; i < 3; ++i) {
    auto opt = sol.point(n[i]);
    if (!opt) {
      LOG(message_group::Warning, loc, arguments.documentRoot(),
          "angle(): solution has no point named '%1$s'", n[i]);
      return Value::undefined.clone();
    }
    p[i] = *opt;
  }
  const double v1x = p[0][0] - p[1][0], v1y = p[0][1] - p[1][1];
  const double v2x = p[2][0] - p[1][0], v2y = p[2][1] - p[1][1];
  constexpr double DEGENERATE = 1e-12;
  if (std::sqrt(v1x*v1x + v1y*v1y) < DEGENERATE ||
      std::sqrt(v2x*v2x + v2y*v2y) < DEGENERATE) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "angle() degenerate: ray endpoint coincides with vertex '%1$s'", n[1]);
    return Value::undefined.clone();
  }
  double raw = std::atan2(v1x*v2y - v1y*v2x, v1x*v2x + v1y*v2y);
  if (raw < 0) raw += 2.0 * M_PI;
  return Value(raw * 180.0 / M_PI);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_builtin_solve()
{
  Builtins::init("solve2d", new BuiltinFunction(&builtin_solve2d),
                 {"solve2d(items) -> Solution",
                  "solve2d(items, solve=false) -> Solution at seed positions (no solver run)"});

  Builtins::init("point", new BuiltinFunction(&builtin_point),
                 {"point(name) -> sketch entity",
                  "point(name, at=[x,y]) -> sketch entity (at= seeds initial position; use con_fixed to pin)"});

  Builtins::init("con_coincident", new BuiltinFunction(&builtin_con_coincident),
                 {"con_coincident(p1, p2) -> sketch constraint"});
  Builtins::init("con_distance", new BuiltinFunction(&builtin_con_distance),
                 {"con_distance(p1, p2, d) -> sketch constraint"});
  Builtins::init("con_le_distance", new BuiltinFunction(&builtin_con_le_distance),
                 {"con_le_distance(p1, p2, d) -> sketch constraint (|p1p2| <= d)"});
  Builtins::init("con_ge_distance", new BuiltinFunction(&builtin_con_ge_distance),
                 {"con_ge_distance(p1, p2, d) -> sketch constraint (|p1p2| >= d)"});
  Builtins::init("con_le_pt_line_distance",
                 new BuiltinFunction(&builtin_con_le_pt_line_distance),
                 {"con_le_pt_line_distance(p, la, lb, d) -> sketch constraint (signed dist <= d)"});
  Builtins::init("con_ge_pt_line_distance",
                 new BuiltinFunction(&builtin_con_ge_pt_line_distance),
                 {"con_ge_pt_line_distance(p, la, lb, d) -> sketch constraint (signed dist >= d)"});
  Builtins::init("con_horizontal", new BuiltinFunction(&builtin_con_horizontal),
                 {"con_horizontal(p1, p2) -> sketch constraint"});
  Builtins::init("con_vertical", new BuiltinFunction(&builtin_con_vertical),
                 {"con_vertical(p1, p2) -> sketch constraint"});
  Builtins::init("con_perpendicular", new BuiltinFunction(&builtin_con_perpendicular),
                 {"con_perpendicular(p1, vertex, p2) -> sketch constraint"});
  Builtins::init("con_parallel", new BuiltinFunction(&builtin_con_parallel),
                 {"con_parallel(p1, p2, p3, p4) -> sketch constraint"});
  Builtins::init("con_angle", new BuiltinFunction(&builtin_con_angle),
                 {"con_angle(p1, p2, p3, p4, deg) -> sketch constraint"});
  Builtins::init("con_le_angle",
                 new BuiltinFunction(&builtin_con_le_angle),
                 {"con_le_angle(p1, p2, p3, p4, deg) -> sketch constraint (angle <= deg)"});
  Builtins::init("con_ge_angle",
                 new BuiltinFunction(&builtin_con_ge_angle),
                 {"con_ge_angle(p1, p2, p3, p4, deg) -> sketch constraint (angle >= deg)"});
  Builtins::init("con_directed_angle",
                 new BuiltinFunction(&builtin_con_directed_angle),
                 {"con_directed_angle(p1, p2, p3, p4, deg) -> sketch constraint (CCW angle in [0,360))"});
  Builtins::init("con_fixed", new BuiltinFunction(&builtin_con_fixed),
                 {"con_fixed(p) -> sketch constraint"});

  Builtins::init("con_pt_on_line", new BuiltinFunction(&builtin_con_pt_on_line),
                 {"con_pt_on_line(p, la, lb) -> sketch constraint"});
  Builtins::init("con_pt_on_segment", new BuiltinFunction(&builtin_con_pt_on_segment),
                 {"con_pt_on_segment(p, la, lb) -> sketch constraint (p on closed segment la-lb)"});
  Builtins::init("con_same_side", new BuiltinFunction(&builtin_con_same_side),
                 {"con_same_side(a, b, p, q) -> sketch constraint (p on same side of line ab as q)"});
  Builtins::init("con_opposite_side", new BuiltinFunction(&builtin_con_opposite_side),
                 {"con_opposite_side(a, b, p, q) -> sketch constraint (p on opposite side of line ab from q)"});
  Builtins::init("con_pt_line_distance", new BuiltinFunction(&builtin_con_pt_line_distance),
                 {"con_pt_line_distance(p, la, lb, d) -> sketch constraint (signed)"});
  Builtins::init("con_at_midpoint", new BuiltinFunction(&builtin_con_at_midpoint),
                 {"con_at_midpoint(m, la, lb) -> sketch constraint"});
  Builtins::init("con_equal_length", new BuiltinFunction(&builtin_con_equal_length),
                 {"con_equal_length(a, b, c, d) -> sketch constraint"});
  Builtins::init("con_length_ratio", new BuiltinFunction(&builtin_con_length_ratio),
                 {"con_length_ratio(a, b, c, d, r) -> sketch constraint (|ab|/|cd|=r)"});
  Builtins::init("con_length_difference", new BuiltinFunction(&builtin_con_length_difference),
                 {"con_length_difference(a, b, c, d, diff) -> sketch constraint (|ab|-|cd|=diff)"});
  Builtins::init("con_le_length_difference",
                 new BuiltinFunction(&builtin_con_le_length_difference),
                 {"con_le_length_difference(a, b, c, d, diff) -> sketch constraint (|ab|-|cd| <= diff)"});
  Builtins::init("con_ge_length_difference",
                 new BuiltinFunction(&builtin_con_ge_length_difference),
                 {"con_ge_length_difference(a, b, c, d, diff) -> sketch constraint (|ab|-|cd| >= diff)"});
  Builtins::init("con_eq_len_pt_line_d", new BuiltinFunction(&builtin_con_eq_len_pt_line_d),
                 {"con_eq_len_pt_line_d(p, la, lb, da, db) -> sketch constraint"});
  Builtins::init("con_eq_pt_ln_distances", new BuiltinFunction(&builtin_con_eq_pt_ln_distances),
                 {"con_eq_pt_ln_distances(p1, l1a, l1b, p2, l2a, l2b) -> sketch constraint"});
  Builtins::init("con_equal_angle", new BuiltinFunction(&builtin_con_equal_angle),
                 {"con_equal_angle(a, b, c, d, e, f, g, h) -> sketch constraint"});
  Builtins::init("con_symmetric_horiz", new BuiltinFunction(&builtin_con_symmetric_horiz),
                 {"con_symmetric_horiz(p1, p2) -> sketch constraint"});
  Builtins::init("con_symmetric_vert", new BuiltinFunction(&builtin_con_symmetric_vert),
                 {"con_symmetric_vert(p1, p2) -> sketch constraint"});
  Builtins::init("con_symmetric_line", new BuiltinFunction(&builtin_con_symmetric_line),
                 {"con_symmetric_line(p1, p2, la, lb) -> sketch constraint"});

  Builtins::init("solved", new BuiltinFunction(&builtin_solved),
                 {"solved(sol) -> bool"});
  Builtins::init("residual", new BuiltinFunction(&builtin_residual),
                 {"residual(sol) -> number"});
  Builtins::init("dof", new BuiltinFunction(&builtin_dof),
                 {"dof(sol) -> number"});
  Builtins::init("pt", new BuiltinFunction(&builtin_pt),
                 {"pt(sol, name) -> [x,y]"});
  Builtins::init("pts", new BuiltinFunction(&builtin_pts),
                 {"pts(sol) -> [[x,y], ...]"});
  Builtins::init("poly", new BuiltinFunction(&builtin_poly),
                 {"poly(sol, [names]) -> [[x,y], ...]"});
  Builtins::init("failed_constraints", new BuiltinFunction(&builtin_failed_constraints),
                 {"failed_constraints(sol) -> [string]"});
  Builtins::init("iterations", new BuiltinFunction(&builtin_iterations),
                 {"iterations(sol) -> outer-loop iteration count (0 if no inequalities)"});
  Builtins::init("active_inequalities", new BuiltinFunction(&builtin_active_inequalities),
                 {"active_inequalities(sol) -> [string] inequalities that are tight"});
  Builtins::init("angle", new BuiltinFunction(&builtin_angle),
                 {"angle(sol, [a, b, c]) -> CCW degrees at vertex b ([0,360))"});
}
