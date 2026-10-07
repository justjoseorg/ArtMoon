#pragma once

/*
 * The vendor app that fights a USB/IP take, and the two things we do about it.
 *
 * Measured on the mini PC, 2026-10-05: with Razer Synapse's engine running, a shared Razer
 * mouse or keyboard FLAPS — taken, handed back, taken, handed back, every couple of seconds —
 * and `usbipd` reports `Shared (forced)` throughout, character for character the same as a take
 * that worked. With the engine stopped, the same bind on the same device is a clean take and the
 * device stays gone for as long as it is shared.
 *
 * So a device cannot be shared while its vendor engine is up, and the fix is to move that engine
 * out of the way for the seconds the device is actually attached. Two rules follow from the
 * measurements and are not negotiable:
 *
 *   - STOP PROCESSES ONLY. Both vendor services stay running and untouched. Stopping the
 *     engine's processes alone was measured sufficient on a fresh boot, and a service we never
 *     touch is a service we cannot break and never have to put back.
 *   - THIS IS THE USER'S SESSION. The engine is launched from an HKCU Run key, so it runs as the
 *     user — which is why ArtMoon, running as the same user, can stop it and start it again with
 *     no elevation prompt, no service change and no scheduled task. That is the whole reason this
 *     belongs here rather than in the privileged helper.
 *
 * Little here is Razer-specific in shape. The engine is identified by the Run key Windows uses
 * to launch it, which is at once the exact command line to restore it and the proof that this is
 * the machine's own way of starting it. A second vendor is a name added to one list.
 *
 * Nothing is cached and nothing is written down: every answer is read from the machine at the
 * moment it is asked for, which is the rule the rest of the sharing code is held to.
 */

#include <QList>
#include <QString>
#include <QStringList>

class VendorEngine
{
public:
    /* A Run-key value, split into the program and the arguments it was written with. */
    struct Command {
        QString     exe;
        QStringList arguments;

        bool isValid() const { return !exe.isEmpty(); }
    };

    /*
     * Executable names, without the ".exe", of vendor engines known to fight the stub driver.
     * Compared case-insensitively.
     */
    static QStringList knownEngineNames();

    /* The name a person would recognise, for a message. Falls back to the exe name. */
    static QString displayName(const QString &engineName);

    /*
     * The known engine a Run-key command belongs to, or an empty string. Read from the program
     * the command names — never from the arguments, which an installer could point anywhere.
     */
    static QString engineNameFor(const Command &command);

    /*
     * Split a Run-key value into program and arguments.
     *
     * Windows writes "[quoted path] args..." — and the quotes are load-bearing, because the path
     * contains spaces and a naive split on the first space yields "C:\Program". An unquoted value
     * is accepted too: the first token is then the program.
     */
    static Command parseCommand(const QString &value);

    /*
     * The engine's own logon command, read from the Run key rather than guessed at. This is the
     * entire restore mechanism: the machine already knows how to start this program, and this is
     * where it says so. Empty when no known engine has a Run entry — which is also how we learn
     * an engine is installed at all.
     */
    static QList<Command> engineCommands();

    /*
     * Whether a known engine is installed on this machine, asked of the Run key. False means
     * there is nothing to stop and nothing to restore, and this feature has no opinion about the
     * machine at all.
     */
    static bool anyEngineInstalled();

    /*
     * The name of the known engine currently running, or an empty string. Empty is the ordinary
     * case — a machine with no vendor peripherals has nothing to move out of the way.
     */
    static QString runningEngine();

    /*
     * Stop the running engine's processes, and return true once none is left.
     *
     * Services are never touched — see the note at the top of this file. Only processes whose
     * image name names a known engine are ended; nothing is matched by window title, by path or
     * by command line.
     */
    static bool stopRunningEngine();

    /*
     * Start the engine again, the way the machine starts it at logon. Idempotent: an engine
     * already running is left alone, so a restore that runs twice cannot produce two engines.
     *
     * Deliberately fire-and-forget. This is an ordinary launch into the caller's own session; the
     * engine reports for itself once it is up, and waiting on it here would block the UI for the
     * ~30 seconds its process tree takes to appear.
     */
    static bool restoreEngine();
};
