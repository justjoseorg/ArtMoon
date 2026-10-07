#include "vendorengine.h"

#include <QProcess>
#include <QSettings>
#include <QChar>

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {

/* "C:\Program Files\Razer\RazerAppEngine\RazerAppEngine.exe" and "RazerAppEngine.exe" both
 * answer "RazerAppEngine". Path separators and the extension are not part of the identity. */
QString baseName(const QString &name)
{
    QString base = name;
    const int slash = qMax(base.lastIndexOf(QLatin1Char('\\')), base.lastIndexOf(QLatin1Char('/')));
    if (slash >= 0) {
        base = base.mid(slash + 1);
    }
    if (base.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) {
        base.chop(4);
    }
    return base;
}

/*
 * Split an argument string into arguments: whitespace separates, and a quoted run is one
 * argument with its quotes removed.
 *
 * Deliberately small. The only strings this ever sees are Run-key values written by an
 * installer, and a full command-line grammar — backslash escaping, caret quoting — would be a
 * liability here rather than a feature: the more it accepts, the more ways it can quietly
 * disagree with what Windows would actually run.
 */
QStringList splitArguments(const QString &text)
{
    QStringList arguments;
    QString current;
    bool quoted = false;

    for (const QChar c : text) {
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
            continue;
        }
        if (!quoted && (c == QLatin1Char(' ') || c == QLatin1Char('\t'))) {
            if (!current.isEmpty()) {
                arguments.append(current);
                current.clear();
            }
            continue;
        }
        current.append(c);
    }

    if (!current.isEmpty()) {
        arguments.append(current);
    }
    return arguments;
}

const QString kRunKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\"
                                       "CurrentVersion\\Run");

#ifdef Q_OS_WIN32

struct EngineProcess {
    DWORD   pid;
    QString engine; // the known name it matched, not the raw image name
};

/*
 * Every running process whose image name names a known engine.
 *
 * Toolhelp rather than a psapi enumeration: it is all kernel32, it hands back the image name
 * directly, and it needs no extra library on the link line. Nothing is matched by window title,
 * by path or by command line — an engine is an exe name and nothing else.
 */
QList<EngineProcess> engineProcesses(const QStringList &known)
{
    QList<EngineProcess> found;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return found;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            const QString name = baseName(QString::fromWCharArray(entry.szExeFile));
            for (const auto &knownName : known) {
                if (name.compare(knownName, Qt::CaseInsensitive) == 0) {
                    found.append({ entry.th32ProcessID, knownName });
                    break;
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

#endif // Q_OS_WIN32

} // namespace

QStringList VendorEngine::knownEngineNames()
{
    /*
     * Razer Synapse's engine, and so far only that. The list is the extension point: a second
     * vendor is a name added here, and every other line in this file then applies to it unchanged.
     */
    return { QStringLiteral("RazerAppEngine") };
}

QString VendorEngine::displayName(const QString &engineName)
{
    if (engineName.compare(QStringLiteral("RazerAppEngine"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Razer Synapse");
    }
    return engineName;
}

VendorEngine::Command VendorEngine::parseCommand(const QString &value)
{
    Command command;

    QString text = value.trimmed();
    if (text.isEmpty()) {
        return command;
    }

    if (text.startsWith(QLatin1Char('"'))) {
        const int close = text.indexOf(QLatin1Char('"'), 1);
        if (close > 1) {
            command.exe = text.mid(1, close - 1);
            text = text.mid(close + 1).trimmed();
        }
        else {
            // An opening quote and no closing one. Take the rest as the program rather than
            // inventing a split point — a malformed Run value should fail closed, not produce a
            // plausible-looking path that then gets launched.
            command.exe = text.mid(1);
            text.clear();
        }
    }
    else {
        const int space = text.indexOf(QLatin1Char(' '));
        command.exe = (space < 0) ? text : text.left(space);
        text = (space < 0) ? QString() : text.mid(space + 1).trimmed();
    }

    if (!text.isEmpty()) {
        command.arguments = splitArguments(text);
    }
    return command;
}

QString VendorEngine::engineNameFor(const Command &command)
{
    if (!command.isValid()) {
        return QString();
    }

    const QString name = baseName(command.exe);
    for (const auto &knownName : knownEngineNames()) {
        if (name.compare(knownName, Qt::CaseInsensitive) == 0) {
            return knownName;
        }
    }
    return QString();
}

QList<VendorEngine::Command> VendorEngine::engineCommands()
{
#ifndef Q_OS_WIN32
    /*
     * Windows-only, and not as a limitation of effort: the engine being moved out of the way is
     * a Windows application launched by a Run key. There is no equivalent here to read, so the
     * honest answer is that this machine has nothing of the kind.
     */
    return {};
#else
    QList<Command> commands;

    QSettings run(kRunKey, QSettings::NativeFormat);
    for (const auto &key : run.allKeys()) {
        const Command command = parseCommand(run.value(key).toString());
        if (!engineNameFor(command).isEmpty()) {
            commands.append(command);
        }
    }
    return commands;
#endif
}

bool VendorEngine::anyEngineInstalled()
{
    return !engineCommands().isEmpty();
}

QString VendorEngine::runningEngine()
{
#ifndef Q_OS_WIN32
    return QString();
#else
    const QList<EngineProcess> running = engineProcesses(knownEngineNames());
    return running.isEmpty() ? QString() : running.first().engine;
#endif
}

bool VendorEngine::stopRunningEngine()
{
#ifndef Q_OS_WIN32
    // Nothing of this kind runs here, so nothing is left running: the postcondition holds.
    return true;
#else
    const QList<EngineProcess> running = engineProcesses(knownEngineNames());
    for (const auto &process : running) {
        /*
         * Terminate, not a polite close. The engine's own shutdown path is a tray application
         * deciding to exit, and there is nothing here worth waiting for: this runs in the middle
         * of taking a device, where the only thing that matters is that the engine is gone before
         * the bind is asked for.
         *
         * The process may already have exited, or may refuse to open — either way the check below
         * is what decides, not this call.
         */
        HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, process.pid);
        if (handle) {
            TerminateProcess(handle, 0);
            CloseHandle(handle);
        }
    }

    /*
     * Verified, not assumed — and verified by re-reading the machine rather than by trusting the
     * calls above. A take that proceeds on an engine that is still up is the flap this whole file
     * exists to prevent, and it would look exactly like a take that worked.
     *
     * Measured: the engine does not come back on its own. Five minutes of polling after a stop
     * showed no processes, no restart attempts in the event log, and no scheduled task that could
     * relaunch it — which is also why the restore below is ours to do rather than something the
     * system will handle.
     */
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (engineProcesses(knownEngineNames()).isEmpty()) {
            return true;
        }
        Sleep(100);
    }
    return false;
#endif
}

bool VendorEngine::restoreEngine()
{
#ifndef Q_OS_WIN32
    return false;
#else
    // Idempotent first: an engine that is already up is left exactly as it is, so a restore that
    // runs twice — a release, then a quit, then a launch — cannot produce two of them.
    if (!runningEngine().isEmpty()) {
        return true;
    }

    const QList<Command> commands = engineCommands();
    for (const auto &command : commands) {
        if (!command.isValid()) {
            continue;
        }
        /*
         * startDetached, and no waiting. The engine's tree takes around thirty seconds to appear,
         * and blocking the UI on it would freeze the window that is meant to be telling the person
         * what is happening. It also keeps this an ordinary launch into this process's own
         * session — which is the one place it definitely works, and the reason none of the
         * scheduled-task machinery the helper-side notes describe is needed here.
         */
        if (QProcess::startDetached(command.exe, command.arguments)) {
            return true;
        }
    }
    return false;
#endif
}
