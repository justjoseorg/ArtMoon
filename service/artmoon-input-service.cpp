// artmoon-input-service — the privileged exporter-side helper for USB/IP input
// passthrough.
//
// ArtMoon runs unprivileged and can enumerate, but it cannot *bind*: making a device
// shareable is a privileged, persistent, local operation. Verified on the exporter
// 2026-10-03 — `attach` runs fine as a normal user, `bind` is refused (a scheduled task
// registered RunLevel=Limited got LastTaskResult 10 against the device, which never left
// "Not shared"). So binding lives here, in a helper ArtMoon's own installer lands — the
// same shape as the service usbipd-win installs for itself.
//
// Terminology is locked in docs/usb-ip-input-passthrough.md: exporter / importer, never
// host / client / server. Read that before changing anything in this file. On our pair of
// machines the words invert — streaming-host is the USB/IP-client — so a bare "host" in
// this code is a bug waiting to happen.
//
// ── Contract ─────────────────────────────────────────────────────────────────────
//
//   artmoon-input-service reconcile [--want <busid>]... [--local <busid>]... [--dry-run]
//   artmoon-input-service status                                                      
//
// `reconcile` binds every --want device, unbinds every --local device that is not wanted,
// and prints one line per action. ArtMoon passes both lists explicitly and this helper
// keeps no state of its own, so there is nothing on disk here that can disagree with what
// the user sees in the Input tab. `--want` and `--local` are named in the arg list rather
// than read from a file because the helper runs as root: it must never be pointed at a
// path a normal user can rewrite.
//
// Exit codes:  0 all actions succeeded
//              1 at least one action failed
//              2 bad arguments
//              3 needs privileges, and was not run with --dry-run
//
// ── Why the validation is this strict ────────────────────────────────────────────
//
// This program runs as root and its arguments come from an unprivileged process. A busid
// ends up in a usbip command line, so anything that is not exactly a busid is a
// privilege-escalation surface. Shape, length and alphabet are all checked before a single
// process is spawned, and on Linux the device must also exist in sysfs — so this can only
// ever act on a USB device that is physically present on this machine.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {

// Where the kernel lists the devices this exporter currently offers. Absent directory
// means the usbip-host module is not loaded, which means nothing is exported — the
// normal state, and NOT an error. Same source of truth as UsbIpDevices::refresh().
const char *kUsbIpHostDriver = "/sys/bus/usb/drivers/usbip-host";

// ── the plan ─────────────────────────────────────────────────────────────────────

struct Plan {
    std::vector<std::string> toBind;    // wanted, and not already offered
    std::vector<std::string> toUnbind;  // offered, and no longer wanted
    std::vector<std::string> rejected;  // failed validation — never acted on
};

// ── validation ───────────────────────────────────────────────────────────────────

// A USB busid: bus "-" port, then any number of ".port" hub hops. "3-10", "1-1.2", "9-3".
// Hand-rolled rather than <regex> so the whole rule is visible in one place and testable
// without pulling in anything.
bool isValidBusid(const std::string &s)
{
    if (s.empty() || s.size() > 16) {
        return false;
    }

    size_t i = 0;
    const auto digits = [&s, &i]() {
        const size_t start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            ++i;
        }
        return i > start;
    };

    if (!digits()) {                    // bus number
        return false;
    }
    if (i >= s.size() || s[i] != '-') {
        return false;
    }
    ++i;
    if (!digits()) {                    // first port
        return false;
    }
    while (i < s.size()) {              // optional hub chain
        if (s[i] != '.') {
            return false;
        }
        ++i;
        if (!digits()) {
            return false;
        }
    }
    return true;
}

bool deviceIsPresent(const std::string &busid)
{
#ifdef _WIN32
    // On Windows the exporter's device list comes from usbipd itself, and usbipd refuses
    // an unknown busid on its own. There is no sysfs equivalent to check against here.
    (void) busid;
    return true;
#else
    return access((std::string("/sys/bus/usb/devices/") + busid).c_str(), F_OK) == 0;
#endif
}

std::set<std::string> exportedBusids()
{
    std::set<std::string> out;
#ifndef _WIN32
    DIR *dir = opendir(kUsbIpHostDriver);
    if (!dir) {
        return out;                     // not loaded == nothing exported
    }
    while (const dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.find('-') != std::string::npos) {
            out.insert(name);
        }
    }
    closedir(dir);
#endif
    return out;
}

// ── running a tool ───────────────────────────────────────────────────────────────

struct ProcessResult {
    int exitCode = -1;
    std::string output;
};

#ifdef _WIN32

std::string quoteForCommandLine(const std::string &arg)
{
    std::string out = "\"";
    for (const char c : arg) {
        out += c;
        if (c == '"') {
            out += '\\';                // double it for the C runtime's parser
        }
    }
    out += '"';
    return out;
}

ProcessResult runProcess(const std::vector<std::string> &argv)
{
    ProcessResult result;

    std::string commandLine;
    for (const auto &arg : argv) {
        if (!commandLine.empty()) {
            commandLine += ' ';
        }
        commandLine += quoteForCommandLine(arg);
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        return result;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back('\0');

    if (CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(writeEnd);
        char buffer[512];
        DWORD read = 0;
        while (ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            result.output.append(buffer, read);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        result.exitCode = static_cast<int>(code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(readEnd);
    } else {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
    }
    return result;
}

#else  // POSIX

// fork + execvp, never a shell: the arguments are validated, but the right response to
// "these arguments are safe" is to not need them to be, not to rely on it.
ProcessResult runProcess(const std::vector<std::string> &argv)
{
    ProcessResult result;

    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        return result;
    }

    std::vector<char *> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto &arg : argv) {
        cargv.push_back(const_cast<char *>(arg.c_str()));
    }
    cargv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        return result;
    }

    if (pid == 0) {
        close(pipeFds[0]);
        dup2(pipeFds[1], STDOUT_FILENO);
        dup2(pipeFds[1], STDERR_FILENO);
        close(pipeFds[1]);
        execvp(cargv[0], cargv.data());
        _exit(127);                     // 127 == "could not execute", as a shell reports it
    }

    close(pipeFds[1]);
    char buffer[512];
    ssize_t n = 0;
    while ((n = read(pipeFds[0], buffer, sizeof(buffer))) > 0) {
        result.output.append(buffer, static_cast<size_t>(n));
    }
    close(pipeFds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

#endif

// ── the platform verbs ───────────────────────────────────────────────────────────

std::vector<std::string> bindCommand(const std::string &busid)
{
#ifdef _WIN32
    return { "usbipd", "bind", "--busid", busid };
#else
    return { "usbip", "bind", "-b", busid };
#endif
}

std::vector<std::string> unbindCommand(const std::string &busid)
{
#ifdef _WIN32
    return { "usbipd", "unbind", "--busid", busid };
#else
    return { "usbip", "unbind", "-b", busid };
#endif
}

// A tool that says "it is already in the state you asked for" has not failed. This is a
// text match because neither usbip nor usbipd offers a machine-readable status for a
// single busid — and it is deliberately narrow, so a real error is never swallowed.
bool outputSaysAlreadyDone(const std::string &output)
{
    const std::string lowered = [&output] {
        std::string s = output;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        return s;
    }();

    return lowered.find("already") != std::string::npos
        || lowered.find("device busy") != std::string::npos;
}

#ifndef _WIN32
// The kernel module has to be loaded before anything can be bound, and it is not loaded
// by default on a machine that has never exported a device.
bool ensureExporterModule()
{
    DIR *dir = opendir(kUsbIpHostDriver);
    if (dir) {
        closedir(dir);
        return true;
    }
    const auto result = runProcess({ "modprobe", "usbip-host" });
    return result.exitCode == 0;
}
#endif

} // namespace

int main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);

    bool dryRun = false;
    std::vector<std::string> wantList;
    std::vector<std::string> localList;
    std::string verb;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg == "--dry-run") {
            dryRun = true;
        } else if (arg == "--want" || arg == "--local") {
            if (i + 1 >= args.size()) {
                std::cerr << "artmoon-input-service: " << arg << " needs a busid\n";
                return 2;
            }
            (arg == "--want" ? wantList : localList).push_back(args[++i]);
        } else if (verb.empty()) {
            verb = arg;
        } else {
            std::cerr << "artmoon-input-service: unexpected argument '" << arg << "'\n";
            return 2;
        }
    }

    if (verb != "reconcile" && verb != "status") {
        std::cerr << "usage: artmoon-input-service reconcile [--want <busid>]... "
                     "[--local <busid>]... [--dry-run]\n"
                     "       artmoon-input-service status\n";
        return 2;
    }

    // ── everything the caller asked for is checked before anything runs ──────────
    Plan plan;
    for (const auto &busid : wantList) {
        if (!isValidBusid(busid) || !deviceIsPresent(busid)) {
            plan.rejected.push_back(busid);
        }
    }
    for (const auto &busid : localList) {
        if (!isValidBusid(busid)) {
            plan.rejected.push_back(busid);
        }
    }

    if (!plan.rejected.empty()) {
        for (const auto &busid : plan.rejected) {
            std::cerr << "artmoon-input-service: refusing '" << busid
                      << "' — not a busid for a device on this machine\n";
        }
        return 2;
    }

    if (verb == "status") {
        const auto exported = exportedBusids();
        if (exported.empty()) {
            std::cout << "nothing is being offered to an importer\n";
        } else {
            for (const auto &busid : exported) {
                std::cout << busid << " offered\n";
            }
        }
        return 0;
    }

#ifndef _WIN32
    if (!dryRun && geteuid() != 0) {
        std::cerr << "artmoon-input-service: binding needs administrator rights; "
                     "this ran without them. Nothing was changed.\n";
        return 3;
    }
#endif

    // ── work out what actually needs doing ───────────────────────────────────────
    const auto exported = exportedBusids();

    for (const auto &busid : wantList) {
        if (exported.find(busid) == exported.end()) {
            plan.toBind.push_back(busid);
        }
    }
    for (const auto &busid : localList) {
        const bool wanted = std::find(wantList.begin(), wantList.end(), busid) != wantList.end();
        if (!wanted && exported.find(busid) != exported.end()) {
            plan.toUnbind.push_back(busid);
        }
    }

    if (dryRun) {
        std::cout << "plan: " << plan.toBind.size() << " to bind, "
                  << plan.toUnbind.size() << " to unbind\n";
        for (const auto &busid : plan.toBind) {
            std::cout << "  would bind   " << busid << "\n";
        }
        for (const auto &busid : plan.toUnbind) {
            std::cout << "  would unbind " << busid << "\n";
        }
        return 0;
    }

    if (plan.toBind.empty() && plan.toUnbind.empty()) {
        std::cout << "already in the state you asked for\n";
        return 0;
    }

    int failures = 0;

#ifndef _WIN32
    if (!plan.toBind.empty() && !ensureExporterModule()) {
        std::cerr << "could not load the usbip-host module, so nothing can be offered\n";
        return 1;
    }
#endif

    for (const auto &busid : plan.toBind) {
        const auto result = runProcess(bindCommand(busid));
        if (result.exitCode == 0 || outputSaysAlreadyDone(result.output)) {
            std::cout << "binding   " << busid << "\n";
        } else {
            std::cout << "failed    " << busid << ": " << result.output << "\n";
            ++failures;
        }
    }

    for (const auto &busid : plan.toUnbind) {
        const auto result = runProcess(unbindCommand(busid));
        if (result.exitCode == 0 || outputSaysAlreadyDone(result.output)) {
            std::cout << "unbinding " << busid << "\n";
        } else {
            std::cout << "failed    " << busid << ": " << result.output << "\n";
            ++failures;
        }
    }

    return failures == 0 ? 0 : 1;
}
