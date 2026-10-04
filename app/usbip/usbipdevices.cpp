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
 * The usbip client ArtMoon ships for Linux, and the library it links against.
 *
 * They get a directory of their own rather than sitting in /usr/libexec beside the helper,
 * because there are two of them and one of them is a library: libusbip.so.0 parked directly
 * in libexec would be a globally-named shared object in a place nothing expects one.
 *
 * Nothing here is invented — these are the paths the helper looks in, in
 * service/artmoon-input-service.cpp. The two must agree, which is why the AppImage build
 * cross-checks the helper's bundled paths the way it already cross-checks the polkit action's.
 */
const char *kUsbipDirLinux      = "/usr/libexec/artmoon-usbip";
const char *kUsbipBinLinux      = "/usr/libexec/artmoon-usbip/usbip";
const char *kUsbipLibLinux      = "/usr/libexec/artmoon-usbip/libusbip.so.0";
// The daemon, beside the client. `usbip bind` only makes a device offerable; a listener has to
// be on 3240 before another machine can attach anything. Windows gets that listener from the
// usbipd-win installer, Linux gets nothing - the distro package may not be installed at all and
// its unit ships disabled even when it is - so we carry the daemon and place it ourselves.
const char *kUsbipdBinLinux     = "/usr/libexec/artmoon-usbip/usbipd";

/*
 * The bounded second look after a toggle. See scheduleSettle().
 *
 * A bind is not finished when pkexec exits, so the re-read that follows a toggle can still
 * catch the machine mid-change. Short on purpose: this covers the kernel registering the
 * device a heartbeat after the helper returns, not a slow operation.
 */
constexpr int kSettleIntervalMs = 1200;

/*
 * How often to look for a machine streaming here, while anything is switched on.
 *
 * Five seconds is a guess with a reason: the rule has to be open before the far machine's
 * attach, and an attach that is refused is retried by the user, not by us. Long enough that
 * the check is invisible, short enough that it has almost always already happened by the time
 * anyone could notice.
 */
constexpr int kPeerIntervalMs = 5000;

/*
 * The same look, at a shorter interval, for the one state where waiting is visible.
 *
 * A tick binds nothing now — the bind happens when a stream appears — so there is a stretch
 * where something is wanted and nothing is held, and in that stretch the other machine is
 * about to ask this one for a device that does not yet exist as an export. Making that window
 * one second instead of five is not about tidiness: it is the difference between the far end
 * finding the device on the attempt it makes and being refused.
 */
constexpr int kPeerWaitIntervalMs = 1000;

/*
 * How many times to try opening for one machine before leaving it alone.
 *
 * The call goes through pkexec, and polkit grants this one only to the local ACTIVE session —
 * deliberately, so a session nobody is sitting at cannot use it. An exporter that is streaming
 * with its desktop unlocked is active and needs no dialog; one whose screen is locked is not,
 * and the call is refused. Three tries covers a session that was momentarily not the active
 * one. Past that it waits for the machine to change rather than asking again every five
 * seconds, because a password dialog appearing on its own, repeatedly, is worse than a
 * connection that did not open.
 */
constexpr int kPeerAttempts = 3;

/*
 * How long a peer the helper has named is still believed to be there, after a look that does not
 * name one.
 *
 * The helper is asked every second or two while a device is switched on, and the connection it
 * names comes and goes on a shorter cycle than that: ArtMoon opens and closes its look at the
 * host every couple of seconds. Believing each look on its own made everything downstream flap on
 * that cycle — the firewall rule was written and taken away again, the bind was handed back and
 * redone, and the word in the list went Shared / Not shared yet / Shared while the person watched
 * it. The connection to the far end is the same connection either way, so the only thing the flap
 * was buying was churn.
 *
 * Fifteen seconds bridges the look's own cycle several times over, and is short enough that a peer
 * which really has gone — the other machine closed, or stopped streaming — is let go promptly.
 */
constexpr int kPeerGraceMs = 15000;
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
    // The same order the helper uses when it binds, and deliberately so: the list this
    // produces and the bind that follows it should never come from two different tools.
    // The host's first, because a distro's usbip is built with the kernel that distro ships;
    // then the copy ArtMoon placed; then PATH, for a distro that keeps it somewhere else.
    //
    // That middle one is the whole point of shipping it. Before it, a machine with no usbip
    // read as "USB/IP is not installed on this PC" and nothing below could enumerate — the
    // toggle was there, the list was empty, and there was nothing a user could do about it.
    const QString host = QStringLiteral("/usr/bin/usbip");
    if (QFileInfo(host).isExecutable()) {
        return host;
    }
    if (QFileInfo(QLatin1String(kUsbipBinLinux)).isExecutable()) {
        return QLatin1String(kUsbipBinLinux);
    }
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
    // Repeating, unlike the settle timer. This one watches for something another machine does,
    // and it stops the moment nothing is switched on. See watchForPeer().
    m_PeerTimer.setInterval(kPeerIntervalMs);
    connect(&m_PeerTimer, &QTimer::timeout, this, &UsbIpDevices::refresh);

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

namespace {
QByteArray fileDigest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return QByteArray();
    }
    return hash.result();
}
}

/*
 * Is the helper on this PC the one THIS build carries?
 *
 * Existing is not the same as current, and until now the app only ever asked the first question.
 * Every property m_CanShare checked — present, root-owned, policy alongside it — an install from
 * an older build satisfies perfectly. So a fix could ship, the AppImage could be updated, setup
 * could report success, and the machine would keep running the old binary for good.
 *
 * Found the hard way on the z13: a new build carrying a corrected helper was staged, the helper
 * was installed by hand from it, and the app went on reporting everything fine — because the
 * file it would have placed and the file already there had the same name. The next build would
 * have repeated it exactly, which is the only reason this is being fixed rather than worked
 * around with another hand-copy.
 *
 * Compared by content. Not by timestamp — a file copied out of an archive carries whatever time
 * the archive gave it, so mtime says nothing about which build it came from — and not by size,
 * which is not a claim about behaviour.
 *
 * The answer is cached because refresh() runs on a timer and hashing a few megabytes every
 * second to reach an answer that cannot change on its own would be work for nothing. The one
 * thing that CAN change it is our own install, which clears the cache.
 */
bool UsbIpDevices::helperMatchesBundle()
{
    if (m_HelperMatchKnown) {
        return m_HelperMatches;
    }
    m_HelperMatchKnown = true;
    m_HelperMatches = false;
#ifndef Q_OS_WIN32
    const QString bundled = bundledHelperPath();
    if (!QFileInfo::exists(bundled)) {
        // Nothing carried to compare against. That is the other condition entirely, and
        // m_CanInstallService reports it; answering here would only say it twice.
        return false;
    }
    const QByteArray installed = fileDigest(QLatin1String(kHelperPathLinux));
    m_HelperMatches = !installed.isEmpty() && installed == fileDigest(bundled);
#endif
    return m_HelperMatches;
}

/*
 * Where the polkit action sits in this build — the same relationship as the helper above, for
 * the same reason: the bundled tree mirrors the layout the installed tree has, so an installed
 * ArtMoon resolves this to the real system path and the app can tell "not set up" from "set up"
 * by looking at the filesystem instead of by remembering.
 */
/*
 * Where the shipped usbip sits in this build — the same relative relationship as the helper
 * and the action above, for the same reason. usr/libexec/artmoon-usbip in the image becomes
 * /usr/libexec/artmoon-usbip on the system, so placing it is a copy and not a remapping.
 */
QString UsbIpDevices::bundledUsbipPath()
{
#ifdef Q_OS_WIN32
    return QString();
#else
    const QDir appDir(QCoreApplication::applicationDirPath());
    return appDir.absoluteFilePath(QStringLiteral("../libexec/artmoon-usbip/usbip"));
#endif
}

QString UsbIpDevices::bundledUsbipLibPath()
{
#ifdef Q_OS_WIN32
    return QString();
#else
    const QDir appDir(QCoreApplication::applicationDirPath());
    return appDir.absoluteFilePath(QStringLiteral("../libexec/artmoon-usbip/libusbip.so.0"));
#endif
}

/*
 * Where the shipped daemon sits in this build - beside the client, same relative relationship
 * again. The helper starts it from the staged path, so the AppImage and the installed tree have
 * to agree about where that is, exactly as they do for usbip itself.
 */
QString UsbIpDevices::bundledUsbipdPath()
{
#ifdef Q_OS_WIN32
    return QString();
#else
    const QDir appDir(QCoreApplication::applicationDirPath());
    return appDir.absoluteFilePath(QStringLiteral("../libexec/artmoon-usbip/usbipd"));
#endif
}

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
        m_UnavailableReason = tr("Device sharing is not installed on this PC. Running the "
             "ArtMoon installer again, with \"Device sharing\" ticked, will add it.");
#else
        m_UnavailableReason = tr("The device sharing tools are missing from this copy of "
             "ArtMoon, so this PC cannot share a device.");
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
                ? tr("The device sharing tools on this PC did not answer.")
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
                 && QFileInfo::exists(QLatin1String(kPolicyPathLinux))
                 && helperMatchesBundle();
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
        // Name the remedy rather than the absence. The files can part company — an install from
        // a build that predates the permission rule, a policy file removed by hand, or a helper
        // left behind by an older build — and a single generic sentence would hide which half is
        // actually wrong. "Not set up yet" is also a plain lie in the third case: it IS set up,
        // and it works, and it is not the one this build carries.
        m_CanShareReason = QFileInfo::exists(QLatin1String(kHelperPathLinux))
            ? (helperMatchesBundle()
               ? tr("Sharing a device needs administrator rights, which ArtMoon's sharing helper "
                    "does for you. The helper is on this PC but the rule that lets ArtMoon use it "
                    "is not — running setup from this screen will finish it.")
               : tr("Sharing a device needs administrator rights, which ArtMoon's sharing helper "
                    "does for you. The helper on this PC is from another version of ArtMoon, and "
                    "running setup from this screen will bring it up to date."))
            : tr("Sharing a device needs administrator rights, which ArtMoon's sharing helper "
                 "does for you. It is not set up on this PC yet — running setup from this screen "
                 "will do it.");
    }
    else {
#ifdef Q_OS_WIN32
        // Windows: the installer places this service, so "not in this build yet" would be a lie —
        // the build carries it, and has since the service landed. What is missing is the service
        // on THIS machine, and the remedy is the installer. The Linux sentence below describes a
        // build that genuinely lacks the bundled helper, and would send a Windows user hunting a
        // build flag that does not exist.
        m_CanShareReason = tr("Sharing a device needs administrator rights, which ArtMoon's "
             "sharing helper does for you. It is not set up on this PC — running the ArtMoon "
             "installer again will add it.");
#else
        m_CanShareReason = tr("Sharing a device needs administrator rights, which ArtMoon's "
             "sharing helper does for you. This version of ArtMoon does not include it.");
#endif
    }

    /*
     * ── Can another machine actually reach this one? ──────────────────────────
     *
     * The failure this whole feature is prone to, and the one nothing on this screen could
     * describe before: the toggles are live, the device says it is shared, and the other
     * machine still cannot see it. That is what happens when nothing is listening on the
     * USB/IP port, or when the port is shut from the outside — and from the far end those two
     * look identical, so the only way to tell them apart was to go and test from there.
     *
     * The helper answers it, unprivileged, out of /proc. Its words are used rather than a
     * second opinion invented here: it is the same code that decides whether to open the
     * firewall, so the answer and the action cannot drift apart.
     */
    m_ReachabilityReason.clear();
#ifndef Q_OS_WIN32
    bool sawPeer = false;
    if (m_CanShare) {
        QProcess helper;
        helper.start(QLatin1String(kHelperPathLinux), { QStringLiteral("status") });
        if (helper.waitForFinished(2000) && helper.exitCode() == 0) {
            const QString output = QString::fromLocal8Bit(helper.readAllStandardOutput());
            const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString &line : lines) {
                // The reachable case is the one that matters to the machine, not the screen:
                // it is the moment this machine can name the one that may start streaming at us,
                // and therefore the moment the firewall rule may be opened and the bind becomes
                // worth doing. Read it here, act on it in watchForPeer(). Note this is NOT the
                // moment a session exists — it arrives before one, which is the whole point.
                if (line.startsWith(QLatin1String("reachable:"))) {
                    m_ReachablePeer = line.mid(10).trimmed();
                    m_PeerSeen.restart();
                    sawPeer = true;
                    continue;
                }
                if (!line.startsWith(QLatin1String("unreachable:"))) {
                    continue;
                }
                const QString code = line.mid(12).trimmed();
                if (code == QLatin1String("no-listener")) {
                    // Two entirely different situations print this one code, and telling them
                    // apart is the whole reason this block exists.
                    //
                    // Nothing wanted: nothing is being offered, so nothing is listening — and that
                    // is the correct state of a machine that has never shared anything. Calling it
                    // "not accepting device connections" describes a fault where there is none,
                    // and sending the user to restart ArtMoon sends them to do something that
                    // cannot possibly help: the listener is not started by a restart, it is
                    // started by sharing a device. Measured on the exporter 2026-10-04, minutes
                    // after a clean first run, with nothing wanted.
                    //
                    // Something wanted: the device says it is shared and nothing is listening,
                    // which IS the fault — and there the restart is a real remedy, because
                    // reconcile runs again on startup with the wanted list and that is what brings
                    // the listener up.
                    if (wantedBusids().isEmpty()) {
                        m_ReachabilityReason = tr("Nothing is switched on below, so there is "
                             "nothing for your other machines to use yet. Switch a device on and "
                             "ArtMoon will make it available.");
                    } else {
                        // Not a fault any more, and no longer says to restart ArtMoon — that
                        // remedy belonged to a build where ticking a device bound it, so a
                        // listener was expected to be up the moment anything was switched on.
                        // Ticking binds nothing now; the bind waits for the stream. This is the
                        // ordinary state of a machine that is ready and not yet in use, and it
                        // is the state the person is in while they are deciding what to stream.
                        m_ReachabilityReason = tr("Ready. Nothing has been taken from this PC "
                             "yet — the devices switched on below go to your other machine when "
                             "a stream starts, and come back when it stops.");
                    }
                } else if (code == QLatin1String("no-client")) {
                    // The second toggle used to live in this sentence. It was true when it was
                    // written and it is not any more: watchForPeer() opens the connection the
                    // moment a machine starts streaming here, so the user's only job is to start
                    // that stream. A message that asks for work we now do ourselves is worse than
                    // no message — it teaches a step that has stopped existing.
                    m_ReachabilityReason = tr("Everything switched on below is ready. Start a "
                         "stream and it will be able to use them — ArtMoon opens the connection "
                         "by itself when you do.");
                } else {
                    // A code this build does not know. Show it rather than swallow it — an
                    // unexplained silence is what this whole block exists to remove.
                    m_ReachabilityReason = code;
                }
                break;
            }
        }
    }

    // The falling edge, with the grace applied. See kPeerGraceMs: a peer is only gone once it has
    // been gone for a while, not the first time a look happens to miss it.
    if (!m_CanShare) {
        m_ReachablePeer.clear();
        m_PeerSeen.invalidate();
    } else if (!sawPeer && (!m_PeerSeen.isValid() || m_PeerSeen.hasExpired(kPeerGraceMs))) {
        m_ReachablePeer.clear();
    }
#else
    m_ReachablePeer.clear();
#endif

#ifdef Q_OS_WIN32
    // Windows had nothing here at all. The whole block above is Linux-only, so a machine whose
    // service had stopped showed no notice, no reason, and toggles that looked perfectly fine —
    // the failure this feature is most prone to, silent on the platform it was built for first.
    //
    // Presence is not reachability on either platform: the registry says the service is
    // installed, and only its pipe says it is answering. Same probe describeToggleFailure()
    // uses, asked at the same time and for the same reason.
    if (m_CanShare && !wantedBusids().isEmpty() && m_ReachabilityReason.isEmpty()) {
        QLocalSocket probe;
        probe.connectToServer(QLatin1String(kServiceNameWindows));
        if (!probe.waitForConnected(1200)) {
            m_ReachabilityReason = tr("Sharing is switched on, but ArtMoon's sharing helper is "
                 "not running, so nothing switched on below can be used. Restart ArtMoon to "
                 "start it again.");
        }
        else {
            probe.disconnectFromServer();
        }
    }
#endif

    watchForPeer();

    // Keep looking while anything is switched on, and stop the moment nothing is. The state
    // this is watching for is created by the other machine, not by us, so the only way to
    // know about it is to ask — and asking forever when nothing is shared would be a
    // background cost for no answer.
    if (m_CanShare && !wantedBusids().isEmpty()) {
        /*
         * Two speeds, because the interval is felt in exactly one of the two states.
         *
         * Waiting to bind is the state a tick alone puts us in — something wanted, nothing taken
         * yet — and it is the state where a slow poll costs something real: the other machine
         * attaches its devices when the stream starts, and it is OUR bind that makes the export
         * exist for it to find. At five seconds there is a window where the PC reaches for a
         * device that is still local here and is refused, which reads at the far end as this
         * machine not sharing anything. Look every second while that is the state.
         *
         * Once something is bound the peer is either there or it is not, and five seconds is
         * plenty — the poll is only being watched for the END of a session by then.
         */
        bool anyShared = false;
        for (const auto &entry : m_Devices) {
            if (entry.toMap().value(QStringLiteral("shared")).toBool()) {
                anyShared = true;
                break;
            }
        }
        const int interval = anyShared ? kPeerIntervalMs : kPeerWaitIntervalMs;
        if (m_PeerTimer.interval() != interval) {
            m_PeerTimer.setInterval(interval);
        }
        if (!m_PeerTimer.isActive()) {
            m_PeerTimer.start(interval);
        }
    }
    else {
        m_PeerTimer.stop();
    }

    rebuild();
}

/*
 * Open the connection for the machine that is streaming here, once it exists.
 *
 * The firewall rule is scoped to one address, and the only address worth opening for is a
 * machine already in a session with us — so the rule cannot be written when the user toggles a
 * device on, because at that moment nobody is streaming. It has to be written when the session
 * appears instead. Nothing tells the app that this happened, so it looks.
 *
 * Deliberately does not refresh afterwards: this runs from inside refresh(), and a second pass
 * from here would be a loop. The next poll reads the result, which is also the honest order —
 * the rule is a request, and the answer is what /proc says afterwards.
 */
void UsbIpDevices::watchForPeer()
{
    const QStringList wanted = wantedBusids();

    // The session has ended, so hand the device back.
    //
    // Switching a device on is a loan for the length of a stream, not a permanent handover.
    // When the machine we were streaming with goes, nothing is using the export any more — and
    // an export left bound is a device the person at this machine can no longer use, which is
    // the thing they notice, and rightly call a bug. Reconciling with nothing wanted makes the
    // helper unbind what it bound and take its firewall rule away with it.
    //
    // The tick itself is untouched: that is the standing choice, and the next session binds it
    // again. So this fires on the fall from a peer to no peer, once — which is why the state is
    // cleared here rather than left to the early return below.
    if (!m_LastPeer.isEmpty() && m_ReachablePeer.isEmpty()) {
        m_LastPeer.clear();
        m_OpenedForPeer.clear();
        m_PeerAttempts = 0;
        if (m_CanShare && !wanted.isEmpty()) {
            reconcileWithService(QStringList(), false);
        }
        return;
    }

    if (!m_CanShare || wanted.isEmpty() || m_ReachablePeer.isEmpty()) {
        m_OpenedForPeer.clear();
        m_LastPeer.clear();
        m_PeerAttempts = 0;
        return;
    }

    // A different machine — or the same one arriving again after a gap — starts over.
    if (m_ReachablePeer != m_LastPeer) {
        m_LastPeer = m_ReachablePeer;
        m_OpenedForPeer.clear();
        m_PeerAttempts = 0;
    }

    // Already open for this one. Re-running would re-derive the same rule on every poll.
    if (m_OpenedForPeer == m_ReachablePeer) {
        return;
    }

    if (m_PeerAttempts >= kPeerAttempts) {
        return;
    }

    if (reconcileWithService(wanted, false)) {
        m_OpenedForPeer = m_ReachablePeer;
    }
    else {
        ++m_PeerAttempts;
    }
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

    /*
     * Turning a device ON does not reconcile, and that is the whole point of the tick.
     *
     * `usbip bind` detaches a device from the local USB stack — there is no Linux state that
     * means "shareable but still mine". So reconciling here, which is what this did, took the
     * device out of the person's hand the instant they ticked it: a mouse stopped moving, for a
     * stream that had not started. Reproduced on the z13, 2026-10-04.
     *
     * On Windows `usbipd bind` only marks a device shareable and it stays usable until a client
     * attaches, so ticking there costs nothing. Linux cannot copy that state, so it copies the
     * only thing that matters: WHEN the device moves. Nothing is bound until a stream actually
     * starts, which watchForPeer() detects and acts on. Bind at the session, not at the tick.
     *
     * The intent is still recorded here, and refresh() below starts the peer watch that will
     * notice the stream — which is why the peer watch is driven by `wanted` and not by `shared`.
     *
     * Turning a device OFF is the opposite case and does reconcile: see below.
     */
    if (wanted) {
        refresh();
        scheduleSettle();
        return;
    }

    /*
     * Switching something off IS an action, because a device lent out mid-stream has to come
     * back now rather than at the end of a stream the person has just cancelled.
     *
     * Bind nothing new — hand back the one switched off, and keep only what the machine is
     * already exporting and still wants. Re-deriving the binds from `busids` would re-take every
     * other ticked device that is not currently lent, which is the bug above in a different
     * shape; the `shared` list is what the machine is actually holding.
     */
    QStringList keep;
    for (const auto &entry : m_Devices) {
        const QVariantMap row = entry.toMap();
        const QString other = row.value(QStringLiteral("busid")).toString();
        if (other != busid && row.value(QStringLiteral("shared")).toBool()
                && busids.contains(other)) {
            keep.append(other);
        }
    }
    reconcileWithService(keep, true);

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
 * Ask the service to reconcile, and let it say what it found.
 *
 * Lifted out of setWanted because it has a second caller now: the app also calls this when a
 * machine starts streaming here. See watchForPeer() — the firewall rule is opened for a
 * machine only once that machine is actually streaming, and that moment is not one the app is
 * told about, so it has to look for it. Before this the rule could only ever be opened by the
 * user toggling a second time, which is a thing to do to a person rather than a design.
 *
 * isUserToggle decides whether a refusal is reported as a failed toggle. A poll that could not
 * open the connection is already described by the reachability line on the same screen, and
 * saying it twice — once as a fault against a device the user never touched — is worse.
 */
bool UsbIpDevices::reconcileWithService(const QStringList &busids, bool isUserToggle)
{
    if (!m_CanShare) {
        return false;
    }

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

    bool ran = tokens.isEmpty();
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
            ran = true;
        }
    }
    return ran;
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
    const bool ran = proc.waitForFinished(4000)
                     && proc.exitStatus() == QProcess::NormalExit
                     && proc.exitCode() == 0;
    if (!ran && isUserToggle) {
        // The wanted list, not one busid: this method serves both the toggle and the peer
        // watch, and what was refused is the request, which may name more than one.
        m_ToggleFailure = tr("ArtMoon's sharing helper refused the request, so %1 could not "
                             "be switched on. Run setup from this screen, then try again.")
                              .arg(busids.join(QStringLiteral(", ")));
    }
    return ran;
#endif
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
        return tr("%1 could not be switched on because ArtMoon's sharing helper is not "
                  "responding. Restart ArtMoon to start it again.").arg(names);
    }
    probe.disconnectFromServer();
#endif

    return tr("%1 did not switch on. If something on this PC is using it, close that first, "
              "then try again.").arg(names);
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
     * The one privileged call runs our own helper, with a verb whose only job is placing these
     * files. The alternatives — a shell, or a system binary given a destination per invocation
     * — were rejected for the reason this comment has always given: whatever we hand to pkexec
     * runs as root, so it should be something whose behaviour can be stated in one sentence and
     * whose command line the approving person can read. `artmoon-input-service install --from
     * <dir>` is both. A script would be neither, and it would turn the prompt into an
     * authorisation of a PATH rather than of an act.
     *
     * It runs the *staged* copy, which lives in a directory this user owns, and that widens
     * nothing: what gets placed is a binary the permission rule then runs as root *silently*
     * for the rest of the machine's life. The user is approving their own payload either way.
     * Now they approve it once, and the thing that runs is the thing being placed.
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
     * The helper applies mode and owner itself, and writes beside each destination before
     * renaming over it, so no file ever exists at its system path with the wrong content,
     * mode or ownership — not even briefly.
     */
    QTemporaryDir staging{
        qEnvironmentVariable("XDG_RUNTIME_DIR").isEmpty()
            ? QStringLiteral("/tmp/artmoon-install-XXXXXX")
            : qEnvironmentVariable("XDG_RUNTIME_DIR") + QStringLiteral("/artmoon-install-XXXXXX")};

    /*
     * Five files go down, and where each of them lands is the helper's business now — it owns
     * the destination table. This side only says *what* it is handing over. That direction
     * matters: a destination named by the unprivileged caller is a destination an attacker gets
     * to name, on a program that runs as root.
     *
     * The action and the helper belong in the same step because the action is the very thing
     * being granted — installing the helper without it would leave a feature that works by
     * asking for a password every time, which is worse than one that is obviously broken.
     *
     * usbip is here for a plainer reason. The helper exports a device by running `usbip bind`,
     * and until now that tool was assumed to be on the host. On a machine that has never heard
     * of USB/IP it is not, and the feature looks broken rather than absent. The helper prefers
     * the host's own copy when there is one, so placing ours takes nothing away from a machine
     * that already had it.
     *
     * The daemon is here because a device that is bound is still invisible to the other machine
     * until something is listening on 3240 — and on Linux nothing is, for a user who has never
     * installed the distro's usbip package (whose unit also ships disabled even when it is
     * installed). Windows never had this problem: usbipd-win's installer places and starts a
     * service. This is that, for Linux, carried by us.
     */
    struct Placement {
        QString bundled;
        QString stagedName;
        QString destination;
        QString label;
    };

    const Placement placements[] = {
        { bundledHelperPath(), QStringLiteral("artmoon-input-service"),
          QLatin1String(kHelperPathLinux), tr("the input service") },
        { bundledPolicyPath(), QStringLiteral("org.artmoon.input-service.policy"),
          QLatin1String(kPolicyPathLinux), tr("the permission rule") },
        { bundledUsbipPath(), QStringLiteral("usbip"),
          QLatin1String(kUsbipBinLinux), tr("the USB/IP tool") },
        { bundledUsbipLibPath(), QStringLiteral("libusbip.so.0"),
          QLatin1String(kUsbipLibLinux), tr("its library") },
        { bundledUsbipdPath(), QStringLiteral("usbipd"),
          QLatin1String(kUsbipdBinLinux), tr("the sharing service") },
    };

    // Every file is prepared first, and only then is anything asked of root. Staging is
    // unprivileged (a copy into this user's own 0700 directory, because root cannot traverse an
    // AppImage's FUSE mount), so a failure here costs nothing and needs no prompt at all.
    bool prepared = true;

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
                /*
                 * Take the execute bit from the file being copied, never from a list of names.
                 *
                 * This used to set 0600 on every staged file, including the helper — which is the
                 * one file here that is going to be *run*. pkexec then authorised correctly, logged
                 * the command, and failed to exec the file it had just been handed, exiting 126:
                 * the same code it uses for a dismissed prompt. So the first run looked exactly
                 * like a closed password dialog, said so on screen, and set nothing up. Whatever is
                 * executable in the build is executable here, and this rule cannot drift from the
                 * payload the way a name list would.
                 */
                QFile::Permissions wanted = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
                if (QFile::permissions(placement.bundled) & QFileDevice::ExeOwner) {
                    wanted |= QFileDevice::ExeOwner;
                }
                QFile::setPermissions(staged, wanted);
                stagedOk = written > 0
                           && QFileInfo(staged).size() == QFileInfo(placement.bundled).size()
                           && (!(QFile::permissions(placement.bundled) & QFileDevice::ExeOwner)
                               || QFileInfo(staged).isExecutable());
            }
        }

        if (!stagedOk) {
            const bool wantedRunnable =
                QFile::permissions(placement.bundled) & QFileDevice::ExeOwner;
            note = (wantedRunnable && QFileInfo(staged).exists()
                    && !QFileInfo(staged).isExecutable())
                ? tr("Not set up — %1 could not be made ready to run. This is a fault in "
                     "ArtMoon, not something you did.").arg(placement.label)
                : tr("Not set up — %1 could not be copied out of this build. This is a fault in "
                     "ArtMoon, not something you did.").arg(placement.label);
            prepared = false;
            break;
        }
    }

    /*
     * ONE privileged call, for all five files.
     *
     * This used to be one `pkexec install` per file — five files, five authorisations, five
     * password dialogs, for one act. And every one of them was a generic exec authorisation,
     * because `install` is not the helper and so the action this setup is busy installing could
     * not cover it. Measured on the exporter 2026-10-04: exactly five dialogs, for a user who
     * had asked for nothing but a device to be shareable.
     *
     * So the single call runs the helper, staged here first for the reason above, and the
     * helper places all five. It already knows every destination; the app no longer names one,
     * which also takes a root-writable path out of the unprivileged side's hands. After this
     * the helper is silent for the rest of the machine's life, because the action granting that
     * is one of the files it just placed — so this is the first and last prompt.
     */
    if (prepared) {
        QProcess proc;
        proc.start(QStringLiteral("pkexec"),
                   { staging.filePath(QStringLiteral("artmoon-input-service")),
                     QStringLiteral("install"),
                     QStringLiteral("--from"), staging.path() });
        proc.waitForFinished(120000);

        const int code = proc.exitCode();
        if (code == 126 || code == 127) {
            // pkexec's own meanings: 126 covers a dismissed prompt *and* a program it could not
            // exec; 127 is a program it could not find.
            //
            // This sentence used to name only the dismissed prompt, and so told a user their
            // password was the problem when the failure was inside ArtMoon. Measured on the
            // exporter 2026-10-04: the helper was staged without its execute bit, pkexec
            // authorised correctly, could not exec the file, exited 126, and this is the line
            // that was shown. Both causes are named now, and the second one says whose it is.
            note = tr("Not set up — ArtMoon could not start the sharing components. If you "
                      "closed the password prompt, try again; if you did not, this is a fault in "
                      "ArtMoon.");
        } else if (code != 0) {
            // The helper's own stderr is a developer's sentence, not a user's: it is prefixed
            // with the program name and names paths. It used to be shown verbatim here. Say what
            // happened in words a person can act on, and leave the detail in the journal where
            // whoever reads it can see the whole line.
            note = tr("Not set up — the sharing components could not be installed. This is a "
                      "fault in ArtMoon, not something you did.");
        }
    }

    // Decide from the artifacts, never from the exit code, and check all of them the same way.
    // A copy that read nothing still leaves a file at the destination, and presence is what
    // turns the toggles live — so a helper that cannot bind anything would look installed.
    for (const Placement &placement : placements) {
        if (!note.isEmpty()) break;

        const QFileInfo installed{placement.destination};
        const QString want = fileSha256(placement.bundled);
        if (!installed.exists()) {
            note = tr("Not set up — %1 did not arrive. This is a fault in ArtMoon, not "
                      "something you did.").arg(placement.label);
            break;
        }
        if (!want.isEmpty() && fileSha256(placement.destination) != want) {
            note = tr("Not set up — %1 was installed, but it is not the one this ArtMoon "
                      "expected. This is a fault in ArtMoon.").arg(placement.label);
            break;
        }
        if (installed.ownerId() != 0) {
            note = tr("Not set up — %1 was installed but does not belong to the system. This "
                      "is a fault in ArtMoon.").arg(placement.label);
            break;
        }
    }
#endif

    m_InstallingService = false;
    emit installingServiceChanged();

    // The cache was answering a question we have just changed the answer to. Without this,
    // refresh() below would compare against the file that was there before the install and
    // report the helper as stale with the new one sitting right in front of it.
    m_HelperMatchKnown = false;
    m_HelperMatches = false;

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
