#include "core/EvalMemo.h"

#include <catch2/catch_all.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

#include "core/BuiltinContext.h"
#include "core/Builtins.h"
#include "core/Context.h"
#include "core/EvaluationSession.h"
#include "core/ModuleInstantiation.h"
#include "core/ScopeContext.h"
#include "core/SourceFile.h"
#include "core/Tree.h"
#include "core/node.h"
#include "openscad.h"
#include "platform/PlatformUtils.h"
#include "utils/exceptions.h"
#include "utils/printutils.h"

namespace fs = std::filesystem;

namespace {

void setUpOnce()
{
  static const bool done = [] {
    const fs::path source = fs::path(__FILE__).parent_path();  // <root>/src/core
    PlatformUtils::registerApplicationPath(source.parent_path().generic_string());
    Builtins::instance()->initialize();
    return true;
  }();
  (void)done;
}

// Every version of a script is parsed as the same file: keys include its directory.
std::unique_ptr<SourceFile> parseScript(const std::string& text)
{
  setUpOnce();
  const std::string mainFile = (fs::temp_directory_path() / "memo-test.scad").generic_string();
  SourceFile *file = nullptr;
  const bool parsed = parse(file, text + "\n\x03\n", mainFile, mainFile, false);
  std::unique_ptr<SourceFile> result(file);
  REQUIRE(parsed);
  result->handleDependencies();
  return result;
}

// Classes, data and each node's statement, which must be the very statement
// of the parse evaluated, and its line.
void dump(const AbstractNode& node, std::ostringstream& out)
{
  out << node.verbose_name() << ':' << node.toString() << '@';
  if (node.modinst && node.modinst->parent_scope) {
    out << static_cast<const void *>(node.modinst) << ':' << node.modinst->location().firstLine();
  }
  if (!node.children.empty()) {
    out << '{';
    for (const auto& child : node.children) dump(*child, out);
    out << '}';
  }
  out << ';';
}

std::string dump(const AbstractNode& node)
{
  std::ostringstream out;
  dump(node, out);
  return out.str();
}

struct Run {
  std::shared_ptr<AbstractNode> root;
  std::vector<std::string> messages;
  memo::Stats stats;
  std::string tree;
};

Run evaluate(const SourceFile& file, memo::MemoTable *table)
{
  Run run;
  resetSuppressedMessages();
  std::vector<Message> messages;
  g_message_capture.push_back(&messages);
  {
    const PrintSuppressGuard quiet;
    EvaluationSession session{fs::temp_directory_path().generic_string()};
    ContextHandle<BuiltinContext> builtin{Context::create<BuiltinContext>(&session)};
    AbstractNode::resetIndexCounter();
    std::optional<memo::EvalMemoSession> memo;
    if (table) {
      memo.emplace(*table, file);
      session.setMemo(&*memo);
    }
    std::shared_ptr<const FileContext> fileContext;
    run.root = file.instantiate(*builtin, &fileContext);
    fileContext.reset();
    if (memo) {
      run.stats = memo->stats();
      session.setMemo(nullptr);
    }
  }
  g_message_capture.pop_back();
  for (const auto& message : messages) {
    if (!message.repeat) run.messages.push_back(message.str());  // what was printed
  }
  REQUIRE(run.root);
  run.tree = dump(*run.root);
  return run;
}

memo::Hash128 syntaxHash(const std::string& text)
{
  const auto file = parseScript(text);
  memo::ASTHasher h;
  h.scope(*file->scope);
  return h.finish();
}

}  // namespace

TEST_CASE("Syntax hashes literals exactly, modifiers too, but not locations", "[memo]")
{
  // The AST printer rounds to six digits; these differ in the ninth.
  CHECK(syntaxHash("x = 1.00000001;") != syntaxHash("x = 1.00000002;"));
  CHECK(syntaxHash("x = [1, 2]; cube(x);") == syntaxHash("\n\n  x = [1,  2];\n\tcube( x );"));

  const auto plain = syntaxHash("cube();");
  const auto root = syntaxHash("!cube();");
  const auto highlight = syntaxHash("#cube();");
  const auto background = syntaxHash("%cube();");
  CHECK(root != plain);
  CHECK(highlight != plain);
  CHECK(background != plain);
  CHECK(root != highlight);
  CHECK(root != background);
  CHECK(highlight != background);
}

TEST_CASE("A second evaluation reuses calls and gives the same tree", "[memo]")
{
  const auto file = parseScript(R"(
module part(size) { translate([size, 0, 0]) cube(size); echo("part", size); }
module pair() { part(1); part(2); }
pair();
part(3);
)");
  memo::MemoTable table;
  const Run first = evaluate(*file, &table);
  CHECK(first.stats.boundaries == 4);
  CHECK(first.stats.stored == 4);
  CHECK(table.generation() == 1);

  const Run second = evaluate(*file, &table);
  // pair() and part(3); the calls inside pair() come with it.
  CHECK(second.stats.boundaries == 2);
  CHECK(second.stats.hits == 2);
  CHECK(second.stats.misses == 0);

  const Run fresh = evaluate(*file, nullptr);
  CHECK(first.tree == fresh.tree);
  CHECK(second.tree == fresh.tree);
  CHECK(second.messages == fresh.messages);
}

TEST_CASE("Reused nodes keep their geometry digests", "[memo]")
{
  const std::string text = R"(
module part(size) translate([size, 0, 0]) cube(size);
module pair() { part(1); part(2); }
pair();
part(3);
)";
  memo::MemoTable table;
  const auto file = parseScript(text);
  const Run first = evaluate(*file, &table);
  const Hash128 digest = Tree(first.root).digest(*first.root);

  // The root is new on every evaluation; what was reused under it comes with its digests.
  const auto again = parseScript(text);  // the nodes point into it
  const Run second = evaluate(*again, &table);
  CHECK(!second.root->hasDigest());
  for (const auto& call : second.root->children) {
    std::vector<const AbstractNode *> nodes{call.get()};
    while (!nodes.empty()) {
      const AbstractNode *node = nodes.back();
      nodes.pop_back();
      CHECK(node->hasDigest());
      for (const auto& child : node->children) nodes.push_back(child.get());
    }
  }
  CHECK(Tree(second.root).digest(*second.root) == digest);

  // A call that runs again makes new nodes, which have none yet.
  const auto changed = parseScript(R"(
module part(size) translate([size, 0, 0]) cube(size);
module pair() { part(1); part(2); }
pair();
part(4);
)");
  const Run third = evaluate(*changed, &table);
  CHECK(third.root->children.at(0)->hasDigest());
  CHECK(!third.root->children.at(1)->hasDigest());
  CHECK(Tree(third.root).digest(*third.root) != digest);
}

TEST_CASE("Calls are counted as a fresh evaluation makes them, reused or not", "[memo]")
{
  // p() takes a value too deep to hash, so it is never stored, but the b()
  // inside it is.
  const auto file = parseScript(R"(
function nest(n) = n == 0 ? 0 : [nest(n - 1)];
module a() cube(1);
module b() { a(); a(); }
module p(v) b();
b();
p(nest(300));
)");
  memo::MemoTable table;
  const Run first = evaluate(*file, &table);
  // b() a() a() p() b() a() a(): the second a() and, in p(), b() with its
  // two are reused from earlier in the same evaluation.
  CHECK(first.stats.userCalls == 7);
  CHECK(first.stats.userCallsReused == 4);

  const Run second = evaluate(*file, &table);
  CHECK(second.stats.boundaries == 3);
  CHECK(second.stats.userCalls == 7);
  CHECK(second.stats.userCallsReused == 6);
}

TEST_CASE("A $ variable a call reads must still have its value", "[memo]")
{
  const auto script = [](const std::string& value) {
    return parseScript("$size = " + value + ";\nmodule part() cube($size);\npart();");
  };
  memo::MemoTable table;
  const auto one = script("1");
  evaluate(*one, &table);
  const auto two = script("2");
  const Run changed = evaluate(*two, &table);
  CHECK(changed.stats.hits == 0);
  CHECK(changed.stats.staleDollar == 1);
  CHECK(changed.tree == evaluate(*two, nullptr).tree);

  const Run back = evaluate(*one, &table);
  CHECK(back.stats.hits == 1);
}

TEST_CASE("A $ variable a call only accumulates into is not a dependency", "[memo]")
{
  // As BOSL2's transforms do with $transform: the value read is assigned to
  // the same variable, so it matters only if something reads that for real.
  const auto script = [](const std::string& value) {
    return parseScript("$t = " + value + ";\nmodule part() { $t = $t * 2; cube(1); }\npart();");
  };
  memo::MemoTable table;
  const auto one = script("1");
  evaluate(*one, &table);
  const auto three = script("3");
  const Run run = evaluate(*three, &table);
  CHECK(run.stats.hits == 1);
  CHECK(run.tree == evaluate(*three, nullptr).tree);
}

TEST_CASE("An accumulated $ variable read for real further down is a dependency", "[memo]")
{
  const auto script = [](const std::string& value) {
    return parseScript("$t = " + value +
                       ";\nmodule inner() cube($t);\nmodule part() { $t = $t * 2; inner(); }\npart();");
  };
  memo::MemoTable table;
  const auto one = script("1");
  evaluate(*one, &table);
  const auto three = script("3");
  const Run run = evaluate(*three, &table);
  CHECK(run.stats.hits == 0);
  CHECK(run.stats.staleDollar >= 1);
  CHECK(run.tree == evaluate(*three, nullptr).tree);
}

TEST_CASE("A reused call prints its messages again", "[memo]")
{
  const auto file = parseScript("module talk() { echo(\"hi\"); cube(); }\ntalk();\ntalk();");
  memo::MemoTable table;
  const Run first = evaluate(*file, &table);
  CHECK(first.stats.hits == 1);  // the second call, in the same evaluation
  const Run second = evaluate(*file, &table);
  CHECK(second.stats.hits == 2);
  CHECK(second.stats.messagesReplayed == 2);

  const Run fresh = evaluate(*file, nullptr);
  REQUIRE(fresh.messages.size() == 2);
  CHECK(first.messages == fresh.messages);
  CHECK(second.messages == fresh.messages);
}

TEST_CASE("A deprecation printed before a call ran is printed by its reuse where it is new", "[memo]")
{
  // rotate_extrude() with an odd $fn and no angle warns, always in the same
  // words, so the second warning is not printed.
  const std::string part = "module part() rotate_extrude($fn = 3) translate([2, 0]) square(1);\n";
  memo::MemoTable table;
  const auto both = parseScript("rotate_extrude($fn = 3) translate([2, 0]) square(1);\n" + part + "part();");
  const Run first = evaluate(*both, &table);
  CHECK(first.messages.size() == 1);

  const auto alone = parseScript(part + "part();");
  const Run run = evaluate(*alone, &table);
  CHECK(run.stats.hits == 1);
  CHECK(run.messages.size() == 1);
  CHECK(run.messages == evaluate(*alone, nullptr).messages);
}

TEST_CASE("A call that ran rands() is not stored", "[memo]")
{
  const auto file = parseScript("module noise() cube(rands(1, 2, 1)[0]);\nnoise();");
  memo::MemoTable table;
  const Run first = evaluate(*file, &table);
  CHECK(first.stats.impure == 1);
  CHECK(first.stats.stored == 0);
  const Run second = evaluate(*file, &table);
  CHECK(second.stats.hits == 0);
}

TEST_CASE("An evaluation that throws leaves the table usable", "[memo]")
{
  // As the GUI's "Stop on first warning" does: the warning throws out of the
  // whole evaluation, once while noisy() records, once while its reuse
  // replays the warning.
  const auto file = parseScript(R"(
module part(n) cube(n);
module noisy() { part(1); part(2); echo(no_such_variable); part(3); }
part(1);
noisy();
)");
  const auto evaluateStopping = [&](memo::MemoTable& table) {
    resetSuppressedMessages();
    std::vector<Message> messages;
    g_message_capture.push_back(&messages);
    set_output_handler([](const Message&, void *) {}, [](const Message&, void *) {}, nullptr);
    OpenSCAD::hardwarnings = true;
    bool threw = false;
    try {
      EvaluationSession session{fs::temp_directory_path().generic_string()};
      ContextHandle<BuiltinContext> builtin{Context::create<BuiltinContext>(&session)};
      memo::EvalMemoSession memo{table, *file};
      session.setMemo(&memo);
      std::shared_ptr<const FileContext> fileContext;
      file->instantiate(*builtin, &fileContext);
    } catch (const HardWarningException&) {
      threw = true;
    }
    OpenSCAD::hardwarnings = false;
    set_output_handler(nullptr, nullptr, nullptr);
    g_message_capture.pop_back();
    return threw;
  };
  const Run fresh = evaluate(*file, nullptr);

  memo::MemoTable recording;
  CHECK(evaluateStopping(recording));
  CHECK(recording.size() == 2);  // part(1) and part(2), not noisy()
  const Run afterRecording = evaluate(*file, &recording);
  CHECK(afterRecording.stats.hits == 3);
  CHECK(afterRecording.tree == fresh.tree);
  CHECK(afterRecording.messages == fresh.messages);

  memo::MemoTable replaying;
  evaluate(*file, &replaying);
  CHECK(evaluateStopping(replaying));
  const Run afterReplaying = evaluate(*file, &replaying);
  CHECK(afterReplaying.stats.hits == 2);
  CHECK(afterReplaying.tree == fresh.tree);
  CHECK(afterReplaying.messages == fresh.messages);
}

TEST_CASE("A function value hashes as its syntax and the values it captures", "[memo]")
{
  const auto script = [](const std::string& k) {
    return parseScript("k = " + k + ";\nf = function(x) x * k;\nmodule part(g) cube(g(1));\npart(f);");
  };
  memo::MemoTable table;
  const auto two = script("2");
  const Run first = evaluate(*two, &table);
  CHECK(first.stats.unhashableArg == 0);
  CHECK(first.stats.stored == 1);

  const auto three = script("3");
  const Run changed = evaluate(*three, &table);
  CHECK(changed.stats.hits == 0);
  CHECK(changed.tree == evaluate(*three, nullptr).tree);

  const auto twoAgain = script("2");
  CHECK(evaluate(*twoAgain, &table).stats.hits == 1);
}

TEST_CASE("Values nested too deeply to hash keep a call out of the memo", "[memo]")
{
  const auto file =
    parseScript("function nest(n) = n == 0 ? 0 : [nest(n - 1)];\nmodule part(v) cube(1);\npart(nest(300));");
  memo::MemoTable table;
  const Run run = evaluate(*file, &table);
  CHECK(run.stats.unhashableArg == 1);
  CHECK(run.stats.stored == 0);
  CHECK(run.tree == evaluate(*file, nullptr).tree);
}

TEST_CASE("Reused nodes point at the statements of the current parse", "[memo]")
{
  const std::string parts = R"(
module wrap() children();
module part(n) { wrap() { cube(n); sphere(n); } }
)";
  const auto version = [&](const std::string& top, const std::string& extra) {
    return parseScript(top + parts + "module both() {\n" + extra + "  part(1);\n  wrap() part(2);\n}\n" +
                       "both();\npart(3);\n");
  };
  memo::MemoTable table;
  auto before = version("", "");
  evaluate(*before, &table);

  // Lines above everything, and a statement in the caller of the parts:
  // both() runs again, but part(1), wrap() part(2) and part(3) are reused.
  auto after = version("// moved down\n\n\n", "  echo(\"new\");\n");
  before.reset();  // the table needs nothing from the old parse
  const Run run = evaluate(*after, &table);
  CHECK(run.stats.misses == 1);
  CHECK(run.stats.hits == 3);
  CHECK(run.stats.relocateFailed == 0);

  const Run fresh = evaluate(*after, nullptr);
  CHECK(run.tree == fresh.tree);
  CHECK(run.messages == fresh.messages);
}

TEST_CASE("Reused nodes from an enclosing call's children follow that call", "[memo]")
{
  // The cube is in the children of the top-level mid(), which wrap()'s own
  // children pass on: reusing wrap() must find it at mid()'s call.
  const auto version = [](const std::string& top, const std::string& extra) {
    return parseScript(top + "module wrap() children();\nmodule mid() {\n" + extra +
                       "  wrap() children();\n}\nmid() cube(1);\n");
  };
  memo::MemoTable table;
  auto before = version("", "");
  evaluate(*before, &table);
  auto after = version("\n\n", "  echo(\"new\");\n");
  before.reset();
  const Run run = evaluate(*after, &table);
  CHECK(run.stats.hits == 1);
  CHECK(run.tree == evaluate(*after, nullptr).tree);
}

TEST_CASE("Children run by a recursive call at one site are not mistaken for another's", "[memo]")
{
  // rec(n - 1) children() runs its own children block as the children of
  // every level of the recursion: the inner calls cannot tell from a
  // statement's place which call ran it, so they are not stored. The outer
  // call is, and its nodes must still come out right.
  const auto version = [](const std::string& top) {
    return parseScript(top + "module rec(n) { if (n > 0) rec(n - 1) children(); else children(); }\n" +
                       "rec(3) cube(1);\n");
  };
  memo::MemoTable table;
  auto before = version("");
  const Run first = evaluate(*before, &table);
  CHECK(first.stats.unlocatable >= 1);
  CHECK(first.tree == evaluate(*before, nullptr).tree);

  auto after = version("\n\n");
  before.reset();
  const Run run = evaluate(*after, &table);
  CHECK(run.stats.hits == 1);
  CHECK(run.tree == evaluate(*after, nullptr).tree);
}

TEST_CASE("Every node class copies itself", "[memo]")
{
  const auto file = parseScript(R"(
group() { cube(1); sphere(1); cylinder(1, 1); polyhedron([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]],
                                                         [[0, 1, 2], [0, 1, 3], [0, 2, 3], [1, 2, 3]]); }
translate([1, 0, 0]) rotate(10) scale(2) mirror([1, 0, 0]) multmatrix(1) color("red") cube();
union() { difference() { cube(); sphere(); } intersection() { cube(); sphere(); } }
render() hull() minkowski() { cube(); sphere(); }
resize([2, 2, 2]) cube();
linear_extrude(1) square(1);
rotate_extrude() translate([2, 0]) circle(1);
offset(1) polygon([[0, 0], [1, 0], [0, 1]]);
projection() cube();
text("a");
intersection_for(i = [0, 1]) cube(i + 1);
)");
  const Run run = evaluate(*file, nullptr);
  const auto copy = run.root->clone();
  REQUIRE(copy);
  CHECK(dump(*copy) == run.tree);
  CHECK(copy->index() != run.root->index());
}

TEST_CASE("A node class without its own copy() is not sliced", "[memo]")
{
  struct Labelled : public GroupNode {
    using GroupNode::GroupNode;
    int label = 7;
  };
  const Labelled labelled(nullptr, "labelled");
  CHECK(labelled.copy() == nullptr);

  const GroupNode group(nullptr, "group");
  const auto copy = group.copy();
  REQUIRE(copy);
  CHECK(typeid(*copy) == typeid(GroupNode));
  CHECK(copy->verbose_name() == "group");
  CHECK(copy->index() != group.index());
}
