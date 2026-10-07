/**
 * Parser tests for the exporter-side device list.
 *
 * Every fixture below is a VERBATIM capture from a real machine — no invented output.
 * The Windows fixtures come from niks-minipc (`usbipd list`), the Linux fixture from a
 * kernel 7.0 box with usbip tools installed (`usbip list -l`). The single exception states
 * so in its own comment: it is constructed to pin a RULE, not to record a machine.
 *
 * Build and run:  ./run.sh
 */

#include "../../app/usbip/usbipdevicelist.h"

#include <QString>
#include <QStringList>
#include <cstdio>

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const QString &what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", qPrintable(what));
    }
}

static void checkEq(const QString &actual, const QString &expected, const QString &what)
{
    check(actual == expected,
          QStringLiteral("%1 — expected \"%2\", got \"%3\"").arg(what, expected, actual));
}

// ---- real capture, niks-minipc, before 9-2 was bound -------------------------------
static const char *kUsbipdListBefore =
    "Connected:\n"
    "BUSID  VID:PID    DEVICE                                                        STATE\n"
    "1-5    8087:0029  Intel(R) Wireless Bluetooth(R)                                Not shared\n"
    "5-3    1532:007e  USB Input Device, Razer Mouse Dock                            Not shared\n"
    "9-1    1532:007b  Razer Viper Ultimate                                          Shared\n"
    "9-2    1532:028d  USB Input Device, Razer BlackWidow V4 Pro                     Not shared\n"
    "9-3    145f:02b4  Trust USB microphone, USB Input Device                         Not shared\n"
    "10-2   1532:0e05  Razer Kiyo Pro                                                Not shared\n"
    "\n"
    "Persisted:\n"
    "GUID                                  DEVICE\n";

// ---- real capture, niks-minipc, after 9-2 was bound --------------------------------
static const char *kUsbipdListAfter =
    "Connected:\n"
    "BUSID  VID:PID    DEVICE                                                        STATE\n"
    "1-5    8087:0029  Intel(R) Wireless Bluetooth(R)                                Not shared\n"
    "5-3    1532:007e  USB Input Device, Razer Mouse Dock                            Not shared\n"
    "9-1    1532:007b  Razer Viper Ultimate                                          Shared\n"
    "9-2    1532:028d  USB Input Device, Razer BlackWidow V4 Pro                     Shared\n"
    "9-3    145f:02b4  Trust USB microphone, USB Input Device                         Not shared\n"
    "10-2   1532:0e05  Razer Kiyo Pro                                                Not shared\n"
    "\n"
    "Persisted:\n"
    "GUID                                  DEVICE\n";

// ---- real capture, Linux box, kernel 7.0 ------------------------------------------
static const char *kUsbipListLocal =
    " - busid 1-6 (0573:1573)\n"
    "   Zoran Co. Personal Media Division (Nogatech) : unknown product (0573:1573)\n"
    "\n"
    " - busid 1-8 (0bda:b85b)\n"
    "   Realtek Semiconductor Corp. : unknown product (0bda:b85b)\n"
    "\n";

// ---- real capture, niks-minipc, 2026-10-06, with 1-6 bound using --force ----------
//
// Taken live from the machine while the drive was stuck. The row that matters is 1-6:
// usbipd prints `Shared (forced)`, and the parser used to match the state with
// endsWith("Shared"), so this row read as NOT SHARED — which is what stopped ArtMoon
// ever handing the drive back. See testForcedState().
static const char *kUsbipdListForced =
    "Connected:\n"
    "BUSID  VID:PID    DEVICE                                                        STATE\n"
    "1-5    8087:0029  Intel(R) Wireless Bluetooth(R)                                Not shared\n"
    "1-6    0bda:9210  USB Attached SCSI (UAS) Mass Storage Device                   Shared (forced)\n"
    "5-1    1532:007b  Razer Viper Ultimate                                          Not shared\n"
    "5-2    1532:028d  USB Input Device, Razer BlackWidow V4 Pro                     Not shared\n"
    "5-3    145f:02b4  Trust USB microphone, USB Input Device                        Not shared\n"
    "6-4    1a34:f517  USB Input Device                                              Not shared\n"
    "7-2    1532:0e05  Razer Kiyo Pro                                                Not shared\n"
    "8-3    1532:007e  USB Input Device, Razer Mouse Dock                            Not shared\n"
    "\n"
    "Persisted:\n"
    "GUID                                  DEVICE\n";

static void testWindows()
{
    const QVector<LocalUsbDevice> before =
        UsbIpDeviceList::parseUsbipdList(QString::fromUtf8(kUsbipdListBefore));

    checkEq(QString::number(before.size()), QStringLiteral("6"),
            QStringLiteral("usbipd: device count"));
    if (before.size() != 6) {
        return;
    }

    checkEq(before.at(2).busid, QStringLiteral("9-1"), QStringLiteral("usbipd: 3rd busid"));
    checkEq(before.at(2).vidPid, QStringLiteral("1532:007b"), QStringLiteral("usbipd: 3rd vid:pid"));
    checkEq(before.at(2).description, QStringLiteral("Razer Viper Ultimate"),
            QStringLiteral("usbipd: 3rd description"));
    check(before.at(2).exported, QStringLiteral("usbipd: 9-1 is Shared"));

    checkEq(before.at(3).busid, QStringLiteral("9-2"), QStringLiteral("usbipd: 4th busid"));
    checkEq(before.at(3).description,
            QStringLiteral("USB Input Device, Razer BlackWidow V4 Pro"),
            QStringLiteral("usbipd: description keeps commas and spaces"));
    check(!before.at(3).exported, QStringLiteral("usbipd: 9-2 is Not shared"));

    // A USB busid with two digits in the second field must not be confused with a column.
    checkEq(before.at(5).busid, QStringLiteral("10-2"), QStringLiteral("usbipd: '10-2' busid"));

    checkEq(before.at(5).description, QStringLiteral("Razer Kiyo Pro"),
            QStringLiteral("usbipd: trailing description"));

    // Nothing may come out of the Persisted: table.
    for (const LocalUsbDevice &d : before) {
        check(!d.busid.contains(QStringLiteral("GUID")),
              QStringLiteral("usbipd: no rows from the Persisted table"));
    }

    const QVector<LocalUsbDevice> after =
        UsbIpDeviceList::parseUsbipdList(QString::fromUtf8(kUsbipdListAfter));
    checkEq(QString::number(after.size()), QStringLiteral("6"),
            QStringLiteral("usbipd: device count unchanged after a bind"));
    if (after.size() == 6) {
        check(after.at(3).exported, QStringLiteral("usbipd: 9-2 flips to Shared"));
    }
}

static void testLinux()
{
    const QVector<LocalUsbDevice> none =
        UsbIpDeviceList::parseUsbipList(QString::fromUtf8(kUsbipListLocal), QStringList());
    checkEq(QString::number(none.size()), QStringLiteral("2"), QStringLiteral("usbip: device count"));
    if (none.size() != 2) {
        return;
    }

    checkEq(none.at(0).busid, QStringLiteral("1-6"), QStringLiteral("usbip: first busid"));
    checkEq(none.at(0).vidPid, QStringLiteral("0573:1573"), QStringLiteral("usbip: first vid:pid"));
    check(!none.at(0).exported, QStringLiteral("usbip: nothing exported with an empty driver list"));

    // The trailing "(0573:1573)" belongs to the name line, not the description.
    checkEq(none.at(0).description,
            QStringLiteral("Zoran Co. Personal Media Division (Nogatech) : unknown product"),
            QStringLiteral("usbip: name line loses its trailing vid:pid"));

    checkEq(none.at(1).busid, QStringLiteral("1-8"), QStringLiteral("usbip: second busid"));

    // Busids come from /sys/bus/usb/drivers/usbip-host/ on Linux, not from the tool.
    const QVector<LocalUsbDevice> one =
        UsbIpDeviceList::parseUsbipList(QString::fromUtf8(kUsbipListLocal),
                                        QStringList { QStringLiteral("1-6") });
    check(one.at(0).exported, QStringLiteral("usbip: 1-6 reads as exported"));
    check(!one.at(1).exported, QStringLiteral("usbip: 1-8 stays not exported"));

    // Empty output must be empty, not a crash or a phantom row.
    checkEq(QString::number(UsbIpDeviceList::parseUsbipList(QString(), QStringList()).size()),
            QStringLiteral("0"), QStringLiteral("usbip: empty input"));
    checkEq(QString::number(UsbIpDeviceList::parseUsbipdList(QString()).size()),
            QStringLiteral("0"), QStringLiteral("usbipd: empty input"));
}

/*
 * The state column is not always one word.
 *
 * `bind --force` makes usbipd report `Shared (forced)`. Every device we ever bound with
 * --force read as NOT shared here — so the row lied about a device that had genuinely been
 * taken from the user, and, far worse, ArtMoon concluded that nothing of its was out and
 * never ran the hand-back. The drive stayed gone from Windows through a reboot.
 */
static void testForcedState()
{
    const QVector<LocalUsbDevice> forced =
        UsbIpDeviceList::parseUsbipdList(QString::fromUtf8(kUsbipdListForced));

    checkEq(QString::number(forced.size()), QStringLiteral("8"),
            QStringLiteral("usbipd: device count with a forced take"));
    if (forced.size() != 8) {
        return;
    }

    const LocalUsbDevice &drive = forced.at(1);
    checkEq(drive.busid, QStringLiteral("1-6"), QStringLiteral("forced: busid"));
    checkEq(drive.vidPid, QStringLiteral("0bda:9210"), QStringLiteral("forced: vid:pid"));
    check(drive.exported, QStringLiteral("forced: 'Shared (forced)' IS shared"));

    // The qualifier belongs to the state, not to the name of the device.
    checkEq(drive.description, QStringLiteral("USB Attached SCSI (UAS) Mass Storage Device"),
            QStringLiteral("forced: description loses the (forced) qualifier"));

    // The unambiguous states must read exactly as they always did.
    check(!forced.at(0).exported, QStringLiteral("forced: 'Not shared' is still not shared"));
    checkEq(forced.at(0).description, QStringLiteral("Intel(R) Wireless Bluetooth(R)"),
            QStringLiteral("forced: a trailing 'Not shared' is still peeled off the name"));

    /*
     * Constructed, not captured: this pins the RULE rather than one particular row. If
     * usbipd ever grows another qualifier, the row must still read as shared rather than
     * quietly reverting to "not shared" — which is the whole failure being guarded here.
     */
    const QVector<LocalUsbDevice> otherQualifier = UsbIpDeviceList::parseUsbipdList(
        QStringLiteral("Connected:\n"
                       "BUSID  VID:PID    DEVICE                        STATE\n"
                       "9-1    1532:007b  Razer Viper Ultimate          Shared (something new)\n"));
    checkEq(QString::number(otherQualifier.size()), QStringLiteral("1"),
            QStringLiteral("forced: an unrecognised qualifier still yields a row"));
    if (!otherQualifier.isEmpty()) {
        check(otherQualifier.at(0).exported,
              QStringLiteral("forced: any parenthesised qualifier still means shared"));
    }
}

int main()
{
    testWindows();
    testLinux();
    testForcedState();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
