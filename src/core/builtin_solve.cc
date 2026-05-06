#include <cstring>
#include <map>
#include <memory>
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

Value make_two_point_constraint(const char *kind, Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 2 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "%1$s() expects two string point names", kind);
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, kind);
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  return obj;
}

Value builtin_coincident(Arguments arguments, const Location& loc)
{
  return make_two_point_constraint("coincident", std::move(arguments), loc);
}

Value builtin_horizontal(Arguments arguments, const Location& loc)
{
  return make_two_point_constraint("horizontal", std::move(arguments), loc);
}

Value builtin_vertical(Arguments arguments, const Location& loc)
{
  return make_two_point_constraint("vertical", std::move(arguments), loc);
}

Value builtin_distance(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 3 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::NUMBER) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "distance() expects (point_name, point_name, number)");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "distance");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("d", arguments[2]->clone());
  return obj;
}

Value builtin_perpendicular(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 3 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::STRING) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "perpendicular() expects three point names: (p1, vertex, p2)");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "perpendicular");
  obj.set("a", arguments[0]->clone());
  obj.set("vertex", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  return obj;
}

Value builtin_parallel(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 4 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::STRING ||
      arguments[3]->type() != Value::Type::STRING) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "parallel() expects four point names");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "parallel");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  return obj;
}

Value builtin_angle(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 5 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::STRING ||
      arguments[3]->type() != Value::Type::STRING ||
      arguments[4]->type() != Value::Type::NUMBER) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "angle() expects four point names and a degree value");
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

Value builtin_fixed(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 1 || arguments[0]->type() != Value::Type::STRING) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "fixed() expects a single point name");
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

Value builtin_pt_on_line(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("pt_on_line", arguments, loc, 3)) return Value::undefined.clone();
  ObjectType obj = make_kind_obj(session, "pt_on_line");
  obj.set("p", arguments[0]->clone());
  obj.set("a", arguments[1]->clone());
  obj.set("b", arguments[2]->clone());
  return obj;
}

Value builtin_pt_line_distance(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings_then_number("pt_line_distance", arguments, loc, 3)) {
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "pt_line_distance");
  obj.set("p", arguments[0]->clone());
  obj.set("a", arguments[1]->clone());
  obj.set("b", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  return obj;
}

Value builtin_at_midpoint(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("at_midpoint", arguments, loc, 3)) return Value::undefined.clone();
  ObjectType obj = make_kind_obj(session, "at_midpoint");
  obj.set("p", arguments[0]->clone());
  obj.set("a", arguments[1]->clone());
  obj.set("b", arguments[2]->clone());
  return obj;
}

Value builtin_equal_length(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("equal_length", arguments, loc, 4)) return Value::undefined.clone();
  ObjectType obj = make_kind_obj(session, "equal_length");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  return obj;
}

Value builtin_length_ratio(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings_then_number("length_ratio", arguments, loc, 4)) {
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

Value builtin_length_difference(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings_then_number("length_difference", arguments, loc, 4)) {
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

Value builtin_eq_len_pt_line_d(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("eq_len_pt_line_d", arguments, loc, 5)) {
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

Value builtin_eq_pt_ln_distances(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("eq_pt_ln_distances", arguments, loc, 6)) {
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

Value builtin_equal_angle(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("equal_angle", arguments, loc, 8)) {
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

Value builtin_symmetric_horiz(Arguments arguments, const Location& loc)
{
  return make_two_point_constraint("symmetric_horiz", std::move(arguments), loc);
}

Value builtin_symmetric_vert(Arguments arguments, const Location& loc)
{
  return make_two_point_constraint("symmetric_vert", std::move(arguments), loc);
}

Value builtin_symmetric_line(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("symmetric_line", arguments, loc, 4)) {
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
  bool anchored = false;
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
};

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

}  // namespace

Value builtin_solve2d(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  const std::string doc_root = arguments.documentRoot();

  if (arguments.size() != 1 || arguments[0]->type() != Value::Type::VECTOR) {
    LOG(message_group::Warning, loc, doc_root,
        "solve2d() expects a single vector of entities and constraints");
    return Value::undefined.clone();
  }

  std::vector<PointDecl> points;
  std::map<std::string, size_t> name_to_idx;
  std::vector<ConstraintDecl> constraints;

  // Parse phase
  const VectorType& items = arguments[0]->toVector();
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
        p.anchored = true;
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
      } else {
        LOG(message_group::Warning, loc, doc_root,
            "solve2d: unknown item kind '%1$s'", kind);
        continue;
      }
      constraints.push_back(std::move(c));
    }
  }

  // Build phase: assemble Slvs_System
  // Group 1: workplane (and anchored point params).
  // Group 2: free point params and all constraints.
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

  // Points. Free points get a small, unique initial offset to avoid
  // degenerate starting geometry (e.g., all-coincident points).
  size_t free_idx = 0;
  for (auto& p : points) {
    Slvs_hGroup g = p.anchored ? g_fixed : g_solve;
    double init_u = p.u;
    double init_v = p.v;
    if (!p.anchored) {
      init_u = static_cast<double>(free_idx) + 1.0;
      init_v = static_cast<double>(free_idx) * 0.5;
      ++free_idx;
    }
    p.u_param = next_param++;
    p.v_param = next_param++;
    sparams.push_back(Slvs_MakeParam(p.u_param, g, init_u));
    sparams.push_back(Slvs_MakeParam(p.v_param, g, init_v));
    p.entity = next_entity++;
    sentities.push_back(Slvs_MakePoint2d(p.entity, g, wrkpl, p.u_param, p.v_param));
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
          "solve2d: %1$s constraint references unknown point '%2$s'", kind, name);
      return 0;
    }
    return points[it->second].entity;
  };

  std::vector<std::string> failed_names;
  std::map<Slvs_hConstraint, std::string> constraint_name_by_h;

  for (const auto& c : constraints) {
    Slvs_hConstraint ch = next_constraint++;
    auto register_name = [&](const std::string& summary) {
      constraint_name_by_h[ch] = summary;
    };

    if (c.kind == "coincident" && c.points.size() == 2) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      if (!a || !b) continue;
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_POINTS_COINCIDENT,
                                                 wrkpl, 0.0, a, b, 0, 0));
      register_name("coincident(" + c.points[0] + "," + c.points[1] + ")");
    } else if (c.kind == "distance" && c.points.size() == 2) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      if (!a || !b) continue;
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PT_PT_DISTANCE,
                                                 wrkpl, c.valA, a, b, 0, 0));
      register_name("distance(" + c.points[0] + "," + c.points[1] + ")");
    } else if (c.kind == "horizontal" && c.points.size() == 2) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      if (!a || !b) continue;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_HORIZONTAL,
                                                 wrkpl, 0.0, 0, 0, line, 0));
      register_name("horizontal(" + c.points[0] + "," + c.points[1] + ")");
    } else if (c.kind == "vertical" && c.points.size() == 2) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      if (!a || !b) continue;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_VERTICAL,
                                                 wrkpl, 0.0, 0, 0, line, 0));
      register_name("vertical(" + c.points[0] + "," + c.points[1] + ")");
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
      register_name("perpendicular(" + c.points[0] + "," + c.points[1] + "," + c.points[2] + ")");
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
      register_name("parallel(" + c.points[0] + "," + c.points[1] + "," +
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
      register_name("angle(...)");
    } else if (c.kind == "fixed" && c.points.size() == 1) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      if (!a) continue;
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_WHERE_DRAGGED,
                                                 wrkpl, 0.0, a, 0, 0, 0));
      register_name("fixed(" + c.points[0] + ")");
    } else if (c.kind == "pt_on_line" && c.points.size() == 3) {
      Slvs_hEntity p = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity a = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[2]);
      if (!p || !a || !b) continue;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PT_ON_LINE,
                                                 wrkpl, 0.0, p, 0, line, 0));
      register_name("pt_on_line(" + c.points[0] + "," + c.points[1] + "," + c.points[2] + ")");
    } else if (c.kind == "pt_line_distance" && c.points.size() == 3) {
      Slvs_hEntity p = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity a = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[2]);
      if (!p || !a || !b) continue;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PT_LINE_DISTANCE,
                                                 wrkpl, c.valA, p, 0, line, 0));
      register_name("pt_line_distance(" + c.points[0] + "," + c.points[1] + "," + c.points[2] + ")");
    } else if (c.kind == "at_midpoint" && c.points.size() == 3) {
      Slvs_hEntity p = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity a = pt_entity(c.kind, c.points[1]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[2]);
      if (!p || !a || !b) continue;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_AT_MIDPOINT,
                                                 wrkpl, 0.0, p, 0, line, 0));
      register_name("at_midpoint(" + c.points[0] + "," + c.points[1] + "," + c.points[2] + ")");
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
      register_name("equal_length(" + c.points[0] + "," + c.points[1] + "," +
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
      register_name("length_ratio(...)");
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
      register_name("length_difference(...)");
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
      register_name("eq_len_pt_line_d(...)");
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
      register_name("eq_pt_ln_distances(...)");
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
      register_name("equal_angle(...)");
    } else if ((c.kind == "symmetric_horiz" || c.kind == "symmetric_vert") &&
               c.points.size() == 2) {
      Slvs_hEntity a = pt_entity(c.kind, c.points[0]);
      Slvs_hEntity b = pt_entity(c.kind, c.points[1]);
      if (!a || !b) continue;
      int type = (c.kind == "symmetric_horiz") ? SLVS_C_SYMMETRIC_HORIZ
                                               : SLVS_C_SYMMETRIC_VERT;
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve, type,
                                                 wrkpl, 0.0, a, b, 0, 0));
      register_name(c.kind + "(" + c.points[0] + "," + c.points[1] + ")");
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
      register_name("symmetric_line(...)");
    } else {
      LOG(message_group::Warning, loc, doc_root,
          "solve2d: malformed %1$s constraint", c.kind);
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

  // Result phase
  auto data = std::make_shared<SolutionType::Data>();
  data->result_code = sys.result;
  data->solved = (sys.result == SLVS_RESULT_OKAY);
  data->dof = sys.dof;
  data->residual = 0.0;  // SolveSpace doesn't expose residual directly.

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
    SolutionType::Point2d pt = {param_value[p.u_param], param_value[p.v_param]};
    data->points[p.name] = pt;
  }

  for (int i = 0; i < sys.faileds; ++i) {
    auto it = constraint_name_by_h.find(sys.failed[i]);
    if (it != constraint_name_by_h.end()) {
      data->failed_constraints.push_back(it->second);
    } else {
      data->failed_constraints.push_back("constraint #" + std::to_string(sys.failed[i]));
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

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_builtin_solve()
{
  Builtins::init("solve2d", new BuiltinFunction(&builtin_solve2d),
                 {"solve2d(items) -> Solution"});

  Builtins::init("point", new BuiltinFunction(&builtin_point),
                 {"point(name) -> sketch entity",
                  "point(name, at=[x,y]) -> sketch entity (anchored)"});

  Builtins::init("coincident", new BuiltinFunction(&builtin_coincident),
                 {"coincident(p1, p2) -> sketch constraint"});
  Builtins::init("distance", new BuiltinFunction(&builtin_distance),
                 {"distance(p1, p2, d) -> sketch constraint"});
  Builtins::init("horizontal", new BuiltinFunction(&builtin_horizontal),
                 {"horizontal(p1, p2) -> sketch constraint"});
  Builtins::init("vertical", new BuiltinFunction(&builtin_vertical),
                 {"vertical(p1, p2) -> sketch constraint"});
  Builtins::init("perpendicular", new BuiltinFunction(&builtin_perpendicular),
                 {"perpendicular(p1, vertex, p2) -> sketch constraint"});
  Builtins::init("parallel", new BuiltinFunction(&builtin_parallel),
                 {"parallel(p1, p2, p3, p4) -> sketch constraint"});
  Builtins::init("angle", new BuiltinFunction(&builtin_angle),
                 {"angle(p1, p2, p3, p4, deg) -> sketch constraint"});
  Builtins::init("fixed", new BuiltinFunction(&builtin_fixed),
                 {"fixed(p) -> sketch constraint"});

  Builtins::init("pt_on_line", new BuiltinFunction(&builtin_pt_on_line),
                 {"pt_on_line(p, la, lb) -> sketch constraint"});
  Builtins::init("pt_line_distance", new BuiltinFunction(&builtin_pt_line_distance),
                 {"pt_line_distance(p, la, lb, d) -> sketch constraint (signed)"});
  Builtins::init("at_midpoint", new BuiltinFunction(&builtin_at_midpoint),
                 {"at_midpoint(m, la, lb) -> sketch constraint"});
  Builtins::init("equal_length", new BuiltinFunction(&builtin_equal_length),
                 {"equal_length(a, b, c, d) -> sketch constraint"});
  Builtins::init("length_ratio", new BuiltinFunction(&builtin_length_ratio),
                 {"length_ratio(a, b, c, d, r) -> sketch constraint (|ab|/|cd|=r)"});
  Builtins::init("length_difference", new BuiltinFunction(&builtin_length_difference),
                 {"length_difference(a, b, c, d, diff) -> sketch constraint (|ab|-|cd|=diff)"});
  Builtins::init("eq_len_pt_line_d", new BuiltinFunction(&builtin_eq_len_pt_line_d),
                 {"eq_len_pt_line_d(p, la, lb, da, db) -> sketch constraint"});
  Builtins::init("eq_pt_ln_distances", new BuiltinFunction(&builtin_eq_pt_ln_distances),
                 {"eq_pt_ln_distances(p1, l1a, l1b, p2, l2a, l2b) -> sketch constraint"});
  Builtins::init("equal_angle", new BuiltinFunction(&builtin_equal_angle),
                 {"equal_angle(a, b, c, d, e, f, g, h) -> sketch constraint"});
  Builtins::init("symmetric_horiz", new BuiltinFunction(&builtin_symmetric_horiz),
                 {"symmetric_horiz(p1, p2) -> sketch constraint"});
  Builtins::init("symmetric_vert", new BuiltinFunction(&builtin_symmetric_vert),
                 {"symmetric_vert(p1, p2) -> sketch constraint"});
  Builtins::init("symmetric_line", new BuiltinFunction(&builtin_symmetric_line),
                 {"symmetric_line(p1, p2, la, lb) -> sketch constraint"});

  Builtins::init("solved", new BuiltinFunction(&builtin_solved),
                 {"solved(sol) -> bool"});
  Builtins::init("residual", new BuiltinFunction(&builtin_residual),
                 {"residual(sol) -> number"});
  Builtins::init("dof", new BuiltinFunction(&builtin_dof),
                 {"dof(sol) -> number"});
  Builtins::init("pt", new BuiltinFunction(&builtin_pt),
                 {"pt(sol, name) -> [x,y]"});
  Builtins::init("poly", new BuiltinFunction(&builtin_poly),
                 {"poly(sol, [names]) -> [[x,y], ...]"});
  Builtins::init("failed_constraints", new BuiltinFunction(&builtin_failed_constraints),
                 {"failed_constraints(sol) -> [string]"});
}
