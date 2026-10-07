/**
 * Tests for the vendor-engine handling around a USB/IP take.
 *
 * The Run-key fixture below is a VERBATIM capture from niks-minipc, read on 2026-10-05:
 *
 *   HKCU\Software\Microsoft\Windows\CurrentVersion\Run
 *     RazerAppEngine = "C:\Program Files\Razer\RazerAppEngine\RazerAppEngine.exe"
 *                      --url-params=apps=synapse --launch-force-hidden=synapse --autoStart=1
 *
 * No invented output, and no Windows needed: everything exercised here is the parsing and
 * matching that runs on every platform. The registry read and the process handling are Windows
 * only, are thin, and are verified on hardware instead.
 *
 * Build and run:  ./run-vendorengine.sh
 */

#include "../../app/usbip/vendorengine.h"

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

// ---- real capture, niks-minipc, 2026-10-05 ------------------------------------------
static const char *kRazerRunValue =
    "\"C:\\Program Files\\Razer\\RazerAppEngine\\RazerAppEngine.exe\" "
    "--url-params=apps=synapse --launch-force-hidden=synapse --autoStart=1";

static const char *kRazerExe = "C:\\Program Files\\Razer\\RazerAppEngine\\RazerAppEngine.exe";

int main()
{
    // ---- the real Run value ------------------------------------------------------
    {
        const VendorEngine::Command command = VendorEngine::parseCommand(QLatin1String(kRazerRunValue));

        checkEq(command.exe, QLatin1String(kRazerExe),
                QStringLiteral("the quoted path is the program, in one piece"));
        check(command.arguments.size() == 3,
              QStringLiteral("the real value yields three arguments, got %1")
                  .arg(command.arguments.size()));
        checkEq(command.arguments.value(0), QStringLiteral("--url-params=apps=synapse"),
                QStringLiteral("first argument"));
        checkEq(command.arguments.value(1), QStringLiteral("--launch-force-hidden=synapse"),
                QStringLiteral("second argument"));
        checkEq(command.arguments.value(2), QStringLiteral("--autoStart=1"),
                QStringLiteral("third argument"));

        // The whole point of quoting: a split on the first space would produce "C:\Program".
        check(!command.exe.endsWith(QLatin1String("Program")),
              QStringLiteral("the path was not split at its first space"));
    }

    // ---- which program does a command name? --------------------------------------
    checkEq(VendorEngine::engineNameFor(VendorEngine::parseCommand(QLatin1String(kRazerRunValue))),
            QStringLiteral("RazerAppEngine"),
            QStringLiteral("the Run value names the Razer engine"));

    // Matched on the program's base name: case, directory and extension are not identity.
    checkEq(VendorEngine::engineNameFor(VendorEngine::parseCommand(
                QStringLiteral("\"C:\\Program Files\\Razer\\RazerAppEngine\\razerappengine.EXE\""))),
            QStringLiteral("RazerAppEngine"),
            QStringLiteral("case and the extension do not change the match"));

    // An unrelated program must not match, however Razer-ish its name looks.
    checkEq(VendorEngine::engineNameFor(VendorEngine::parseCommand(
                QStringLiteral("\"C:\\Windows\\System32\\notepad.exe\" --title=RazerAppEngine"))),
            QString(),
            QStringLiteral("the ARGUMENTS must never decide the match — only the program does"));

    checkEq(VendorEngine::engineNameFor(VendorEngine::parseCommand(
                QStringLiteral("\"C:\\Program Files\\Steam\\steam.exe\" -silent"))),
            QString(),
            QStringLiteral("an unrelated Run entry does not match"));

    // A path that merely CONTAINS the engine's name, as a file, is not the engine.
    checkEq(VendorEngine::engineNameFor(VendorEngine::parseCommand(
                QStringLiteral("\"C:\\Program Files\\Razer\\notes\\RazerAppEngine.txt\""))),
            QString(),
            QStringLiteral("a file whose name starts with the engine's is not the engine"));

    // ---- malformed values fail closed -------------------------------------------
    check(!VendorEngine::parseCommand(QString()).isValid(),
          QStringLiteral("an empty value names nothing"));
    check(!VendorEngine::parseCommand(QStringLiteral("   ")).isValid(),
          QStringLiteral("a whitespace-only value names nothing"));

    {
        // An opening quote with no closing one. The rest becomes the program rather than a
        // plausible-looking first token, so nothing invented can reach a launch.
        const VendorEngine::Command command =
            VendorEngine::parseCommand(QStringLiteral("\"C:\\Program Files\\Broken\\app.exe"));
        checkEq(command.exe, QStringLiteral("C:\\Program Files\\Broken\\app.exe"),
                QStringLiteral("an unterminated quote takes the remainder as the program"));
        check(command.arguments.isEmpty(),
              QStringLiteral("an unterminated quote yields no arguments"));
    }

    // Unquoted, as some installers write it: the first token is the program.
    {
        const VendorEngine::Command command =
            VendorEngine::parseCommand(QStringLiteral("C:\\Tools\\app.exe --flag 1"));
        checkEq(command.exe, QStringLiteral("C:\\Tools\\app.exe"),
                QStringLiteral("an unquoted value takes the first token as the program"));
        check(command.arguments.size() == 2,
              QStringLiteral("an unquoted value keeps its arguments"));
    }

    // ---- argument splitting ------------------------------------------------------
    {
        const VendorEngine::Command command = VendorEngine::parseCommand(
            QStringLiteral("\"C:\\app.exe\" --title=\"My Thing\" --plain"));
        checkEq(command.arguments.value(0), QStringLiteral("--title=My Thing"),
                QStringLiteral("quotes around a value keep it as one argument, quotes removed"));
        checkEq(command.arguments.value(1), QStringLiteral("--plain"),
                QStringLiteral("a plain argument after a quoted one still lands"));
    }

    // Runs of spaces must not produce empty arguments — an empty argv entry is the kind of
    // thing that only shows up as a launch failing for no stated reason.
    {
        const VendorEngine::Command command =
            VendorEngine::parseCommand(QStringLiteral("\"C:\\app.exe\"   --a    --b"));
        check(command.arguments.size() == 2,
              QStringLiteral("repeated spaces do not produce empty arguments, got %1")
                  .arg(command.arguments.size()));
    }

    // ---- the known list and the human name ---------------------------------------
    check(VendorEngine::knownEngineNames().contains(QStringLiteral("RazerAppEngine")),
          QStringLiteral("RazerAppEngine is a known engine"));
    checkEq(VendorEngine::displayName(QStringLiteral("RazerAppEngine")),
            QStringLiteral("Razer Synapse"),
            QStringLiteral("the engine has a name a person would recognise"));
    checkEq(VendorEngine::displayName(QStringLiteral("SomethingElse")),
            QStringLiteral("SomethingElse"),
            QStringLiteral("an unknown name is reported as itself rather than as Razer"));

#ifdef Q_OS_WIN32
    // Nothing to assert about the registry or the process table here: those are the Windows
    // halves, and they are checked on a real machine.
#else
    // On anything but Windows there is no Run key to read, and this feature must not pretend
    // otherwise — a false "installed" would put a Synapse warning in front of a user who has
    // never had Synapse.
    check(!VendorEngine::anyEngineInstalled(),
          QStringLiteral("non-Windows reports no engine installed"));
    check(VendorEngine::engineCommands().isEmpty(),
          QStringLiteral("non-Windows finds no engine command"));
    check(VendorEngine::runningEngine().isEmpty(),
          QStringLiteral("non-Windows reports no engine running"));
    check(VendorEngine::stopRunningEngine(),
          QStringLiteral("with nothing to stop, the postcondition still holds"));
#endif

    std::printf("%d checks, %s\n", g_checks, g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
