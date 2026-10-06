#include "gui/BOSL2Updater.h"

#include <zip.h>

#include <QByteArray>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QUrl>
#include <QtConcurrentRun>
#include <QtGlobal>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "core/parsersettings.h"
#include "platform/PlatformUtils.h"
#include "utils/printutils.h"

namespace fs = std::filesystem;
using BOSL2Library::Version;

namespace {

constexpr const char *kLatestReleaseUrl =
  "https://api.github.com/repos/BelfrySCAD/BOSL2/releases/latest";
// Staging and swap folders in the update root; any left over from an
// interrupted update are removed on the next launch.
constexpr const char *kScratchPrefix = ".BOSL2-";

QNetworkRequest makeRequest(const QUrl& url, int timeoutMs)
{
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::UserAgentHeader,
                    QString::fromStdString(PlatformUtils::user_agent()));
  request.setRawHeader("Accept", "application/vnd.github+json");
  // The zipball URL redirects to codeload.github.com.
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
  request.setTransferTimeout(timeoutMs);
#endif
  return request;
}

BOSL2Updater::Unpacked failed(const QString& error)
{
  return {{}, std::nullopt, error};
}

// Unpacks a GitHub zipball, whose entries all sit under one
// "<owner>-<repo>-<sha>/" folder, into a new staging folder in root. Entries
// named .git* are left out, as they are from the built-in copy.
BOSL2Updater::Unpacked unpack(const QByteArray& zipData, const fs::path& root)
{
  QTemporaryDir staging(QString::fromStdString((root / kScratchPrefix).generic_string()) +
                        "XXXXXX");
  if (!staging.isValid()) return failed("cannot create a staging folder: " + staging.errorString());
  const fs::path dest = staging.path().toStdString();

  zip_error_t zerr;
  zip_error_init(&zerr);
  zip_source_t *source = zip_source_buffer_create(zipData.constData(), zipData.size(), 0, &zerr);
  zip_t *archive = source ? zip_open_from_source(source, ZIP_RDONLY, &zerr) : nullptr;
  if (!archive) {
    if (source) zip_source_free(source);
    const QString reason = zip_error_strerror(&zerr);
    zip_error_fini(&zerr);
    return failed("the download is not a zip archive (" + reason + ")");
  }

  QString error;
  const zip_int64_t count = zip_get_num_entries(archive, 0);
  for (zip_int64_t i = 0; i < count && error.isEmpty(); ++i) {
    zip_stat_t st;
    if (zip_stat_index(archive, i, 0, &st) != 0 || !(st.valid & ZIP_STAT_NAME)) {
      error = "cannot read the archive's entry list";
      break;
    }
    const std::string name = st.name;
    const auto slash = name.find('/');
    if (slash == std::string::npos) continue;
    const fs::path rel = fs::path(name.substr(slash + 1)).lexically_normal();
    if (rel.empty() || rel == ".") continue;
    const std::string first = rel.begin()->generic_string();
    if (rel.has_root_path() || first == "..") {
      error = QString("unsafe entry name %1").arg(QString::fromStdString(name));
      break;
    }
    if (first.rfind(".git", 0) == 0) continue;

    const fs::path out = dest / rel;
    std::error_code ec;
    if (name.back() == '/') {
      fs::create_directories(out, ec);
      if (ec) error = QString::fromStdString("cannot create " + out.generic_string());
      continue;
    }
    fs::create_directories(out.parent_path(), ec);
    zip_file_t *in = zip_fopen_index(archive, i, 0);
    std::ofstream os(out, std::ios::binary);
    if (!in || !os) {
      if (in) zip_fclose(in);
      error = QString::fromStdString("cannot extract " + rel.generic_string());
      break;
    }
    char buffer[64 * 1024];
    zip_int64_t n;
    while ((n = zip_fread(in, buffer, sizeof(buffer))) > 0) {
      os.write(buffer, static_cast<std::streamsize>(n));
    }
    zip_fclose(in);
    if (n < 0 || !os) error = QString::fromStdString("cannot extract " + rel.generic_string());
  }
  zip_discard(archive);
  if (!error.isEmpty()) return failed(error);

  if (!fs::exists(dest / "std.scad")) return failed("the release has no std.scad");
  staging.setAutoRemove(false);
  return {staging.path(), BOSL2Library::readVersion(dest), {}};
}

void removeScratch(const fs::path& root)
{
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (entry.path().filename().generic_string().rfind(kScratchPrefix, 0) == 0) {
      std::error_code rmec;
      fs::remove_all(entry.path(), rmec);
    }
  }
}

QString versionText(const Version& version)
{
  return QString::fromStdString("v" + BOSL2Library::toString(version));
}

}  // namespace

BOSL2Updater::BOSL2Updater(QObject *parent) : QObject(parent) {}

void BOSL2Updater::start()
{
  root = BOSL2Library::updateRoot();
  if (root.empty()) return;
  std::error_code ec;
  fs::create_directories(root, ec);
  if (ec) {
    PRINTDB("BOSL2 update skipped: cannot create %s", root.generic_string());
    return;
  }

  // One updater at a time across OpenSCAD instances; the others use what is on disk.
  lock = std::make_unique<QLockFile>(QString::fromStdString((root / ".update.lock").generic_string()));
  lock->setStaleLockTime(10 * 60 * 1000);
  if (!lock->tryLock(0)) {
    lock.reset();
    return;
  }
  removeScratch(root);

  QNetworkReply *reply = nam.get(makeRequest(QUrl(kLatestReleaseUrl), 30 * 1000));
  connect(reply, &QNetworkReply::finished, this, [this, reply]() { onRelease(reply); });
}

void BOSL2Updater::onRelease(QNetworkReply *reply)
{
  reply->deleteLater();
  if (reply->error() != QNetworkReply::NoError) {
    PRINTDB("BOSL2 update check failed: %s", reply->errorString().toStdString());
    finish();
    return;
  }
  const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
  const auto latest = BOSL2Library::parseTag(release.value("tag_name").toString().toStdString());
  const QUrl zipball(release.value("zipball_url").toString());
  if (!latest || !zipball.isValid()) {
    PRINTDB("BOSL2 update check: unexpected reply from %s", kLatestReleaseUrl);
    finish();
    return;
  }
  const auto current = BOSL2Library::activeVersion();
  if (current && *latest <= *current) {
    finish();
    return;
  }

  QNetworkReply *download = nam.get(makeRequest(zipball, 120 * 1000));
  connect(download, &QNetworkReply::finished, this,
          [this, download, version = *latest]() { onDownload(download, version); });
}

void BOSL2Updater::onDownload(QNetworkReply *reply, const Version& latest)
{
  reply->deleteLater();
  if (reply->error() != QNetworkReply::NoError) {
    PRINTDB("BOSL2 download failed: %s", reply->errorString().toStdString());
    finish();
    return;
  }

  auto *watcher = new QFutureWatcher<Unpacked>(this);
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, latest]() {
    watcher->deleteLater();
    install(watcher->result(), latest);
  });
  watcher->setFuture(
    QtConcurrent::run([data = reply->readAll(), root = root]() { return unpack(data, root); }));
}

// Runs on the main thread, as compiles do, so no compile sees the swap half done.
void BOSL2Updater::install(const Unpacked& unpacked, const Version& latest)
{
  const auto warn = [&](const QString& reason) {
    LOG(message_group::Warning, "Could not update BOSL2 to %1$s: %2$s",
        versionText(latest).toStdString(), reason.toStdString());
  };
  if (!unpacked.error.isEmpty()) {
    warn(unpacked.error);
    finish();
    return;
  }

  const fs::path staging = unpacked.dir.toStdString();
  std::error_code ec;
  const auto previous = BOSL2Library::activeVersion();
  if (!unpacked.version) {
    warn("its version.scad has no BOSL_VERSION");
    fs::remove_all(staging, ec);
    finish();
    return;
  }
  if (previous && *unpacked.version <= *previous) {
    fs::remove_all(staging, ec);
    finish();
    return;
  }

  const fs::path target = root / "BOSL2";
  const fs::path old = staging.generic_string() + "-old";
  if (fs::exists(target)) {
    fs::rename(target, old, ec);
    if (ec) {
      warn(QString::fromStdString("cannot move aside " + target.generic_string() + ": " + ec.message()));
      fs::remove_all(staging, ec);
      finish();
      return;
    }
  }
  fs::rename(staging, target, ec);
  if (ec) {
    warn(QString::fromStdString("cannot move it into " + target.generic_string() + ": " + ec.message()));
    std::error_code restore;
    if (fs::exists(old)) fs::rename(old, target, restore);
    fs::remove_all(staging, restore);
    finish();
    return;
  }
  fs::remove_all(old, ec);

  // The first download, or one replacing a copy older than the built-in, changes
  // which folder is on the library path.
  refresh_library_path();
  if (previous) {
    LOG("BOSL2 updated from %1$s to %2$s; the next render uses it.",
        versionText(*previous).toStdString(), versionText(*unpacked.version).toStdString());
  } else {
    LOG("BOSL2 %1$s installed; the next render uses it.",
        versionText(*unpacked.version).toStdString());
  }
  finish();
}

void BOSL2Updater::finish()
{
  lock.reset();
}
