#pragma once

#include <QLockFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <filesystem>
#include <memory>
#include <optional>

#include "core/BOSL2Library.h"

class QNetworkReply;

/**
 * Keeps the built-in BOSL2 at the latest GitHub release. Started once per GUI
 * launch: it asks GitHub for the latest release and, when that is newer than
 * the copy in use, downloads it, unpacks it off the main thread and swaps it
 * into BOSL2Library::updateRoot(). The next preview or render uses it.
 *
 * Not reaching GitHub is silent (the copy on disk keeps working); a download
 * that cannot be installed is reported as a warning in the console.
 */
class BOSL2Updater : public QObject
{
public:
  explicit BOSL2Updater(QObject *parent);
  void start();

  struct Unpacked {
    QString dir;  // staging folder holding the release's files, or empty
    std::optional<BOSL2Library::Version> version;
    QString error;
  };

private:
  void onRelease(QNetworkReply *reply);
  void onDownload(QNetworkReply *reply, const BOSL2Library::Version& latest);
  void install(const Unpacked& unpacked, const BOSL2Library::Version& latest);
  void finish();

  QNetworkAccessManager nam;
  std::unique_ptr<QLockFile> lock;
  std::filesystem::path root;
};
