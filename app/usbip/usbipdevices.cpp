#include "usbipdevices.h"
#include "usbipdevicelist.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QQmlEngine>
#include <QSettings>
#include <QStandardPaths>
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
    QSettings services(QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services"),
                       QSettings::NativeFormat);
    m_CanShare = services.childGroups().contains(QLatin1String(kServiceNameWindows));
#else
    m_CanShare = QFileInfo::exists(QLatin1String(kHelperPathLinux));
#endif
    m_CanShareReason = m_CanShare
        ? QString()
        : tr("Sharing a device needs administrator rights, which the ArtMoon input service "
             "does on your behalf. It is not installed in this build yet.");

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
    if (m_CanShare) {
        QProcess proc;
#ifdef Q_OS_WIN32
        proc.start(QStringLiteral("sc"),
                   { QStringLiteral("start"), QLatin1String(kServiceNameWindows),
                     QStringLiteral("reconcile") });
        proc.waitForFinished(4000);
#else
        proc.start(QLatin1String(kHelperPathLinux), { QStringLiteral("reconcile") });
        proc.waitForFinished(4000);
#endif
    }

    rebuild();
}
