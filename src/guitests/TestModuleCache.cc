#include "TestModuleCache.h"

#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <memory>

#include "core/node.h"
#include "platform/PlatformUtils.h"

void touchFile(const QString& filename)
{
  auto timeStamp = QDateTime::currentDateTime();

  QFileInfo fileInfo(filename);
  QFile file(filename);
  file.open(QIODevice::WriteOnly);
  if (file.isOpen()) {
    file.setFileTime(timeStamp, QFileDevice::FileModificationTime);
    file.setFileTime(timeStamp, QFileDevice::FileAccessTime);
  }
}

// F4: renders if the file changed, on a worker thread, so wait for the compile to end.
void reloadAndRender(MainWindow *window)
{
  QSignalSpy done(window, &MainWindow::compilationDone);
  window->actionReloadRender();
  if (done.isEmpty()) QVERIFY2(done.wait(30000), "The render should finish.");
}

void TestModuleCache::testBasicCache()
{
  restoreWindowInitialState();

  QString filename = QString::fromStdString("test-tmp.scad");
  SourceFile *previousFile{nullptr};
  SourceFile *currentFile{nullptr};
  connect(window, &MainWindow::compilationDone,
          [&currentFile](SourceFile *file) { currentFile = file; });

  window->designActionAutoReload->setChecked(false);  // Disable auto-reload  & render
  window->tabManager->open(filename);                 // Open use.scad
  reloadAndRender(window);

  QVERIFY2(currentFile != nullptr, "The file 'test-tmp.scad' should be loaded.");
  previousFile = currentFile;  // save the loaded Source from the

  reloadAndRender(window);
  QVERIFY2(previousFile == currentFile,
           "The file should be the same as the file cache should have done its work.");
  sleep(1);

  touchFile(filename);
  reloadAndRender(window);
  QVERIFY2(
    previousFile != currentFile,
    "The file should *not* be the same as the file cache should have detected the timestamp change.");
}

std::vector<std::string>& findNode(std::shared_ptr<AbstractNode> node, std::vector<std::string>& path)
{
  path.push_back(node->verbose_name());
  for (auto child : node->getChildren()) return findNode(child, path);
  return path;
}

void TestModuleCache::testMCAD()
{
  restoreWindowInitialState();

  QString filename =
    QString::fromStdString(PlatformUtils::resourceBasePath()) + "/tests/modulecache-tests/use-mcad.scad";
  window->tabManager->open(filename);  // Open use-mcad.scad
  reloadAndRender(window);

  auto node = window->instantiateRootFromSource(window->rootFile.get());
  QVERIFY2(node->verbose_name().empty(), "Root node name must be empty");
  QVERIFY2(node->getChildren().size() != 0, "There must have at least a node");
  QCOMPARE(QString::fromStdString(node->getChildren()[0]->verbose_name()),
           QString::fromStdString("module roundedBox"));
}
