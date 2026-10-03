#include "usbipdevices.h"
#include "usbipdevicelist.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QQmlEngine>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVariantMap>

#include "settings/streamingpreferences.h"

namespace {

/*
 * The privileged local helper that owns the bind step. Named here rather than in the
 * installer only, because this file has to be able to say whether it is present.
 *
 * The service is what makes a share toggle mean anything: `bind` is refused to a normal
 * user (verified on niks-minipc 2026-10-03, while `attach` on the importer runs fine
 * unelevated). So this name is the single source of truth for "can this build share".
 */
const char *kServiceNameWindows = "ArtMoonInputService";
const char *kHelperPathLinux    = "/usr/libexec/artmoon-input-service";

/* Platform tool that enumerates. `usbipd` is not on PATH by default on Windows. */
QString usbipdProgram()
{
#ifdef Q_OS_WIN32
    const QString installed = QStringLiteral("C:/Program Files/usbipd-win/usbipd.exe");
    if (QFileInfo::exists(installed)) {
        return installed;
    }
    return QStandardPaths::findExecutable(QStringLiteral("usbipd"));
#else
    return QStandardPaths::findExecutable(QStringLiteral("usbip"));
#endif
}

/*
 * The sha256 of a file, or an empty string if it cannot be read.
 *
 * Used to decide whether an install actually happened. Existence alone is not enough: a
 * privileged copy that read nothing still leaves a file at the destination, and presence is
 * what turns the toggles live — so a helper that cannot bind would look installed.
 */
QString fileSha256(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) {
        return QString();
    }
    return QString::fromLatin1(hash.result().toHex());
}

/*
 * Linux has no state column: "offered to an importer" means "bound to the usbip-host
 * driver". That directory does not exist until the module is loaded, so an empty result
 * means "nothing is exported", NOT "the tool failed" — an important distinction that this
 * returns as an empty list either way, and the caller must not read it as an error.
 */
QStringList exportedBusidsLinux()
{
    QStringList out;
#ifndef Q_OS_WIN32
    QDir dir(QStringLiteral("/sys/bus/usb/drivers/usbip-host"));
    if (dir.exists()) {
        const auto entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto &e : entries) {
            if (e.contains(QLatin1Char('-'))) {
                out.append(e);
            }
        }
    }
#endif
    return out;
}

} // namespace

UsbIpDevices::UsbIpDevices(QObject *parent)
    : QObject(parent)
{
    refresh();
}

UsbIpDevices::~UsbIpDevices() = default;

UsbIpDevices *UsbIpDevices::get(QQmlEngine *qmlEngine)
{
    static UsbIpDevices *instance = nullptr;
    if (!instance) {
        instance = new UsbIpDevices();
        // Deliberately owned by no engine: the same object must survive a page reload, or
        // the remembered toggles would be re-read from disk every time the tab is opened.
        if (qmlEngine) {
            instance->setParent(qmlEngine);
        }
    }
    return instance;
}

/*
 * Where the helper sits in this build.
 *
 * An AppImage runs the app out of usr/bin inside a read-only mount, so the helper is a
 * sibling tree at usr/libexec — deliberately the same relative place it occupies on the
 * system. That relationship is what keeps this honest: installed normally, this resolves to
 * the real system path, which exists only once the helper is genuinely installed, so no
 * offer to set it up is ever made where there is nothing to do.
 *
 * Windows returns nothing, because its service arrives with the installer — the same shape as
 * the service usbipd-win installs for itself — so there is nothing for the app to place.
 */
QString UsbIpDevices::bundledHelperPath()
{
#ifdef Q_OS_WIN32
    return QString();
#else
    const QDir appDir(QCoreApplication::applicationDirPath());
    return appDir.absoluteFilePath(QStringLiteral("../libexec/artmoon-input-service"));
#endif
}

void UsbIpDevices::refresh()
{
    const QString program = usbipdProgram();

    m_Available = false;
    m_UnavailableReason.clear();
    m_Devices.clear();

    // ── Can this machine be shared from at all? ───────────────────────────────
    if (program.isEmpty()) {
#ifdef Q_OS_WIN32
        m_UnavailableReason = tr("USB/IP is not installed on this PC.");
#else
        m_UnavailableReason = tr("USB/IP is not installed on this PC.");
#endif
    }
    else {
        QProcess proc;
#ifdef Q_OS_WIN32
        proc.start(program, { QStringLiteral("list") });
#else
        proc.start(program, { QStringLiteral("list"), QStringLiteral("-l") });
#endif
        if (proc.waitForFinished(2000) && proc.exitCode() == 0) {
            const QString output = QString::fromLocal8Bit(proc.readAllStandardOutput());

#ifdef Q_OS_WIN32
            const auto parsed = UsbIpDeviceList::parseUsbipdList(output);
#else
            const auto parsed = UsbIpDeviceList::parseUsbipList(output, exportedBusidsLinux());
#endif
            for (const auto &dev : parsed) {
                QVariantMap row;
                row.insert(QStringLiteral("busid"), dev.busid);
                row.insert(QStringLiteral("vidPid"), dev.vidPid);
                row.insert(QStringLiteral("description"), dev.description);
                row.insert(QStringLiteral("shared"), dev.exported);
                row.insert(QStringLiteral("wanted"), false);
                m_Devices.append(row);
            }
            m_Available = true;
        }
        else {
            // Carry the tool's own words rather than inventing a reason. An elevated-only
            // enumeration is a real possibility on Windows, and if that is what happened the
            // caption should say so in usbipd's language, not ours.
            QString err = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
            if (err.isEmpty()) {
                err = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
            }
            m_UnavailableReason = err.isEmpty()
                ? tr("USB/IP did not answer on this PC.")
                : err.split(QLatin1Char('\n')).first().trimmed();
        }
    }

    // ── Can a toggle do anything? ─────────────────────────────────────────────
#ifdef Q_OS_WIN32
    // Present == the installer created the service. Nothing to remember to flip: landing
    // the service is what turns the toggles live.
    QSettings services(QStringLiteral("HKEY_LOCAL_MACHINE\\\\SYSTEM\\\\CurrentControlSet\\\\Services"),
                       QSettings::NativeFormat);
    m_CanShare = services.childGroups().contains(QLatin1String(kServiceNameWindows));
#else
    /*
     * Ownership is part of presence. A helper that is not root-owned cannot bind anything, so
     * counting it as present would light the toggles against something that refuses every
     * request. After a privileged install it is also the property that says the install really
     * happened as intended rather than leaving a file behind.
     */
    const QFileInfo systemHelper{QLatin1String(kHelperPathLinux)};
    m_CanShare = systemHelper.exists() && systemHelper.ownerId() == 0;
#endif

    // ── Is there something we could do about it? ──────────────────────────────
    // Only worth offering when a helper is actually here to place and it is not in place
    // already. Derived from the files on disk, never from a constant someone has to remember
    // to update — the same rule canShare itself is held to.
    m_CanInstallService = false;
#ifndef Q_OS_WIN32
    m_CanInstallService = !m_CanShare && QFileInfo::exists(bundledHelperPath());
#endif

    if (m_CanShare) {
        m_CanShareReason.clear();
    }
    else if (m_CanInstallService) {
        // The service is present but not placed: name the remedy rather than the absence.
        m_CanShareReason = tr("Sharing a device needs administrator rights. This build includes "
                              "the ArtMoon input service, which does that for you, but it is not "
                              "set up on this PC yet.");
    }
    else {
        m_CanShareReason = tr("Sharing a device needs administrator rights, which the ArtMoon input service "
             "does on your behalf. It is not installed in this build yet.");
    }

    rebuild();
}

void UsbIpDevices::rebuild()
{
    // Mark each row against the remembered intent. Kept as a separate pass from refresh()
    // so recording a toggle does not have to re-run the enumeration.
    const QStringList wanted = wantedBusids();

    for (auto &entry : m_Devices) {
        QVariantMap row = entry.toMap();
        row.insert(QStringLiteral("wanted"), wanted.contains(row.value(QStringLiteral("busid")).toString()));
        entry = row;
    }

    emit devicesChanged();
}

QStringList UsbIpDevices::wantedBusids() const
{
    return StreamingPreferences::get()->usbIpWantedBusids;
}

void UsbIpDevices::storeWantedBusids(const QStringList &busids)
{
    auto *prefs = StreamingPreferences::get();
    if (prefs->usbIpWantedBusids == busids) {
        return;
    }
    prefs->usbIpWantedBusids = busids;
    prefs->save();
}

void UsbIpDevices::setWanted(const QString &busid, bool wanted)
{
    if (busid.isEmpty()) {
        return;
    }

    QStringList busids = wantedBusids();
    if (wanted) {
        if (!busids.contains(busid)) {
            busids.append(busid);
        }
    }
    else {
        busids.removeAll(busid);
    }
    storeWantedBusids(busids);

    // Reconcile with the service. Absent service == the intent is still recorded, and
    // `shared` is left alone, so the tab shows "wanted, not shared" instead of pretending.
    //
    // The desired set is handed over explicitly rather than read from a file: the helper runs
    // as root, so it must never be pointed at a path an unprivileged process can rewrite.
    if (m_CanShare) {
#ifdef Q_OS_WIN32
        /*
         * The service is reached over its own local pipe — not through `sc start`, because a
         * service that is already running refuses a start (error 1056), so start arguments
         * could only ever deliver the first desired set and never another one.
         *
         * The PLAN is built here rather than in the service. This is the side that parses
         * usbipd's table, so this is the side that knows what needs releasing — one parser in
         * the product, and it is the one with tests. The service executes and refuses; it does
         * not decide.
         */
        QStringList tokens;
        for (const auto &entry : m_Devices) {
            const QVariantMap row = entry.toMap();
            const QString busid = row.value(QStringLiteral("busid")).toString();
            if (busid.isEmpty()) {
                continue;
            }
            const bool want = busids.contains(busid);
            const bool shared = row.value(QStringLiteral("shared")).toBool();
            if (want && !shared) {
                tokens << (QStringLiteral("+") + busid);
            }
            else if (!want && shared) {
                tokens << (QStringLiteral("-") + busid);
            }
        }

        if (!tokens.isEmpty()) {
            QLocalSocket service;
            // Qt maps this onto \\.\pipe\ArtMoonInputService on Windows.
            service.connectToServer(QStringLiteral("ArtMoonInputService"));
            if (service.waitForConnected(2000)) {
                service.write(QStringLiteral("reconcile %1\n")
                                  .arg(tokens.join(QLatin1Char(' '))).toUtf8());
                service.waitForBytesWritten(2000);
                // The reply is read but not trusted, and not shown: `usbipd list` is the
                // authority on what is actually shared, and that is what the next refresh()
                // reads. It is also why a device stuck busy still reads "not shared yet"
                // rather than done.
                service.waitForReadyRead(3000);
                service.disconnectFromServer();
            }
        }
#else
        QProcess proc;
        QStringList helperArgs{ QStringLiteral("reconcile") };
        for (const auto &busid : busids) {
            helperArgs << QStringLiteral("--want") << busid;
        }
        for (const auto &entry : m_Devices) {
            helperArgs << QStringLiteral("--local")
                       << entry.toMap().value(QStringLiteral("busid")).toString();
        }
        proc.start(QLatin1String(kHelperPathLinux), helperArgs);
        proc.waitForFinished(4000);
#endif
    }

    rebuild();
}

void UsbIpDevices::installInputService()
{
    if (!m_CanInstallService || m_InstallingService) {
        return;
    }

    m_InstallingService = true;
    emit installingServiceChanged();

    QString note;

#ifdef Q_OS_WIN32
    // Nothing to do on Windows: its service is installed by ArtMoon's own installer, the same
    // shape as the one usbipd-win installs for itself. Kept as a clean no-op rather than an
    // #ifdef at the call site, so the QML has one thing to call on both platforms.
    note = tr("The input service is installed with ArtMoon on Windows.");
#else
    /*
     * install(1) — not a shell command, and not a script carried in the bundle. That is a
     * security choice, not a style one. Whatever we hand to pkexec runs as root, so it should
     * be a system binary that takes two paths and does exactly one thing, and whoever approves
     * the prompt can see precisely what is being authorised. Approving a script that lives in
     * a user-writable mount would be authorising that script's PATH — and anything able to
     * write to that path could then have a program of its own choosing run as root.
     *
     * The payload is staged first, and that is a requirement rather than a preference. An
     * AppImage runs from a FUSE mount, and the runtime mounts it WITHOUT allow_other or
     * allow_root, so only the mounting user may traverse it — root is refused too, because
     * CAP_DAC_OVERRIDE does not bypass the FUSE mount-owner check. Handing pkexec the in-mount
     * path fails with "cannot stat: Permission denied" (Niks-z13 2026-10-03, options
     * ro,nosuid,nodev,relatime,user_id=1000,group_id=1000). So the copy happens here first,
     * into a directory only this user can reach: 0700 under XDG_RUNTIME_DIR, else a 0700 temp
     * dir. No other principal can redirect what root then reads; the user alone can, and the
     * user is the one approving the prompt. Handing the payload to pkexec over stdin instead
     * of as a path would harden that last step, but it depends on pkexec carrying stdin
     * through, which is not established — so it is not what ships.
     *
     * install applies mode and owner itself, so the helper never exists at its system path
     * with the wrong ownership, not even briefly.
     */
    QTemporaryDir staging{
        qEnvironmentVariable("XDG_RUNTIME_DIR").isEmpty()
            ? QStringLiteral("/tmp/artmoon-install-XXXXXX")
            : qEnvironmentVariable("XDG_RUNTIME_DIR") + QStringLiteral("/artmoon-install-XXXXXX")};

    const QString staged = staging.filePath(QStringLiteral("artmoon-input-service"));
    const QString bundled = bundledHelperPath();

    bool stagedOk = false;
    if (staging.isValid()) {
        QFile in(bundled);
        QFile out(staged);
        if (in.open(QIODevice::ReadOnly) && out.open(QIODevice::WriteOnly)) {
            const qint64 written = out.write(in.readAll());
            out.close();
            QFile::setPermissions(staged, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            stagedOk = written > 0 && QFileInfo(staged).size() == QFileInfo(bundled).size();
        }
    }

    if (!stagedOk) {
        note = tr("Not set up — the input service could not be prepared from this build.");
    }
    else {
        QProcess proc;
        proc.start(QStringLiteral("pkexec"),
                   { QStringLiteral("install"),
                     QStringLiteral("-m"), QStringLiteral("0755"),
                     QStringLiteral("-o"), QStringLiteral("root"),
                     QStringLiteral("-g"), QStringLiteral("root"),
                     staged, QLatin1String(kHelperPathLinux) });
        proc.waitForFinished(120000);

        const int code = proc.exitCode();
        if (code == 126 || code == 127) {
            // pkexec's own meanings: the prompt was dismissed, or this user may not authenticate.
            note = tr("Not set up — the password prompt was closed.");
        }
        else if (code != 0) {
            QString err = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
            note = err.isEmpty()
                ? tr("Not set up — the install did not complete.")
                : err.split(QLatin1Char('\n')).first().trimmed();
        }
        else {
            // Decide from the artifact, never from the exit code. A copy that read nothing
            // still leaves a file at the destination, and it is presence that turns the
            // toggles live — so a helper that cannot bind anything would look installed.
            const QString want = fileSha256(bundled);
            const QFileInfo installed{QLatin1String(kHelperPathLinux)};
            if (!installed.exists()) {
                note = tr("Not set up — the install reported success but nothing landed.");
            }
            else if (!want.isEmpty() && fileSha256(QLatin1String(kHelperPathLinux)) != want) {
                note = tr("Not set up — the installed service does not match this build.");
            }
            else if (installed.ownerId() != 0) {
                note = tr("Not set up — the installed service is not owned by root.");
            }
        }
    }
#endif

    m_InstallingService = false;
    emit installingServiceChanged();

    // Re-read the world rather than trust the exit code. canShare is derived from the helper
    // actually being present, and that is the only thing that turns the toggles live.
    refresh();

    // refresh() rebuilds the reason from scratch, so a failure has to be said afterwards or it
    // is lost — and leaving someone staring at an unchanged screen after a prompt they refused
    // is exactly the silent-nothing case the rest of this section exists to avoid.
    if (!m_CanShare && !note.isEmpty()) {
        m_CanShareReason = note;
        emit devicesChanged();
    }
}
