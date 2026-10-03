/**
 * USB/IP input passthrough — exporter side, device list.
 *
 * Terminology is locked; read docs/usb-ip-input-passthrough.md before touching this.
 * This file describes the devices plugged into THIS machine and which of them are
 * currently made shareable to an importer. It is the data behind the Input settings tab.
 *
 * Do not name anything here host / client / server — "usbipd" calls the device side the
 * host, and that word means the opposite in streaming. Use exporter / importer.
 *
 * The parsers are pure (string in, list out) so they can be tested without the real
 * tools, without admin rights, and without any hardware attached.
 */

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

struct LocalUsbDevice
{
    QString busid;        // "9-1", "1-6" — the stable handle used to bind and attach
    QString vidPid;       // "1532:007b"
    QString description;  // best human-readable name available
    bool exported;        // currently made shareable to an importer
};

namespace UsbIpDeviceList {

/**
 * Windows: parse the output of `usbipd list`.
 *
 * Reads the "Connected:" table only — the "Persisted:" table that follows is a record of
 * bindings across reboots, not a list of live devices, and picking rows out of it is how
 * you end up offering a device that is not plugged in.
 *
 * Note the state is reported here, not inferred: `usbipd` says "Shared" or "Not shared"
 * for every connected device.
 */
QVector<LocalUsbDevice> parseUsbipdList(const QString &output);

/**
 * Linux: parse the output of `usbip list -l`, given the busids currently exported.
 *
 * There is no state column on Linux — an exported device is one bound to the
 * `usbip-host` driver, so pass the entries of /sys/bus/usb/drivers/usbip-host/ in
 * `exportedBusids`. That directory does not exist until the module is loaded, so an
 * empty list is normal and means "nothing is exported", not "the tool failed".
 *
 * Caveat: the names in `usbip list -l` come from the static usb.ids table, which is
 * frequently wrong or stale — verified 2026-10-03, where a CSCTEK "USB Audio and HID"
 * box was reported as "Zoran Co. Personal Media Division (Nogatech)". Prefer the
 * product/manufacturer strings from sysfs where a better name is needed.
 */
QVector<LocalUsbDevice> parseUsbipList(const QString &output,
                                       const QStringList &exportedBusids);

} // namespace UsbIpDeviceList
