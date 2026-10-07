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
 * Installs BOSL2's latest GitHub release into BOSL2Library::updateRoot() when it is newer than
 * the copy in use.
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
