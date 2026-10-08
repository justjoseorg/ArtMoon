// Derived from ArtMoon's appupdate by Onaiaku — https://github.com/onaiaku/ArtMoon
// (commits 9ebfc60, 1418d3f, 6ed4df2), GPLv3. What was changed and why is in appupdate.h.

#include "appupdate.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <SDL.h>

namespace {

const char* const Owner = "onaiaku";

// "Don't remind me" in the startup prompt: the tag it was ticked for.
const char* const SkippedVersionKey = "appupdate/skippedversion";

// The lookup is a few kilobytes of JSON asked for from the Settings screen or at startup, with
// the event loop running either way — Qt's own transfer timeout is safe there.
constexpr int LookupTimeoutMs = 15000;

// The download's watchdog (see m_StallWatch in the header). A tick later than LateTickMs means
// the main thread was held — by a stream, in practice — and says nothing about the network.
constexpr int StallTimeoutMs = 30000;
constexpr int StallTickMs = 5000;
constexpr int LateTickMs = 2 * StallTickMs;

// The release asset THIS platform updates with, anchored at both ends so a ".exe.sig"
// or a "_Installer_debug.exe" cannot qualify. Both names carry the version and it has
// to be the tag's own, so an old file left attached to a newer release is not mistaken
// for it.
//
// The pattern is per-platform rather than "the installer": a Windows build and a Linux
// build do not share an update mechanism, and matching the .exe from Linux is exactly
// how this used to offer an update it then refused to perform.
#ifdef Q_OS_WIN32
// OutputBaseFilename=ArtMoon_{#AppVersion}_Installer in ArtMoon.iss.
const QRegularExpression& assetPattern()
{
    static const QRegularExpression re(
        QStringLiteral("^ArtMoon_(\\d+(?:\\.\\d+){1,3})_Installer\\.exe$"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}
#else
// The CI AppImage — the glob actions/upload-artifact is given in build-linux.yml.
// One AppImage per architecture, so only this build's own may qualify.
#if defined(Q_PROCESSOR_ARM_64)
#define ARTMOON_APPIMAGE_ARCH "aarch64"
#else
#define ARTMOON_APPIMAGE_ARCH "x86_64"
#endif
const QRegularExpression& assetPattern()
{
    static const QRegularExpression re(
        QStringLiteral("^ArtMoon-(\\d+(?:\\.\\d+){1,3})-" ARTMOON_APPIMAGE_ARCH "\\.AppImage$"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// A program we could run by name, looked up the way a shell would rather than trusted
// to PATH order.
bool haveProgram(const QString& name)
{
    return !QStandardPaths::findExecutable(name).isEmpty();
}
#endif

// SHA-256 of a file on disk, lower-case hex; empty if it cannot be read.
QByteArray fileSha256(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return hash.result().toHex();
}

}

AppUpdate::AppUpdate(QObject* parent)
    : QObject(parent),
      m_Nam(new QNetworkAccessManager(this))
{
    m_Nam->setStrictTransportSecurityEnabled(true);
    // Asset downloads redirect from github.com to GitHub's storage host; https to https only.
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    m_StallWatch.setInterval(StallTickMs);
    connect(&m_StallWatch, &QTimer::timeout, this, &AppUpdate::checkStall);
}

AppUpdate::~AppUpdate()
{
    m_StallWatch.stop();
    if (m_Download != nullptr) {
        // Disconnected first: abort() emits finished synchronously, and the handler would
        // otherwise run from inside this destructor and signal a QML engine being torn down.
        disconnect(m_Download, nullptr, this, nullptr);
        m_Download->abort();
    }
    delete m_File;      // uncommitted QSaveFile: discards its temp file
}

QList<int> AppUpdate::parseVersion(const QString& version)
{
    QString s = version;
    // U+FEFF is what a UTF-8 BOM in version.txt becomes by the time it reaches VERSION_STR.
    s.remove(QChar(0xFEFF));
    s = s.trimmed();
    if (s.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        s.remove(0, 1);
    }

    const QStringList parts = s.split(QLatin1Char('.'));
    if (parts.size() < 2 || parts.size() > 4) {
        return {};
    }

    QList<int> numbers;
    for (const QString& part : parts) {
        bool ok = false;
        const int n = part.toInt(&ok);
        // toInt accepts a sign; a version component never has one.
        if (!ok || n < 0 || part.startsWith(QLatin1Char('+'))) {
            return {};
        }
        numbers.append(n);
    }
    return numbers;
}

int AppUpdate::compareVersions(const QString& a, const QString& b)
{
    const QList<int> va = parseVersion(a);
    const QList<int> vb = parseVersion(b);
    if (va.isEmpty() || vb.isEmpty()) {
        return 0;
    }

    const int n = qMax(va.size(), vb.size());
    for (int i = 0; i < n; i++) {
        const int x = i < va.size() ? va[i] : 0;
        const int y = i < vb.size() ? vb[i] : 0;
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return 0;
}

QString AppUpdate::installedVersion()
{
    return QString(VERSION_STR);
}

QString AppUpdate::downloadDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("ArtMoon-update"));
}

void AppUpdate::clearDownloads()
{
    // Best effort. The installer of the update that just ran can still be open for the few
    // seconds Setup takes to exit after relaunching us; whatever is locked now goes next time.
    QDir(downloadDir()).removeRecursively();
}

QString AppUpdate::appImagePath()
{
#ifdef Q_OS_WIN32
    return {};
#else
    // The AppImage runtime exports APPIMAGE with the absolute path of the file it is
    // running. It is the only trustworthy answer to "which file do I replace": the
    // process's own /proc/self/exe points inside the mounted squashfs, at a path that
    // does not exist outside the mount and carries no write permission.
    return qEnvironmentVariable("APPIMAGE");
#endif
}

bool AppUpdate::updateAvailable() const
{
    return !m_LatestArtMoon.isEmpty()
            && !parseVersion(m_LatestArtMoon).isEmpty()
            && !parseVersion(installedVersion()).isEmpty()
            && compareVersions(installedVersion(), m_LatestArtMoon) < 0;
}

bool AppUpdate::shouldPrompt() const
{
    // An installer to update with, not just a newer tag: "Yes" leads to Update now, and a
    // release without one would put the user straight onto an error.
    if (!updateAvailable() || !m_HasAsset) {
        return false;
    }

    const QString skipped = QSettings().value(QLatin1String(SkippedVersionKey)).toString();
    if (skipped.isEmpty() || parseVersion(skipped).isEmpty()) {
        return true;
    }
    // Skipping one release also silences an older one, never a newer one.
    return compareVersions(m_LatestArtMoon, skipped) > 0;
}

void AppUpdate::skipLatestVersion()
{
    if (m_LatestArtMoon.isEmpty()) {
        return;
    }
    QSettings().setValue(QLatin1String(SkippedVersionKey), m_LatestArtMoon);
}

void AppUpdate::setState(State state, const QString& failureReason)
{
    if (m_State == state && m_FailureReason == failureReason) {
        return;
    }
    m_State = state;
    m_FailureReason = failureReason;
    emit stateChanged();
}

void AppUpdate::fail(const QString& reason)
{
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "AppUpdate: %s", qPrintable(reason));
    setState(Failed, reason);
}

QNetworkReply* AppUpdate::getJson(const QString& repo)
{
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/%1/%2/releases/latest")
                                 .arg(QLatin1String(Owner), repo)));
    request.setRawHeader("Accept", "application/vnd.github+json");
    // GitHub's API refuses requests without a User-Agent. Qt adds a generic one when none is
    // set; naming ourselves is what GitHub asks for.
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("ArtMoon/%1").arg(parseVersion(installedVersion()).isEmpty()
                                                           ? QStringLiteral("unknown")
                                                           : installedVersion().remove(QChar(0xFEFF)).trimmed()));
    request.setTransferTimeout(LookupTimeoutMs);
    return m_Nam->get(request);
}

void AppUpdate::checkLatest()
{
    // A new Settings screen opening mid-update must not pull the release, or the verified
    // installer waiting for Install now, out from under it.
    if (holdsDownload()) {
        return;
    }

    // Leftovers of earlier updates, ~28 MB each. Nothing is in use: holdsDownload() is false.
    clearDownloads();

    setState(Checking);

    QNetworkReply* sl = getJson(QStringLiteral("ArtMoon"));
    connect(sl, &QNetworkReply::finished, this, [this, sl]() { handleArtMoon(sl); });

    QNetworkReply* st = getJson(QStringLiteral("ArtLight"));
    connect(st, &QNetworkReply::finished, this, [this, st]() { handleArtLight(st); });
}

void AppUpdate::handleArtLight(QNetworkReply* reply)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;     // the label keeps saying "checking…", as it always has on a failed lookup
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    const QString tag = doc.object().value(QStringLiteral("tag_name")).toString();
    if (!tag.isEmpty() && tag != m_LatestArtLight) {
        m_LatestArtLight = tag;
        emit latestChanged();
    }
}

void AppUpdate::handleArtMoon(QNetworkReply* reply)
{
    reply->deleteLater();

    // An update started while this lookup was in flight (updateNow() accepts Checking) is
    // working from the asset it was started with; this answer must not swap it out.
    if (holdsDownload()) {
        return;
    }
    const bool ownsState = (m_State == Checking);

    if (reply->error() != QNetworkReply::NoError) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "AppUpdate: release lookup failed: %s",
                    qPrintable(reply->errorString()));
        if (ownsState) {
            setState(Idle);
        }
        return;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &err);
    const QJsonObject obj = doc.object();
    const QString tag = obj.value(QStringLiteral("tag_name")).toString();
    if (doc.isNull() || tag.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "AppUpdate: release lookup returned no tag: %s",
                    qPrintable(err.errorString()));
        if (ownsState) {
            setState(Idle);
        }
        return;
    }

    // The installer for THIS release: its name must match the pattern and carry the tag's own
    // version, so an old installer left attached to a newer release is not mistaken for it.
    m_HasAsset = false;
    m_Asset = Asset();
    const QList<int> tagVersion = parseVersion(tag);
    const QJsonArray assets = obj.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue& value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        const QRegularExpressionMatch match = assetPattern().match(name);
        if (!match.hasMatch() || parseVersion(match.captured(1)) != tagVersion) {
            continue;
        }

        m_Asset.url = asset.value(QStringLiteral("browser_download_url")).toString();
        m_Asset.name = name;
        const QString digest = asset.value(QStringLiteral("digest")).toString();
        if (digest.startsWith(QLatin1String("sha256:"), Qt::CaseInsensitive)) {
            m_Asset.sha256 = digest.mid(7).trimmed().toLower().toLatin1();
        }
        m_HasAsset = !m_Asset.url.isEmpty();
        break;
    }

    // ⚠️ Changed is emitted after the asset is known, not before: the startup prompt reacts to
    // this signal and asks shouldPrompt(), which needs m_HasAsset. The tag alone may not change
    // (a second lookup finding the same release), so the asset answer is signalled either way.
    m_LatestArtMoon = tag;
    emit latestChanged();
    if (ownsState) {
        setState(Idle);
    }
}

void AppUpdate::setInstallBlocked(bool blocked)
{
    if (m_InstallBlocked == blocked) {
        return;
    }
    m_InstallBlocked = blocked;
    emit installBlockedChanged();
}

void AppUpdate::updateNow()
{
    switch (m_State) {
    case Ready:
        // Refused here as well as greyed in the button's label: this is the one call that
        // quits the app, so it does not trust the QML to have checked.
        if (m_InstallBlocked) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "AppUpdate: install held — something on Home is still in progress");
            return;
        }
        launchInstaller();
        return;
    case Downloading:
    case Launching:
        return;
    case Idle:
    case Checking:
    case Failed:
        // Checking is NOT a reason to refuse. The button is on screen because an earlier lookup
        // found a newer release, and a press while Settings re-checks used to be dropped
        // without a word; the asset from that lookup is the one to use, and
        // handleArtMoon() leaves it alone once the download has started.
        startDownload();
        return;
    }
}

void AppUpdate::startDownload()
{
#ifndef Q_OS_WIN32
    // Linux. Both of these are checked BEFORE the download rather than after it: a user who
    // is about to be asked for a password, or who is going to be told there is no way to
    // write the file at all, should hear it before 80 MB arrive.
    //
    // Self-update replaces the AppImage we are running from, so there has to be one. A
    // source build, or a copy unpacked with --appimage-extract, has no file to replace and
    // no honest way to update itself — say that, instead of the Windows-only message this
    // used to give, which was true of the platform and useless about the situation.
    const QString appImage = appImagePath();
    if (appImage.isEmpty()) {
        fail(tr("This is not an AppImage copy of ArtMoon, so it cannot update itself"));
        return;
    }
    // install.sh puts it in /usr/local/bin, which is root's; a copy the user downloaded
    // themselves is not. Only the first case needs pkexec, and only the first case can
    // fail for want of it.
    const QString appImageDir = QFileInfo(appImage).absolutePath();
    if (!QFileInfo(appImageDir).isWritable() && !haveProgram(QStringLiteral("pkexec"))) {
        fail(tr("Cannot write %1 and pkexec is not installed").arg(appImageDir));
        return;
    }
#endif

    if (!updateAvailable()) {
        fail(tr("No newer version to install"));
        return;
    }
    if (!m_HasAsset) {
        fail(tr("This release has no installer"));
        return;
    }
    // Refused before the download, not after: without a published digest there is nothing to
    // check the file against, and it would run elevated.
    if (m_Asset.sha256.size() != 64) {
        fail(tr("This release has no checksum"));
        return;
    }

    clearDownloads();
    if (!QDir().mkpath(downloadDir())) {
        fail(tr("Could not create the download folder"));
        return;
    }

    delete m_File;
    m_File = new QSaveFile(QDir(downloadDir()).filePath(m_Asset.name));
    if (!m_File->open(QIODevice::WriteOnly)) {
        fail(tr("Could not save the installer"));
        delete m_File;
        m_File = nullptr;
        return;
    }

    m_Hash.reset();
    m_WriteFailed = false;
    m_Stalled = false;
    m_InstallerPath.clear();
    m_Progress = -1;
    emit progressChanged();
    setState(Downloading);

    QNetworkRequest request{QUrl(m_Asset.url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ArtMoon"));
    m_Download = m_Nam->get(request);

    connect(m_Download, &QNetworkReply::readyRead, this, &AppUpdate::handleDownloadData);
    connect(m_Download, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        m_SinceData.restart();
        const int p = total > 0 ? int(received * 100 / total) : -1;
        if (p != m_Progress) {
            m_Progress = p;
            emit progressChanged();
        }
    });
    connect(m_Download, &QNetworkReply::finished, this, &AppUpdate::handleDownloadFinished);

    m_SinceData.start();
    m_SinceTick.start();
    m_StallWatch.start();
}

void AppUpdate::checkStall()
{
    if (m_Download == nullptr) {
        m_StallWatch.stop();
        return;
    }

    const bool late = m_SinceTick.elapsed() > LateTickMs;
    m_SinceTick.restart();
    if (late) {
        // The main thread was held (a stream, in practice), so whatever arrived meanwhile is
        // still queued behind this tick. Not evidence of a stall: start the clock over.
        m_SinceData.restart();
        return;
    }

    if (m_SinceData.elapsed() > StallTimeoutMs) {
        m_Stalled = true;
        m_Download->abort();    // finishes with OperationCanceledError, handled below
    }
}

void AppUpdate::handleDownloadData()
{
    if (m_Download == nullptr || m_File == nullptr) {
        return;
    }
    m_SinceData.restart();
    const QByteArray chunk = m_Download->readAll();
    m_Hash.addData(chunk);
    if (m_File->write(chunk) != chunk.size()) {
        m_WriteFailed = true;
        m_File->cancelWriting();
        m_Download->abort();    // finishes with an error, handled below
    }
}

void AppUpdate::handleDownloadFinished()
{
    QNetworkReply* reply = m_Download;
    m_Download = nullptr;
    m_StallWatch.stop();
    if (reply == nullptr) {
        return;
    }
    reply->deleteLater();

    // Whatever is still buffered: readyRead is not guaranteed to have drained it.
    if (reply->error() == QNetworkReply::NoError && m_File != nullptr) {
        const QByteArray tail = reply->readAll();
        m_Hash.addData(tail);
        if (m_File->write(tail) != tail.size()) {
            m_WriteFailed = true;
            m_File->cancelWriting();
        }
    }

    const auto discard = [this]() {
        delete m_File;      // not committed: the temp file goes with it
        m_File = nullptr;
    };

    if (m_WriteFailed) {
        discard();
        fail(tr("Could not save the installer"));
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "AppUpdate: download failed: %s",
                    qPrintable(reply->errorString()));
        discard();
        fail(m_Stalled ? tr("Download stalled") : tr("Download failed"));
        return;
    }

    if (m_Hash.result().toHex() != m_Asset.sha256) {
        discard();
        fail(tr("Checksum mismatch"));
        return;
    }

    const QString path = m_File->fileName();
    const bool committed = m_File->commit();
    delete m_File;
    m_File = nullptr;
    if (!committed) {
        fail(tr("Could not save the installer"));
        return;
    }

    // Nothing runs from here. The download ending is not a moment the user chose, and every
    // defect this feature has had came from treating it as one — see "Two presses" in the
    // header. The button now offers Install now.
    m_InstallerPath = path;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "AppUpdate: installer verified, ready to install");
    setState(Ready);
}

void AppUpdate::launchInstaller()
{
    // Checked again, not trusted from the download: Install now can come a long time after,
    // and the file has spent that time in a folder the user's own processes can write to.
    if (m_InstallerPath.isEmpty() || !QFile::exists(m_InstallerPath)) {
        fail(tr("The downloaded installer is gone"));
        return;
    }
    if (fileSha256(m_InstallerPath) != m_Asset.sha256) {
        fail(tr("Checksum mismatch"));
        return;
    }

#ifdef Q_OS_WIN32
    // No switches: the wizard runs visibly, so the user sees every step, and its last page
    // carries "Launch StreamLight", which is what reopens the app — see "The installer runs
    // visibly" in the header. Setup.e64 is asInvoker and elevates itself, so this starts
    // without a prompt of ours and the one UAC prompt is Setup's own.
    if (!QProcess::startDetached(QDir::toNativeSeparators(m_InstallerPath), QStringList())) {
        fail(tr("Could not start the installer"));
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "AppUpdate: installer started, quitting");
    setState(Launching);
    // The installer cannot replace StreamLight.exe while this process holds it.
    QCoreApplication::quit();
#else
    // There is no installer to run on Linux: the AppImage IS the application.
    launchLinuxSwap();
#endif
}

#ifndef Q_OS_WIN32

// ── Linux: replacing the AppImage that is currently running ────────────────────────────
//
// This cannot be done in-process, and the reason is worth stating because the obvious
// version of it does not work. The file to replace is the file we are running; after the
// swap the app has to start again — and ArtMoon has a single-instance guard, so a launch
// while the old process is still up only raises it and exits. So the order has to be
// quit, then replace, then launch, and every one of those steps after the first has to be
// taken by something that outlives this process.
//
// Hence the two scripts below. They are written from code rather than shipped as files,
// so the script and the build that wrote it can never be a version apart.

// The half that waits. Runs as the user, which is what makes the relaunch at the end
// correct for free: it inherits this process's environment, so the new copy comes up on
// the same display and session bus the old one was using. A root process relaunching a
// desktop app has to reconstruct all of that and gets it wrong on some sessions.
const char* const SwapHelperScript = R"SH(#!/bin/sh
# ArtMoon self-update. Written by the running app at update time.
#   $1 pid     the app that wrote this; the swap waits for it to exit
#   $2 new     the verified AppImage, just downloaded
#   $3 target  the AppImage in use, which is the file that gets replaced
#   $4 work    this script's own directory
set -u

pid="$1"; new="$2"; target="$3"; work="$4"
dir="$(dirname "$target")"

# 1. Wait for ArtMoon to go. Its quit was asked for, so this waits rather than kills.
#    0.2s up to fifteen minutes: long enough for an unhurried exit, bounded so that a
#    wedged process cannot hold the update open forever.
i=0
while [ "$i" -lt 4500 ]; do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.2
    i=$((i + 1))
done

# 2. Replace it. A copy in a directory the user owns needs no privilege at all; install.sh
#    puts it in /usr/local/bin, which is root's, and there pkexec is what asks for the
#    password. Either way the move itself is the same script, so the two paths cannot
#    drift apart. Failures are swallowed on purpose: step 3 has to run regardless.
if [ -w "$dir" ]; then
    sh "$work/artmoon-swap.sh" "$new" "$target" || true
else
    pkexec /bin/sh "$work/artmoon-swap.sh" "$new" "$target" || true
fi

# 3. Open ArtMoon again either way — and this is the point of swallowing the failure
#    above. A password prompt that gets dismissed, or a copy that fails, must not leave
#    the user with no app at all: in both cases the old build is still sitting there
#    untouched, and it opens exactly as it was. setsid puts the new process in its own
#    session so this script's exit cannot take it down with it.
if command -v setsid >/dev/null 2>&1; then
    setsid "$target" </dev/null >/dev/null 2>&1 &
else
    "$target" </dev/null >/dev/null 2>&1 &
fi
)SH";

// The move itself. Staged into the target's own directory and then renamed over it: a
// rename inside one filesystem is atomic, so nothing can ever observe a half-written
// ArtMoon — which is exactly what copying straight onto the target would produce, both
// for a running instance and for a desktop entry clicked mid-copy.
const char* const SwapMoveScript = R"SH(#!/bin/sh
# ArtMoon self-update: replace one AppImage with another. $1 new, $2 target.
set -u

new="$1"; target="$2"
dir="$(dirname "$target")"

[ -f "$new" ] || exit 1

staged="$dir/.artmoon-staged.$$"
cp -f "$new" "$staged" || { rm -f "$staged"; exit 1; }
chmod 755 "$staged" 2>/dev/null || true

# Keep whoever owned the old file. install.sh's copy is the user's; anything installed by
# the hand-over is not. Replacing it as root would otherwise leave it root-owned and the
# next update needing a password it did not need before.
owner="$(stat -c '%u:%g' "$target" 2>/dev/null)"
if [ -n "$owner" ]; then
    chown "$owner" "$staged" 2>/dev/null || true
fi

# One previous build is kept, overwritten each time. Without it a bad download costs a
# fresh download; with it, the file is right there.
mv -f "$target" "$target.previous" 2>/dev/null || true
mv -f "$staged" "$target" || { rm -f "$staged"; exit 1; }
rm -f "$new" 2>/dev/null || true
exit 0
)SH";

void AppUpdate::launchLinuxSwap()
{
    const QString target = appImagePath();
    if (target.isEmpty()) {
        fail(tr("This is not an AppImage copy of ArtMoon, so it cannot update itself"));
        return;
    }

    // A directory of our own inside the download folder, 0700. This process writes the
    // scripts and then runs one of them, and the temp directory is shared with every other
    // user on the machine — the mode is what keeps a script from being swapped between the
    // write and the exec.
    const QString work = QDir(downloadDir()).filePath(QStringLiteral("swap"));
    QDir().mkpath(work);
    QFile::setPermissions(work, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                               | QFileDevice::ExeOwner);

    const auto writeScript = [&work](const QString& name, const char* body) {
        QFile file(QDir(work).filePath(name));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        const QByteArray bytes(body);
        return file.write(bytes) == bytes.size();
    };

    if (!writeScript(QStringLiteral("artmoon-swap.sh"), SwapMoveScript)
            || !writeScript(QStringLiteral("artmoon-update.sh"), SwapHelperScript)) {
        fail(tr("Could not write the updater"));
        return;
    }

    // Detached, and started through /bin/sh rather than by the script's own path: this
    // process quits in the next breath, and the helper is the thing that has to outlive it.
    if (!QProcess::startDetached(QStringLiteral("/bin/sh"),
                                 {QDir(work).filePath(QStringLiteral("artmoon-update.sh")),
                                  QString::number(QCoreApplication::applicationPid()),
                                  m_InstallerPath, target, work})) {
        fail(tr("Could not start the updater"));
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "AppUpdate: update helper started, quitting to let it replace %s",
                qPrintable(target));
    setState(Launching);
    QCoreApplication::quit();
}

#endif  // !Q_OS_WIN32
