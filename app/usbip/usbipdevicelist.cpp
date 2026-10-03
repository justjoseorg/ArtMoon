#include "usbipdevicelist.h"

#include <QRegularExpression>

namespace UsbIpDeviceList {

namespace {

// "1532:007b" — every real row carries one, and it is the cheapest way to reject the
// section headers and any wrapping noise the tools add.
bool looksLikeVidPid(const QString &token)
{
    static const QRegularExpression re(QStringLiteral("^[0-9a-fA-F]{4}:[0-9a-fA-F]{4}$"));
    return re.match(token).hasMatch();
}

} // namespace

QVector<LocalUsbDevice> parseUsbipdList(const QString &output)
{
    QVector<LocalUsbDevice> devices;

    bool inConnected = false;
    const QStringList lines = output.split(QLatin1Char('\n'));

    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();

        if (line.startsWith(QStringLiteral("Connected:"))) {
            inConnected = true;
            continue;
        }

        // Everything after this is the rebind record, not live hardware. Stop rather
        // than hoping the next rows are distinguishable.
        if (line.startsWith(QStringLiteral("Persisted:"))) {
            break;
        }

        if (!inConnected || line.isEmpty()) {
            continue;
        }

        const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")),
                                             Qt::SkipEmptyParts);
        if (parts.size() < 2 || !looksLikeVidPid(parts.at(1))) {
            continue; // "BUSID VID:PID DEVICE STATE" and anything malformed
        }

        LocalUsbDevice dev;
        dev.busid = parts.at(0);
        dev.vidPid = parts.at(1);

        // The description can contain spaces, commas and brackets, so rebuild it from
        // the remaining tokens and then peel the state off the end.
        QString rest = parts.mid(2).join(QLatin1Char(' ')).trimmed();

        // Check "Not shared" first — it does not end with the capitalised "Shared", but
        // testing in the other order is a trap waiting for a case change upstream.
        const QString notShared = QStringLiteral("Not shared");
        const QString shared = QStringLiteral("Shared");

        if (rest.endsWith(notShared)) {
            dev.exported = false;
            rest.chop(notShared.size());
        } else if (rest.endsWith(shared)) {
            dev.exported = true;
            rest.chop(shared.size());
        } else {
            dev.exported = false;
        }

        dev.description = rest.trimmed();
        devices.append(dev);
    }

    return devices;
}

QVector<LocalUsbDevice> parseUsbipList(const QString &output,
                                       const QStringList &exportedBusids)
{
    QVector<LocalUsbDevice> devices;

    static const QRegularExpression busidLine(
        QStringLiteral("^-\\s*busid\\s+(\\S+)\\s+\\(([0-9a-fA-F]{4}:[0-9a-fA-F]{4})\\)$"));
    static const QRegularExpression trailingVidPid(
        QStringLiteral("\\s*\\([0-9a-fA-F]{4}:[0-9a-fA-F]{4}\\)\\s*$"));

    const QStringList lines = output.split(QLatin1Char('\n'));

    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        const QRegularExpressionMatch m = busidLine.match(line);
        if (!m.hasMatch()) {
            continue;
        }

        LocalUsbDevice dev;
        dev.busid = m.captured(1);
        dev.vidPid = m.captured(2);
        dev.exported = exportedBusids.contains(dev.busid);

        // The name is on the following non-empty line, when there is one. It comes from
        // the static usb.ids table and can be wrong — see the header.
        for (int j = i + 1; j < lines.size(); ++j) {
            const QString next = lines.at(j).trimmed();
            if (next.isEmpty()) {
                continue;
            }
            if (!busidLine.match(next).hasMatch()) {
                dev.description = next;
                dev.description.remove(trailingVidPid);
                dev.description = dev.description.trimmed();
            }
            break;
        }

        devices.append(dev);
    }

    return devices;
}

} // namespace UsbIpDeviceList
