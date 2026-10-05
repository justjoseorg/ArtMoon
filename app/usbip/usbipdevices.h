#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

class QQmlEngine;

/**
 * USB/IP input passthrough — exporter side (the machine the devices are plugged into).
 *
 * Terminology is locked; read docs/usb-ip-input-passthrough.md before touching this.
 * This class never says host / client / server: in USB/IP the "host" is the DEVICE side,
 * and in streaming the host is the machine serving the VIDEO. On our two machines those
 * are opposite PCs, so the bare words invert the design. Use exporter / importer.
 *
 * This is the model behind the Input settings tab. It answers two questions and nothing
 * else:
 *   - which USB devices are plugged into THIS machine, and is each one already offered
 *     to an importer;
 *   - which of them has the user asked to offer.
 *
 * It does NOT bind. Binding is a privileged operation (verified 2026-10-03: `attach` runs
 * fine as a normal user, `bind` is refused), so it belongs to the input service that ships
 * in ArtMoon's own installer. This class asks that service to reconcile, and reports
 * honestly when the service is not there.
 */
class UsbIpDevices : public QObject
{
    Q_OBJECT

    /*
     * One map per device, for the Input tab's row repeater:
     *   busid        "9-1"        the stable handle used to bind and attach
     *   vidPid       "1532:007b"
     *   description  best human-readable name available
     *   shared       bool — the exporter currently offers it
     *   wanted       bool — the user asked for it
     *
     * `shared` and `wanted` are separate on purpose. They disagree whenever the service
     * has not caught up with the user yet (or could not), and a single bool would hide
     * exactly the state the user needs to see.
     */
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)

    /*
     * True when this machine can enumerate at all. False on a box with no USB/IP installed,
     * which is the normal case for an importer-side machine that happens to run ArtMoon.
     * Gates the whole Input section: an empty list with no explanation reads as a bug.
     */
    Q_PROPERTY(bool available READ available NOTIFY devicesChanged)

    /* Why `available` is false, ready to show. Empty when it is true. */
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY devicesChanged)

    /*
     * True when the privileged input service is installed, which is what makes a share
     * toggle able to do anything.
     *
     * ⚠️ Derived from the service actually being present, NOT from a constant someone has
     * to remember to flip. Installing the service — the moment it lands — is what turns the
     * toggles live; until then they are greyed with the reason named on the row, because a
     * switch that looks live and silently does nothing is worse than one that says why it
     * cannot. See artmoon-gui-development: "gating a row on what the build actually supports".
     */
    Q_PROPERTY(bool canShare READ canShare NOTIFY devicesChanged)

    /* Why `canShare` is false, ready to show in the row caption. */
    Q_PROPERTY(QString canShareReason READ canShareReason NOTIFY devicesChanged)

    /*
     * Why another machine cannot reach this one, ready to show. Empty when it can.
     *
     * Separate from the two above because it is a different kind of problem with a different
     * remedy. `canShare` false means the toggle cannot act at all; this means it acted, the
     * device really is offered, and the other machine still cannot see it — because there is
     * no listener on the USB/IP port, or because no client is connected for a firewall rule to
     * be scoped to. Every symptom on this screen looks identical from the far end, so without
     * this the only way to tell them apart is to go and test from the other machine.
     */
    Q_PROPERTY(QString reachabilityReason READ reachabilityReason NOTIFY devicesChanged)

    /*
     * The last toggle that did not take, ready to show. Empty when the machine is doing
     * what was asked of it.
     *
     * "Not shared yet" is honest and useless on its own: it says the request went out and
     * the device did not change, but not whether anything was listening to it. A row that
     * sits in that state forever with nothing else said is a dead switch with extra steps —
     * and it is exactly how a service whose pipe had never once been created managed to look
     * like a service with nothing to report.
     */
    Q_PROPERTY(QString toggleFailure READ toggleFailure NOTIFY devicesChanged)

    /*
     * True when this build carries the service and could set it up here: the helper is inside
     * the bundle but not yet at its system path.
     *
     * Separate from canShare because they answer different questions. canShare asks "can a
     * toggle act right now"; this asks "is there something I can offer to fix that". A build
     * with no helper to install must not show a button that leads nowhere — same rule as a
     * switch that looks live and does nothing.
     */
    Q_PROPERTY(bool canInstallService READ canInstallService NOTIFY devicesChanged)

    /* True while the install is in flight, so the UI can say what is happening. */
    Q_PROPERTY(bool installingService READ installingService NOTIFY installingServiceChanged)

public:
    explicit UsbIpDevices(QObject *parent = nullptr);
    ~UsbIpDevices() override;

    static UsbIpDevices *get(QQmlEngine *qmlEngine);

    QVariantList devices() const { return m_Devices; }
    bool available() const { return m_Available; }
    QString unavailableReason() const { return m_UnavailableReason; }
    bool canShare() const { return m_CanShare; }
    QString canShareReason() const { return m_CanShareReason; }
    QString reachabilityReason() const { return m_ReachabilityReason; }
    QString toggleFailure() const { return m_ToggleFailure; }
    bool canInstallService() const { return m_CanInstallService; }
    bool installingService() const { return m_InstallingService; }

    /* Re-run the local enumeration and rebuild `devices`. Safe to call at any time. */
    Q_INVOKABLE void refresh();

    /*
     * Record the user's intent for one device and ask the service to reconcile.
     *
     * Deliberately does not bind here. If the service is absent this records the intent and
     * leaves `shared` alone, so the tab shows "wanted but not shared" rather than lying.
     */
    Q_INVOKABLE void setWanted(const QString &busid, bool wanted);

    /*
     * Put the bundled helper at its system path, asking for administrator rights once.
     *
     * No-ops unless canInstallService. Refreshes afterwards and decides from the result, not
     * from the exit code: canShare is derived from the helper actually being present, and
     * that is the only thing that turns the toggles live.
     */
    Q_INVOKABLE void installInputService();

signals:
    void devicesChanged();
    void installingServiceChanged();

private:
    void rebuild();
    QStringList wantedBusids() const;
    void storeWantedBusids(const QStringList &busids);
    static QString bundledHelperPath();
    static QString bundledPolicyPath();
    static QString bundledUsbipPath();
    static QString bundledUsbipLibPath();
    static QString bundledUsbipdPath();

    /* The bounded second look after a toggle. See scheduleSettle() in the .cpp. */
    void scheduleSettle();
    void settle();
    bool anyUnsettled() const;

    /*
     * Ask the service to reconcile. True when it ran and did not refuse.
     *
     * The return value exists for watchForPeer(): a poll has to be able to tell "opened" from
     * "refused" without inventing a state of its own. See the .cpp.
     */
    bool reconcileWithService(const QStringList &busids, bool isUserToggle);

    /*
     * Open the connection for the machine streaming here, once there is one.
     *
     * Without this the firewall rule could only ever be opened by the user toggling a second
     * time, because the rule is scoped to a machine that is already in a session and no
     * session exists at the moment of the toggle. See the .cpp.
     */
    void watchForPeer();

    /* Why a toggle did not take, as far as we can actually tell. See the .cpp. */
    QString describeToggleFailure() const;

    /*
     * Is anything of ours bound right now?
     *
     * The idle path in watchForPeer() can only ever give a device back, so it needs to know
     * whether there is anything to hand back before it asks for a privileged run. See the .cpp.
     */
    bool anythingExported() const;

    /*
     * Is the helper on this PC the one this build carries?
     *
     * Presence is not currency: an install from an older build satisfies every other property
     * canShare checks, so a fix could ship and never reach the machine. See the .cpp.
     */
    bool helperMatchesBundle();

    /* Cached, because refresh() is on a timer and the answer only changes when we install. */
    bool m_HelperMatchKnown = false;
    bool m_HelperMatches = false;

    QVariantList m_Devices;
    bool m_Available = false;

    /*
     * How many consecutive reads a ticked busid has been missing from the device list.
     *
     * A count rather than a flag, so a device that is briefly absent — a hub re-enumerating, a
     * replug — keeps the tick it had. See the prune in the .cpp.
     */
    QHash<QString, int> m_MissingFor;

    QString m_UnavailableReason;
    bool m_CanShare = false;
    QString m_CanShareReason;
    QString m_ReachabilityReason;
    QString m_ToggleFailure;
    bool m_CanInstallService = false;
    bool m_InstallingService = false;

    QTimer m_SettleTimer;
    int m_SettleTries = 0;

    /* Repeating, and never stopped — it is also what re-reads the device list. See refresh(). */
    QTimer m_PeerTimer;
    QElapsedTimer m_PeerSeen;     /* when the helper last named a peer; see kPeerGraceMs */
    QString m_ReachablePeer;      /* as last reported by the helper, empty when unreachable */
    QString m_LastPeer;           /* the one the attempts below belong to */
    QString m_OpenedForStream;    /* what the port is already open for: the wanted set, joined.
                                   * Keyed on the stream rather than on a peer address, because the
                                   * peer cannot appear until this is open. See watchForPeer(). */
    int m_PeerAttempts = 0;
};
