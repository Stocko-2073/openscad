#include "TestEvalMemo.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <cstdint>
#include <deque>
#include <memory>
#include <sstream>
#include <string>

#include "RenderStatistic.h"
#include "core/EvalMemo.h"
#include "core/ModifierOverlays.h"
#include "core/ModuleInstantiation.h"
#include "core/Settings.h"
#include "core/node.h"
#include "geometry/GeometryCache.h"
#include "gui/Preferences.h"
#ifdef ENABLE_CGAL
#include "geometry/cgal/CGALCache.h"
#endif

namespace {

const QString kParts = R"(module part(n) translate([n * 3, 0, 0]) cube(n);
module pair() { part(1); part(2); }
)";

// Saves a version of a document, stamped a few seconds after the last so that a reload sees a
// new file whatever its size.
void save(const QString& path, const QString& text)
{
  static QDateTime stamp = QDateTime::currentDateTime();
  stamp = stamp.addSecs(2);
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(text.toUtf8());
  file.flush();
  QVERIFY(file.setFileTime(stamp, QFileDevice::FileModificationTime));
}

QString read(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return QString::fromUtf8(file.readAll());
}

// Waits for whatever render is under way to end, so that the next one starts at once.
bool idle()
{
  for (int i = 0; i < 30000 && GuiLocker::isLocked(); ++i) QTest::qWait(10);
  return !GuiLocker::isLocked();
}

// Does what starts a render, and waits for that render to end. (A QSignalSpy on
// compilationDone aborted with qBadAlloc, in Qt 6.11, during renders of a large design.)
template <typename Start>
bool rendered(MainWindow *window, Start start)
{
  if (!idle()) return false;
  bool done = false;
  const auto connection =
    QObject::connect(window, &MainWindow::compilationDone, [&done](SourceFile *) { done = true; });
  start();
  for (int i = 0; i < 30000 && !done; ++i) QTest::qWait(10);
  QObject::disconnect(connection);
  return done;
}

// Renders as F6 does, or with `reload` as auto-reload does after a save.
bool render(MainWindow *window, bool reload = false)
{
  return rendered(window, [&]() {
    if (reload) window->actionReloadRender();
    else window->actionRender();
  });
}

// Sets the animation time as typing it into the Animate dock does, which renders.
bool setTime(MainWindow *window, const QString& t)
{
  return rendered(window, [&]() { window->animateWidget->e_tval->setText(t); });
}

// The remark on the last render's "Script evaluation" line.
QString evaluationNote(MainWindow *window)
{
  for (const auto& phase : window->renderStatistic.phaseTimes()) {
    if (phase.name == RenderStatistic::PHASE_EVALUATION) return QString::fromStdString(phase.note);
  }
  return {};
}

// Classes, data and each node's statement with its line: a reused node must point at the very
// statement that a fresh evaluation of the parse on screen picks.
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

bool matchesFreshEvaluation(MainWindow *window)
{
  if (!window->absoluteRootNode) return false;
  const auto fresh = window->instantiateRootFromSource(window->rootFile.get());
  return fresh && dump(*window->absoluteRootNode) == dump(*fresh);
}

std::shared_ptr<const AbstractNode> findNamed(const std::shared_ptr<const AbstractNode>& node,
                                              const std::string& name)
{
  if (node->name() == name) return node;
  for (const auto& child : node->children) {
    if (auto found = findNamed(child, name)) return found;
  }
  return nullptr;
}

void setReuse(bool on)
{
  Settings::Settings::reuseModuleResults.setValue(on);
  emit GlobalPreferences::inst()->reuseModuleResultsChanged(on);
}

}  // namespace

void TestEvalMemo::initTestCase()
{
  // Renders start when these tests say, and not also when auto-reload notices a save.
  window->designActionAutoReload->setChecked(false);
}

void TestEvalMemo::reusesAcrossRenders()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = dir.filePath("parts.scad");
  save(path, kParts + "pair();\npart(3);\n");
  window->tabManager->createTab(path);
  EditorInterface *editor = window->activeEditor;

  QVERIFY(render(window));
  QVERIFY(editor->memoTable);
  QCOMPARE(editor->memoTable->generation(), uint64_t{1});
  QCOMPARE(evaluationNote(window), QString("reused 0 of 4 module calls"));
  QVERIFY(matchesFreshEvaluation(window));

  // Each render parses afresh: everything is reused, and points into the new parse.
  QVERIFY(render(window));
  QCOMPARE(evaluationNote(window), QString("reused 4 of 4 module calls"));
  QVERIFY(matchesFreshEvaluation(window));

  // A save that moves everything down a line, highlights pair() and changes part(3).
  save(path, "// moved down\n" + kParts + "#pair();\npart(5);\n");
  QVERIFY(render(window, true));
  QCOMPARE(evaluationNote(window), QString("reused 3 of 4 module calls"));
  QVERIFY(matchesFreshEvaluation(window));

  // What the picker and the editor's highlight read: a reused cube, found by its index, is on
  // its new line.
  const auto cube = findNamed(window->rootNode, "cube");
  QVERIFY(cube);
  std::deque<std::shared_ptr<const AbstractNode>> path_to_cube;
  const auto picked = window->rootNode->getNodeByID(cube->index(), path_to_cube);
  QVERIFY(picked == cube);
  QCOMPARE(picked->modinst->location().firstLine(), 2);
  // And the # on the call of pair(), whose nodes were all reused, is seen.
  const auto overlays = overlay::collect(window->tree, *window->rootNode);
  QCOMPARE(overlays.size(), size_t{1});
  QVERIFY(overlays.front().kind == overlay::Kind::Highlight);

  window->tabManager->closeCurrentTab();
}

void TestEvalMemo::flushAndPreferenceDropTheTable()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = dir.filePath("pair.scad");
  save(path, kParts + "pair();\n");
  window->tabManager->createTab(path);
  EditorInterface *editor = window->activeEditor;

  QVERIFY(render(window));
  QVERIFY(editor->memoTable);
  window->designActionFlushCaches->trigger();
  QVERIFY(!editor->memoTable);
  QVERIFY(render(window));
  QCOMPARE(evaluationNote(window), QString("reused 0 of 3 module calls"));

  setReuse(false);
  QVERIFY(!editor->memoTable);
  QVERIFY(render(window));
  QVERIFY(!editor->memoTable);
  QCOMPARE(evaluationNote(window), QString());
  QVERIFY(matchesFreshEvaluation(window));

  setReuse(true);
  QVERIFY(render(window));
  QCOMPARE(evaluationNote(window), QString("reused 0 of 3 module calls"));
  QVERIFY(render(window));
  QCOMPARE(evaluationNote(window), QString("reused 3 of 3 module calls"));

  window->tabManager->closeCurrentTab();
}

void TestEvalMemo::animationTimeIsADependency()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = dir.filePath("spin.scad");
  save(path,
       "module spin() rotate($t * 360) cube(1);\n"
       "module still() translate([3, 0, 0]) sphere(1);\n"
       "spin();\nstill();\n");
  window->tabManager->createTab(path);
  Animate *animate = window->animateWidget;

  QVERIFY(setTime(window, "0"));
  QVERIFY(setTime(window, "0.25"));
  QCOMPARE(evaluationNote(window), QString("reused 1 of 2 module calls"));
  QVERIFY(matchesFreshEvaluation(window));
  QVERIFY(setTime(window, "0"));
  QCOMPARE(evaluationNote(window), QString("reused 2 of 2 module calls"));
  QVERIFY(matchesFreshEvaluation(window));

  // Playing it evaluates frames ahead on worker threads, which have no memo, while a render on
  // this thread uses the document's.
  animate->e_fsteps->setText("8");
  animate->e_fps->setText("30");
  QTest::qWait(1000);
  QVERIFY(render(window));
  QTest::qWait(500);
  animate->e_fps->setText("");
  animate->e_fsteps->setText("");
  QVERIFY(setTime(window, "0.5"));
  QVERIFY(matchesFreshEvaluation(window));
  QVERIFY(setTime(window, ""));

  window->tabManager->closeCurrentTab();
}

/*
 * Not a test: times the refreshes of a series of saves, as the console reports them, with the
 * memo and without. OPENSCAD_MEMO_BENCH lists the versions, separated by ':'; they are saved in
 * turn over one document in the directory of the first, where includes resolve as for them.
 */
void TestEvalMemo::benchmarkEditSequence()
{
  const QString list = qEnvironmentVariable("OPENSCAD_MEMO_BENCH");
  if (list.isEmpty()) QSKIP("Set OPENSCAD_MEMO_BENCH to the versions of a design, separated by ':'.");
  const QStringList versions = list.split(':', Qt::SkipEmptyParts);
  const QString path = QFileInfo(versions.front()).dir().filePath("memo-bench.scad");

  // As large as the preferences allow, so that geometry is not evicted between steps.
  GeometryCache::instance()->setMaxSizeMB(5000);
#ifdef ENABLE_CGAL
  CGALCache::instance()->setMaxSizeMB(5000);
#endif
  for (const bool reuse : {true, false}) {
    setReuse(reuse);
    GeometryCache::instance()->clear();
#ifdef ENABLE_CGAL
    CGALCache::instance()->clear();
#endif
    save(path, read(versions.front()));
    window->tabManager->createTab(path);
    for (int i = 0; i < versions.size(); ++i) {
      if (i > 0) save(path, read(versions[i]));
      QVERIFY(render(window, i > 0));
      QString line = QString("%1 %2 %3:")
                       .arg(reuse ? "memo " : "fresh")
                       .arg(i + 1)
                       .arg(QFileInfo(versions[i]).fileName());
      for (const auto& phase : window->renderStatistic.phaseTimes()) {
        line += QString(" %1 %2 ms").arg(QString::fromStdString(phase.name)).arg(phase.ms.count());
        if (!phase.note.empty()) line += " (" + QString::fromStdString(phase.note) + ")";
        line += ",";
      }
      line += QString(" total %1 ms").arg(window->renderStatistic.ms().count());
      qInfo().noquote() << line;
    }
    window->tabManager->closeCurrentTab();
  }
  setReuse(true);
  QFile::remove(path);
}
