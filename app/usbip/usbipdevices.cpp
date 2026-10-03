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

/*
 * The polkit action that lets the app reach the helper with privilege and WITHOUT a password
 * prompt on every toggle. Placing it is what the user actually approves, so the grant and the
 * prompt are the same moment — which is why the two files are installed together and not in
 * two separate offers.
 *
 * It is part of what "can share" means, not a nicety. With the helper in place and the action
 * missing, pkexec has nothing to grant and falls back to asking for a password on every single
 * toggle: the feature would be technically working and awful. Requiring it is what lets the
 * app say "not set up" and offer the one-step fix instead.
 */
const char *kPolicyPathLinux    = "/usr/share/polkit-1/actions/org.artmoon.input-service.policy";

/*
 * The bounded second look after a toggle. See scheduleSettle().
 *
 * A bind is not finished when pkexec exits, so the re-read that follows a toggle can still
 * catch the machine mid-change. Short on purpose: this covers the kernel registering the
 * device a heartbeat after the helper returns, not a slow operation.
 */
constexpr int kSettleIntervalMs = 1200;
constexpr int kSettleTries      = 6;

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
    m_SettleTimer.setSingleShot(true);
    connect(&m_SettleTimer, &QTimer::timeout, this, &UsbIpDevices::settle);
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

/*
 * Where the polkit action sits in this build — the same relationship as the helper above, for
 * the same reason: the bundled tree mirrors the layout the installed tree has, so an installed
 * ArtMoon resolves this to the real system path and the app can tell "not set up" from "set up"
 * by looking at the filesystem instead of by remembering.
 */
QString UsbIpDevices::bundledPolicyPath()
{
#ifdef Q_OS_WIN32
    return QString();
#else
    const QDir appDir(QCoreApplication::applicationDirPath());
    return appDir.absoluteFilePath(QStringLiteral(
        "../share/polkit-1/actions/org.artmoon.input-service.policy"));
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
        // The engine ships inside ArtMoon's own installer, so the remedy is that installer
        // rather than a trip to the internet — which is what lets this sentence name it. Both
        // branches used to carry the identical string inside a Windows-only #ifdef, which said
        // nothing at all.
        m_UnavailableReason = tr("USB/IP is not installed on this PC. Re-running the ArtMoon "
             "installer with \"Device sharing\" ticked will add it.");
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
    // Deliberately single-escaped. Qt strips "HKEY_LOCAL_MACHINE" plus ONE backslash and
    // hands the remainder straight to RegOpenKeyExW, so a doubled backslash leaves a leading
    // backslash and the key never opens (ERROR_BAD_PATHNAME, 161). childGroups() then comes
    // back empty and the toggles stay dead on a machine where the service is perfectly fine.
    // Written the same way as every other registry read in this product.
    QSettings services(QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services"),
                       QSettings::NativeFormat);
    m_CanShare = services.childGroups().contains(QLatin1String(kServiceNameWindows));
#else
    /*
     * Three things, all read off the filesystem, all required:
     *
     *   - the helper exists, so there is something to run;
     *   - it is root-owned, because one that is not cannot bind anything. Counting it as
     *     present would light the toggles against something that refuses every request, and
     *     after a privileged install ownership is also the property that says the install
     *     really happened rather than leaving a file behind;
     *   - the polkit action exists, because without it pkexec has nothing to grant and would
     *     ask for a password on every toggle.
     */
    const QFileInfo systemHelper{QLatin1String(kHelperPathLinux)};
    m_CanShare = systemHelper.exists() && systemHelper.ownerId() == 0
                 && QFileInfo::exists(QLatin1String(kPolicyPathLinux));
#endif

    // ── Is there something we could do about it? ──────────────────────────────
    // Only worth offering when a helper is actually here to place and it is not in place
    // already. Derived from the files on disk, never from a constant someone has to remember
    // to update — the same rule canShare itself is held to.
    m_CanInstallService = false;
#ifndef Q_OS_WIN32
    // Both files have to be carried to be worth offering, and either one being absent from the
    // system is enough to make the offer correct. Asking "is anything missing" rather than "is
    // the helper missing" is what makes an install from an older build repairable by the same
    // button — and that is a real case, not a hypothetical: the permission rule is new.
    m_CanInstallService = !m_CanShare
                          && QFileInfo::exists(bundledHelperPath())
                          && QFileInfo::exists(bundledPolicyPath());
#endif

    if (m_CanShare) {
        m_CanShareReason.clear();
    }
    else if (m_CanInstallService) {
        // Name the remedy rather than the absence. The two files can part company — an install
        // from a build that predates the permission rule, or a policy file removed by hand —
        // and a single generic sentence would hide which half is actually missing.
        m_CanShareReason = QFileInfo::exists(QLatin1String(kHelperPathLinux))
            ? tr("Sharing a device needs administrator rights. The ArtMoon input service is here, "
                 "but the permission rule that lets ArtMoon reach it is not — setting it up will "
                 "place the missing piece.")
            : tr("Sharing a device needs administrator rights. This build includes the ArtMoon "
                 "input service, which does that for you, but it is not set up on this PC yet.");
    }
    else {
#ifdef Q_OS_WIN32
        // Windows: the installer places this service, so "not in this build yet" would be a lie —
        // the build carries it, and has since the service landed. What is missing is the service
        // on THIS machine, and the remedy is the installer. The Linux sentence below describes a
        // build that genuinely lacks the bundled helper, and would send a Windows user hunting a
        // build flag that does not exist.
        m_CanShareReason = tr("Sharing a device needs administrator rights, which the ArtMoon input "
             "service does on your behalf. The service is not set up on this PC — re-running the "
             "ArtMoon installer will place it.");
#else
        m_CanShareReason = tr("Sharing a device needs administrator rights, which the ArtMoon input service "
             "does on your behalf. It is not installed in this build yet.");
#endif
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

    // A fresh attempt supersedes the last explanation: the user has just asked again, and
    // the answer may be different this time.
    if (!m_ToggleFailure.isEmpty()) {
        m_ToggleFailure.clear();
        emit devicesChanged();
    }

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
        // pkexec, not the helper directly. The helper refuses to act without administrator
        // rights, and this is how it gets them. It does not prompt: the polkit action placed by
        // the one-time setup grants the active session exactly this one program, which is what
        // that action exists for. canShare requires the action to be present, so the case where
        // pkexec would fall back to asking on every toggle cannot arise here.
        QStringList helperArgs{ QLatin1String(kHelperPathLinux), QStringLiteral("reconcile") };
        for (const auto &busid : busids) {
            helperArgs << QStringLiteral("--want") << busid;
        }
        for (const auto &entry : m_Devices) {
            helperArgs << QStringLiteral("--local")
                       << entry.toMap().value(QStringLiteral("busid")).toString();
        }
        proc.start(QStringLiteral("pkexec"), helperArgs);
        // The exit is checked, not ignored. A pkexec that refused — no policy, no session,
        // helper not where we think it is — used to leave exactly the same mark as a bind
        // that had merely not landed yet, so the row read "Not shared yet" for as long as
        // anyone cared to look. Silence on this side is how a request that never ran passes
        // for one still in flight.
        if (!proc.waitForFinished(4000)
            || proc.exitStatus() != QProcess::NormalExit
            || proc.exitCode() != 0) {
            m_ToggleFailure = tr("The input service refused the request for %1.").arg(busid);
        }
#endif
    }

    /*
     * Re-read, not re-mark.
     *
     * This used to be rebuild(), which only restated the request: it re-marked each row against
     * the remembered intent and left `shared` exactly as the last enumeration found it. So a
     * device the service had genuinely bound kept reading "Not shared yet" until the app was
     * restarted — the label describing what we asked for instead of what the machine did, on
     * the one screen whose whole job is telling those two apart. Verified on the z13: the bind
     * landed (kernel: `usbip-host 3-10: register new device`) while the row still said
     * "not shared yet".
     *
     * refresh() reads /sys and settles it. Everything this class reports is decided from an
     * artifact — the helper, the policy, the driver binding — and this is that same rule
     * applied to the aftermath of our own action.
     */
    refresh();
    scheduleSettle();
}

/*
 * A bind is not finished when pkexec exits.
 *
 * The helper returns, and the kernel registers the device with usbip-host a moment later. The
 * immediate refresh above usually catches it and cannot be relied on to. This is the small
 * bounded second look that covers the gap.
 *
 * Bounded deliberately, and it stops as soon as every wanted device matches reality either
 * way. A label that corrects itself within a second is just a label; a permanent poll is a
 * background cost nobody asked for and a thing to explain later.
 */
void UsbIpDevices::scheduleSettle()
{
    m_SettleTries = 0;
    m_SettleTimer.start(kSettleIntervalMs);
}

void UsbIpDevices::settle()
{
    refresh();

    if (!anyUnsettled()) {
        // The machine is doing what was asked of it. Nothing left to explain.
        if (!m_ToggleFailure.isEmpty()) {
            m_ToggleFailure.clear();
            emit devicesChanged();
        }
        return;
    }

    if (++m_SettleTries < kSettleTries) {
        m_SettleTimer.start(kSettleIntervalMs);
        return;
    }

    // Out of tries and the machine still disagrees. Say so, rather than leaving the row to
    // imply it forever. A message already set (the Linux refusal above) is more specific and
    // is left alone.
    if (m_ToggleFailure.isEmpty()) {
        m_ToggleFailure = describeToggleFailure();
    }
    emit devicesChanged();
}

/*
 * Why a toggle did not take, as far as we can actually tell.
 *
 * "Not shared yet" cannot distinguish a bind that is still landing from a request that
 * nothing ever received — and the second looks exactly like the first, indefinitely. That is
 * how a service which had been failing to create its pipe on every call since it started
 * read as a service with nothing to report.
 *
 * So ask the question that separates them. Presence is not reachability: the registry says
 * the service is installed, and this says whether it is listening. The same artifact rule
 * the rest of this class is held to.
 */
QString UsbIpDevices::describeToggleFailure() const
{
    const QString names = wantedBusids().join(QStringLiteral(", "));

#ifdef Q_OS_WIN32
    QLocalSocket probe;
    probe.connectToServer(QLatin1String(kServiceNameWindows));
    if (!probe.waitForConnected(1500)) {
        return tr("The ArtMoon input service is installed but is not answering, so %1 could "
                  "not be shared. Sharing a device needs that service running.").arg(names);
    }
    probe.disconnectFromServer();
#endif

    return tr("The input service was asked about %1 and the device did not change. It can be "
              "toggled again.").arg(names);
}

/* True while any device's label disagrees with the machine. */
bool UsbIpDevices::anyUnsettled() const
{
    const QStringList wanted = wantedBusids();

    for (const auto &entry : m_Devices) {
        const QVariantMap row = entry.toMap();
        const QString busid = row.value(QStringLiteral("busid")).toString();
        if (busid.isEmpty()) {
            continue;
        }
        if (wanted.contains(busid) != row.value(QStringLiteral("shared")).toBool()) {
            return true;
        }
    }

    return false;
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
     * install applies mode and owner itself, so neither file ever exists at its system path
     * with the wrong ownership, not even briefly.
     */
    QTemporaryDir staging{
        qEnvironmentVariable("XDG_RUNTIME_DIR").isEmpty()
            ? QStringLiteral("/tmp/artmoon-install-XXXXXX")
            : qEnvironmentVariable("XDG_RUNTIME_DIR") + QStringLiteral("/artmoon-install-XXXXXX")};

    /*
     * Two files go down, one approval each: the helper, and the polkit action that lets the app
     * reach it later without a password prompt on every toggle. They belong in the same step
     * because the action is the very thing being granted — installing the helper without it
     * would leave a feature that works by asking for a password every time, which is worse than
     * one that is obviously broken.
     */
    struct Placement {
        QString bundled;
        QString stagedName;
        QString destination;
        QString mode;
        QString label;
    };

    const Placement placements[] = {
        { bundledHelperPath(), QStringLiteral("artmoon-input-service"),
          QLatin1String(kHelperPathLinux), QStringLiteral("0755"), tr("the input service") },
        { bundledPolicyPath(), QStringLiteral("org.artmoon.input-service.policy"),
          QLatin1String(kPolicyPathLinux), QStringLiteral("0644"), tr("the permission rule") },
    };

    for (const Placement &placement : placements) {
        const QString staged = staging.isValid()
            ? staging.filePath(placement.stagedName)
            : QString();

        bool stagedOk = false;
        if (!staged.isEmpty()) {
            QFile in(placement.bundled);
            QFile out(staged);
            if (in.open(QIODevice::ReadOnly) && out.open(QIODevice::WriteOnly)) {
                const qint64 written = out.write(in.readAll());
                out.close();
                QFile::setPermissions(staged, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                stagedOk = written > 0
                           && QFileInfo(staged).size() == QFileInfo(placement.bundled).size();
            }
        }

        if (!stagedOk) {
            note = tr("Not set up — %1 could not be prepared from this build.").arg(placement.label);
            break;
        }

        QProcess proc;
        proc.start(QStringLiteral("pkexec"),
                   { QStringLiteral("install"),
                     QStringLiteral("-m"), placement.mode,
                     QStringLiteral("-o"), QStringLiteral("root"),
                     QStringLiteral("-g"), QStringLiteral("root"),
                     staged, placement.destination });
        proc.waitForFinished(120000);

        const int code = proc.exitCode();
        if (code == 126 || code == 127) {
            // pkexec's own meanings: the prompt was dismissed, or this user may not authenticate.
            note = tr("Not set up — the password prompt was closed.");
            break;
        }
        if (code != 0) {
            QString err = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
            note = err.isEmpty()
                ? tr("Not set up — %1 could not be installed.").arg(placement.label)
                : err.split(QLatin1Char('\n')).first().trimmed();
            break;
        }

        // Decide from the artifact, never from the exit code. A copy that read nothing still
        // leaves a file at the destination, and presence is what turns the toggles live — so a
        // helper that cannot bind anything would look installed.
        const QFileInfo installed{placement.destination};
        const QString want = fileSha256(placement.bundled);
        if (!installed.exists()) {
            note = tr("Not set up — %1 reported success but nothing landed.").arg(placement.label);
            break;
        }
        if (!want.isEmpty() && fileSha256(placement.destination) != want) {
            note = tr("Not set up — the installed %1 does not match this build.").arg(placement.label);
            break;
        }
        if (installed.ownerId() != 0) {
            note = tr("Not set up — the installed %1 is not owned by root.").arg(placement.label);
            break;
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
