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
//   artmoon-input-service install --from <staged-directory>                           
//
// `reconcile` binds every --want device, unbinds every --local device that is not wanted,
// and prints one line per action. ArtMoon passes both lists explicitly and this helper
// keeps no state of its own, so there is nothing on disk here that can disagree with what
// the user sees in the Input tab. `--want` and `--local` are named in the arg list rather
// than read from a file because the helper runs as root: it must never be pointed at a
// path a normal user can rewrite.
//
// `reconcile` also makes this machine *reachable*, whenever --want is non-empty, because a
// device that is bound and unreachable is the failure this feature is most prone to. That
// means a listener on 3240 (started, and enabled so it survives a restart) and, on a machine
// that filters, a firewall rule for the one client we are actually serving. Both are derived
// here rather than configured, and neither opens anything wider than that single machine.
//
// `status` answers "can another machine reach this one?" without privileges, by reading
// /proc — so the app can ask the question without being able to change the answer. It prints
// `reachable: <address>` or `unreachable: <code>`, where the code is `no-listener` or
// `no-client`; the sentence shown to a person is written where it is shown, not here.
//
// Exit codes:  0 all actions succeeded
//              1 at least one action failed
//              2 bad arguments
//              3 needs privileges, and was not run with --dry-run
//
// ── How this gets privileges on Linux ────────────────────────────────────────────
//
// Through pkexec, under the polkit action org.artmoon.input-service.reconcile — see
// service/org.artmoon.input-service.policy. That action grants the active local session the
// right to run this program with no authentication, so the single approval the user gives is
// the one that placed this helper and that policy, not one per device toggle.
//
// A setuid binary is ruled out, and the reason is in this file: `usbip` is spawned by bare
// name through execvp, which resolves it through PATH. As a setuid-root program that is a
// root hole by construction — put a program called `usbip` in a directory earlier in PATH and
// it runs as root. The PATH fix in main() below closes that regardless, but polkit gives the
// same privilege with session scoping, an audit trail, and no setuid binary on the machine.
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
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <cerrno>
#  include <dirent.h>
#  include <fcntl.h>
#  include <strings.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {

// Where the kernel lists the devices this exporter currently offers. Absent directory
// means the usbip-host module is not loaded, which means nothing is exported — the
// normal state, and NOT an error. Same source of truth as UsbIpDevices::refresh().
const char *kUsbIpHostDriver = "/sys/bus/usb/drivers/usbip-host";

// The usbip ArtMoon ships, if the user's copy has placed it. It gets a directory of its own
// because it is two files and not one: usbip links against libusbip.so.0, which travels
// beside it and which it finds through the $ORIGIN rpath the bundle gives it. Which copy
// actually runs is decided in usbipProgram().
const char *kUsbipBundledPath = "/usr/libexec/artmoon-usbip/usbip";

#ifndef _WIN32
// The daemon, staged beside the client by the app's one-time setup.
//
// Binding a device makes it *offerable*; a listener has to be on 3240 before any other machine
// can attach it. On Windows that listener arrives with the usbipd-win installer, which is why
// the Windows half never had to think about it. On Linux there is nothing: the distro package
// may not be installed at all, and its unit ships DISABLED even when it is. Measured on the
// exporter 2026-10-04 — usbipd.service `disabled` and `inactive`, nothing on 3240, and the other
// machine could not connect. So we carry the daemon and we make it start, and we make it start
// AGAIN after a reboot, which is the part a one-shot command would have missed.
const char *kUsbipdBundledPath = "/usr/libexec/artmoon-usbip/usbipd";
// Our own unit, deliberately NOT named usbipd.service: if the user later installs the distro
// package, pacman owns that path and a file of ours sitting on it would be a conflict. When the
// distro's unit IS there we use it instead of installing ours — see ensureDaemon().
const char *kUsbipdUnitName = "artmoon-usbipd";
const char *kUsbipdUnitPath = "/etc/systemd/system/artmoon-usbipd.service";
// The distro's, if this machine has the usbip package.
const char *kDistroUsbipdUnit = "usbipd.service";

/*
 * Everything one-time setup places, and where it goes.
 *
 * This table lives here rather than being handed in by the app, and that is the whole point:
 * the caller supplies only a directory to read from. A destination chosen by whoever invokes a
 * program that runs as root is a destination an attacker gets to choose, and the app is the
 * unprivileged side of this boundary. The app used to name each destination in its own pkexec
 * command line; it no longer names one at all.
 *
 * The mode is applied explicitly rather than left to the source file's, so what lands is what
 * this table says regardless of what the staging copy looks like by the time we read it.
 */
struct Placement {
    const char *staged;
    const char *destination;
    unsigned mode;
};

const Placement kPlacements[] = {
    { "artmoon-input-service",
      "/usr/libexec/artmoon-input-service",                            0755 },
    { "org.artmoon.input-service.policy",
      "/usr/share/polkit-1/actions/org.artmoon.input-service.policy",  0644 },
    { "usbip",           "/usr/libexec/artmoon-usbip/usbip",           0755 },
    { "libusbip.so.0",   "/usr/libexec/artmoon-usbip/libusbip.so.0",   0644 },
    { "usbipd",          "/usr/libexec/artmoon-usbip/usbipd",          0755 },
};
#endif

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

#ifndef _WIN32
// Which usbip to run, as a path rather than a bare name.
//
// The host's own first. A distro's usbip is built with the kernel that distro ships, and this
// helper runs on machines whose kernel we have never seen — so where there is a matched pair,
// that pair is the better tool. Ours is the fallback, and it is here for the one case this
// bundle exists for: a machine with no usbip on it at all.
//
// The bare name at the end is for a distro that keeps it somewhere other than /usr/bin. It is
// still not the caller's choice: PATH is set to a literal list in main(), before anything runs.
std::string usbipProgram()
{
    if (access("/usr/bin/usbip", X_OK) == 0) {
        return "/usr/bin/usbip";
    }
    if (access(kUsbipBundledPath, X_OK) == 0) {
        return kUsbipBundledPath;
    }
    return "usbip";
}
#endif

std::vector<std::string> bindCommand(const std::string &busid)
{
#ifdef _WIN32
    return { "usbipd", "bind", "--busid", busid };
#else
    return { usbipProgram(), "bind", "-b", busid };
#endif
}

std::vector<std::string> unbindCommand(const std::string &busid)
{
#ifdef _WIN32
    return { "usbipd", "unbind", "--busid", busid };
#else
    return { usbipProgram(), "unbind", "-b", busid };
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

// ── the listener ─────────────────────────────────────────────────────────────────
//
// A bound device is still invisible to the other machine until something is listening on 3240.
// Everything in this block exists so that a user who has never installed anything, on a machine
// that has never heard of USB/IP, ends up with a listener — and still has one after a reboot.

// Read straight out of /proc/net/tcp rather than by running `ss`: this helper runs as root on
// machines whose userland we have never seen, and `ss` is not guaranteed to be installed, while
// /proc always is. Port 3240 is 0x0CA8 and LISTEN is 0x0A in that file's own hex, and the local
// address is the field before the colon. Unprivileged callers can read this too, which is what
// lets `status` answer the question without being able to change anything.
bool daemonIsListening()
{
    const char *const tables[] = { "/proc/net/tcp", "/proc/net/tcp6" };
    for (const char *table : tables) {
        FILE *file = fopen(table, "r");
        if (!file) {
            continue;
        }
        char line[512];
        bool header = true;
        bool found = false;
        while (!found && fgets(line, sizeof(line), file)) {
            if (header) {
                header = false;
                continue;
            }
            char local[128] = { 0 };
            char remote[128] = { 0 };
            char state[16] = { 0 };
            if (sscanf(line, " %*d: %127s %127s %15s", local, remote, state) != 3) {
                continue;
            }
            if (strcmp(state, "0A") != 0) {     // 0A == LISTEN
                continue;
            }
            const char *colon = strrchr(local, ':');
            if (colon && strcasecmp(colon + 1, "0CA8") == 0) {
                found = true;
            }
        }
        fclose(file);
        if (found) {
            return true;
        }
    }
    return false;
}

bool systemdUnitExists(const char *unit)
{
    // `cat` rather than `list-unit-files`: it exits non-zero for a unit that is not there, which
    // is the question being asked, and it does not depend on how the pattern is matched.
    const auto result = runProcess({ "systemctl", "cat", unit });
    return result.exitCode == 0;
}

// Write the unit that runs the daemon we placed.
bool writeUsbipdUnit()
{
    FILE *file = fopen(kUsbipdUnitPath, "w");
    if (!file) {
        return false;
    }
    const int written = fprintf(file,
                                "[Unit]\n"
                                "Description=ArtMoon USB/IP device sharing service\n"
                                "Documentation=man:usbipd(8)\n"
                                "After=network.target\n"
                                "\n"
                                "[Service]\n"
                                "ExecStart=%s\n"
                                "Restart=on-failure\n"
                                "\n"
                                "[Install]\n"
                                "WantedBy=multi-user.target\n",
                                kUsbipdBundledPath);
    fclose(file);
    if (written <= 0) {
        return false;
    }
    chmod(kUsbipdUnitPath, 0644);
    return true;
}

// Bring the listener up, and leave it coming up on its own after a reboot.
bool ensureDaemon()
{
    const char *unit = kUsbipdUnitName;

    // The distro's unit first, when the machine has one. It points at the distro's own daemon,
    // built against that machine's kernel, and it is what the user's own tooling expects — so
    // leaning on it is better than competing with it. Only when there is nothing to lean on do
    // we install a unit of our own.
    if (systemdUnitExists(kDistroUsbipdUnit)) {
        unit = "usbipd";
    } else {
        if (access(kUsbipdBundledPath, X_OK) != 0) {
            return false;               // nothing staged to run
        }
        if (!writeUsbipdUnit()) {
            return false;
        }
        runProcess({ "systemctl", "daemon-reload" });
    }

    // `enable --now` and not either half alone. `start` leaves every boot after this one with no
    // listener; `enable` alone leaves this boot without one. Both together is the only form that
    // means "and it still works after a restart".
    runProcess({ "systemctl", "enable", "--now", unit });

    // Then wait for the thing we are actually asking about.
    //
    // `enable --now` returns the moment the unit is *started*, which is not the moment the daemon
    // has *bound its port*. Checking once, right here, read a socket that was a moment away as a
    // failure — and the caller then told the user "no other machine can reach this one" while it
    // could, a second later. Measured on the exporter 2026-10-04: the warning printed and the
    // listener was up by the time the next command ran. A false negative here is worse than a
    // slow answer, because it tells someone their machine is unreachable when it is not.
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (daemonIsListening()) {
            return true;
        }
        usleep(100000);                     // 100 ms a turn, so two seconds at the outside
    }
    return false;
}

// ── reachability ────────────────────────────────────────────────────────────────
//
// A listener is only half of it. On a machine that runs a firewall the port is still shut from
// the outside, and the other machine's attach fails with a connection error that says nothing
// about why. Measured on the exporter 2026-10-04: `systemctl enable --now usbipd` alone still
// left Test-NetConnection false from the far end until a rule was added by hand.
//
// So this opens the port for exactly one machine, and only ever one — the client we are already
// streaming to. Never a subnet, never "anywhere". If we cannot name that machine we open
// nothing at all; see streamingPeer().
//
// And it closes the port again when that machine stops. A rule that outlives its session is a
// port standing open for a machine that finished with us hours ago — and one more of them every
// time sharing is toggled. See the firewall half of reconcile().

// The ports a client connects TO when it is in a session with us. Taken from the product's own
// address definitions rather than chosen here. The video and audio ports are UDP and so never
// appear in the tables below; only the control and RTSP ports can match.
//
// 47984 and 47989 are deliberately NOT in this list, even though they are the product's own HTTPS
// and HTTP ports. ArtMoon connects to both of them to draw the host and its app list — a glance,
// not a session — and it opens and closes that connection every couple of seconds for as long as
// the app is open. Counting a glance here made it read as "a stream just started": the helper
// bound the device, the poll closed, the next read released it, and the export flapped on and off
// underneath the user. A real session is seen on 48010 — measured live on the exporter 2026-10-04,
// where a stream held an ESTABLISHED connection to it — and 48002 is kept as the other
// session-time control port. So: only ports a session holds belong here, never ports a poll opens.
const int kStreamingPorts[] = { 48002, 48010 };

// The marker our own rules carry, so a rule of the user's is never mistaken for one of ours.
const char *kRuleMarker = "artmoon-device-sharing";

// firewalld's rich rules have no comment field, so on that side there is nothing on the face of
// a rule that says it is ours. Both consequences are handled below, and both are deliberate:
// the rule we add carries a timeout, so it cannot outlive the session that needed it even if
// nothing ever comes back to tidy up; and removal only ever happens for an address we can name,
// by exact text, so a rule written by hand can never be caught by a pattern.
const char *kFirewalldRuleFormat =
    "rule family=\"ipv4\" source address=\"%s\" port port=\"3240\" protocol=\"tcp\" accept";

// Twelve hours, refreshed every time sharing is toggled. Long enough that a session in progress
// never loses its rule underneath it, short enough that a port opened for a machine that walked
// away does not stay open for the rest of the week.
const int kFirewalldRuleSeconds = 12 * 60 * 60;

std::string firewalldRuleFor(const std::string &address)
{
    char buffer[512];
    snprintf(buffer, sizeof(buffer), kFirewalldRuleFormat, address.c_str());
    return std::string(buffer);
}

// /proc prints each 32-bit word in host order, so an IPv4 address's bytes come out reversed:
// 10.6.0.3 is 0x0A060003 on the wire and "0300060A" in the file.
std::string hexWordToAddress(const std::string &word)
{
    const unsigned long value = strtoul(word.c_str(), nullptr, 16);
    char text[24];
    snprintf(text, sizeof(text), "%lu.%lu.%lu.%lu",
             (value >> 0) & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff, (value >> 24) & 0xff);
    return std::string(text);
}

// The remote field of a /proc/net/tcp{,6} line as a dotted quad, or "" if it is not one we can
// use. IPv6 in its v4-mapped form is handled because that is what a dual-stack listener reports
// for a plain IPv4 client; anything genuinely IPv6 is left alone rather than guessed at.
std::string remoteAddressOf(const std::string &table, const std::string &remote)
{
    const std::string::size_type colon = remote.rfind(':');
    if (colon == std::string::npos) {
        return std::string();
    }
    const std::string host = remote.substr(0, colon);
    if (table == "/proc/net/tcp") {
        return host.size() == 8 ? hexWordToAddress(host) : std::string();
    }
    // 32 hex digits, four words: ::ffff:a.b.c.d is zeros, zeros, 0000FFFF, then the address.
    if (host.size() != 32 || host.compare(16, 8, "0000FFFF") != 0) {
        return std::string();
    }
    for (int i = 0; i < 16; ++i) {
        if (host[i] != '0') {
            return std::string();
        }
    }
    return hexWordToAddress(host.substr(24, 8));
}

struct Peer {
    bool found = false;
    std::string address;
};

// The machine we are actually serving: the far end of an established connection to one of our
// streaming ports. Only a client that is in a session has one, which is what makes this the
// narrow signal worth having — it names a machine that is genuinely connected, rather than
// anyone who happens to share a network with us.
Peer streamingPeer()
{
    Peer peer;
    const char *const tables[] = { "/proc/net/tcp", "/proc/net/tcp6" };
    for (const char *table : tables) {
        FILE *file = fopen(table, "r");
        if (!file) {
            continue;
        }
        char line[512];
        bool header = true;
        while (!peer.found && fgets(line, sizeof(line), file)) {
            if (header) {
                header = false;
                continue;
            }
            char local[128] = { 0 };
            char remote[128] = { 0 };
            char state[16] = { 0 };
            if (sscanf(line, " %*d: %127s %127s %15s", local, remote, state) != 3) {
                continue;
            }
            if (strcmp(state, "01") != 0) {         // 01 == ESTABLISHED
                continue;
            }
            // Which end of the connection is the session? Either one, and the difference
            // matters:
            //
            //   - A machine streaming INTO us is a client we are serving. Our local port is
            //     the streaming port, and the far end is the address we open for.
            //   - A machine we are streaming TO is the host we are sitting at. The connection
            //     is ours and goes out, so it is the REMOTE port that is the streaming one —
            //     and the far end is still the address we open for, because that is the
            //     machine about to be handed the device.
            //
            // Only the first shape was recognised, and that was the bug: switch a device on
            // for a stream going out and nothing ever matched. No rule was opened, the import
            // at the far end was dropped in silence, and the client hung waiting on it.
            //
            // In both shapes the far end is the remote address — we are always the local end
            // of our own connections — so one lookup serves both.
            const char *localColon = strrchr(local, ':');
            const char *remoteColon = strrchr(remote, ':');
            if (!localColon || !remoteColon) {
                continue;
            }
            const unsigned long localPort = strtoul(localColon + 1, nullptr, 16);
            const unsigned long remotePort = strtoul(remoteColon + 1, nullptr, 16);

            bool streaming = false;
            for (const int candidate : kStreamingPorts) {
                const unsigned long port = static_cast<unsigned long>(candidate);
                if (localPort == port || remotePort == port) {
                    streaming = true;
                }
            }
            if (!streaming) {
                continue;
            }
            const std::string address = remoteAddressOf(table, remote);
            // Loopback is not another machine, and a rule for it would do nothing while looking
            // like it had done something.
            if (address.empty() || address.compare(0, 4, "127.") == 0) {
                continue;
            }
            peer.found = true;
            peer.address = address;
        }
        fclose(file);
        if (peer.found) {
            break;
        }
    }
    return peer;
}

enum class Firewall { None, Ufw, Firewalld };

bool binaryExists(const char *path)
{
    return access(path, X_OK) == 0;
}

// Only two backends, and deliberately. ufw and firewalld both own their rules and both can be
// told about a single machine. Anything else is left alone rather than guessed at with raw nft
// or iptables, where a rule we add is a rule the user cannot find again — and where being
// slightly wrong means opening more than we meant to.
Firewall detectFirewall()
{
    if (binaryExists("/usr/sbin/ufw") || binaryExists("/sbin/ufw")) {
        const auto status = runProcess({ "ufw", "status" });
        return status.output.find("Status: active") != std::string::npos ? Firewall::Ufw
                                                                        : Firewall::None;
    }
    if (binaryExists("/usr/bin/firewall-cmd") || binaryExists("/usr/sbin/firewall-cmd")) {
        const auto state = runProcess({ "firewall-cmd", "--state" });
        return state.output.find("running") != std::string::npos ? Firewall::Firewalld
                                                                 : Firewall::None;
    }
    return Firewall::None;
}

bool allowPortFor(const std::string &address, Firewall firewall)
{
    if (firewall == Firewall::Ufw) {
        return runProcess({ "ufw", "allow", "from", address, "to", "any", "port", "3240",
                            "proto", "tcp", "comment", kRuleMarker }).exitCode == 0;
    }
    if (firewall == Firewall::Firewalld) {
        // Runtime rather than permanent, and with a lifetime on it. Permanent would mean a rule
        // that survives a reboot into a machine where the machine it names may be long gone, and
        // that nothing here can safely find again to remove. Runtime rules take effect at once,
        // so there is deliberately no --reload: reloading is the very thing that would drop it.
        return runProcess({ "firewall-cmd", "--add-rich-rule=" + firewalldRuleFor(address),
                            "--timeout=" + std::to_string(kFirewalldRuleSeconds) }).exitCode == 0;
    }
    return true;        // nothing is filtering, so the listener is already the whole story
}

// Take back what was opened for a machine we are no longer serving.
//
// Without this the port slowly opens to every client that has ever connected — each one its own
// rule, none of them visible unless the user goes looking for them.
//
// An empty `keep` means take back everything we wrote, which is what a machine that has stopped
// sharing, or is sharing but serving nobody, should end up with. On ufw that is exact, because
// our rules carry a marker and a rule the user wrote themselves does not. firewalld has no such
// marker to carry, so there the same call does nothing rather than guess — see the arm below —
// and the timeout on the rule is what closes that case instead.
void revokeStaleRules(const std::string &keep, Firewall firewall)
{
    if (firewall == Firewall::Ufw) {
        const auto status = runProcess({ "ufw", "status", "verbose" });
        if (status.exitCode != 0) {
            return;
        }
        std::istringstream lines(status.output);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.find(kRuleMarker) == std::string::npos || line.find("3240") == std::string::npos) {
                continue;
            }
            // "3240/tcp   ALLOW IN   10.6.0.3   # artmoon-device-sharing" — the address is the
            // last field that looks like one, and nothing after the comment marker counts.
            std::istringstream fields(line);
            std::string token;
            std::string address;
            while (fields >> token) {
                if (token == "#") {
                    break;
                }
                if (token.find('.') != std::string::npos) {
                    address = token;
                }
            }
            if (address.empty() || address == keep) {
                continue;
            }
            runProcess({ "ufw", "--force", "delete", "allow", "from", address, "to", "any",
                         "port", "3240", "proto", "tcp" });
        }
        return;
    }

    if (firewall == Firewall::Firewalld) {
        // One exact string: the rule this program generates for the address being replaced, and
        // nothing else. The version this replaced matched on "port 3240, has a source address"
        // and would therefore have deleted a rule the user added by hand for their own purposes
        // — a permission they granted, removed by a program that had no business touching it.
        //
        // With no address to keep there is nothing we can name, so nothing is removed. That case
        // is not left open: the rule carries a timeout and closes itself.
        if (keep.empty()) {
            return;
        }
        runProcess({ "firewall-cmd", "--remove-rich-rule=" + firewalldRuleFor(keep) });
    }
}
#endif

} // namespace

#ifndef _WIN32
// ── one-time setup ───────────────────────────────────────────────────────────────
//
// Placing the files is a privileged operation, and this is why it lives here instead of in the
// app. The app used to run one `pkexec install` per file: five files, five authorisations, five
// password dialogs, for one logical act — "set this up". Measured on the exporter 2026-10-04:
// exactly five prompts, and every one of them a generic exec authorisation, because `install`
// is not this program and so the action below could not cover it.
//
// The fix is not to make the dialogs cheaper. It is to make there be one, by having the single
// privileged call be this program, which already knows every destination and can place all
// five in one run.
//
// This does mean the first run executes a staged copy, from a directory the calling user owns,
// as root. That is the same authority the current design already grants one step later: what
// gets installed is a binary the permission rule then lets run as root *silently*, for the rest
// of the machine's life. The user is approving their own payload either way — the difference is
// that now they approve it once, and can see that the thing running is the thing being installed.

// The uid of whoever asked for this to happen.
//
// It is NOT getuid(). pkexec does not keep the caller's real uid and run us with an effective
// one — it switches fully to root, so getuid() here is 0 and the person who typed the password
// is invisible to us. That mistake is why the first run of this feature refused its own payload
// on the exporter, after a successful prompt, with "that is not a directory this app staged".
//
// pkexec puts the caller's uid in PKEXEC_UID, built by pkexec itself into a minimal environment
// it constructs from scratch (pkexec.c saves a short whitelist of LC_* and SHELL, then appends
// PKEXEC_UID), so the caller cannot set it — which is what makes it safe to trust, and better
// than taking the uid as an argument, where any user could name any other user's directory.
//
// With no pkexec in the picture — root running us directly, which is what the build gate does —
// there is no caller to distinguish from the process, and getuid() is the honest answer.
uid_t invokingUid()
{
    const char *pkexec = std::getenv("PKEXEC_UID");
    if (pkexec != nullptr && *pkexec != '\0') {
        char *end = nullptr;
        const long value = std::strtol(pkexec, &end, 10);
        if (end != nullptr && *end == '\0' && value >= 0)
            return static_cast<uid_t>(value);
    }
    return getuid();
}

// Directories we will read a payload from. The app stages into a 0700 directory it creates under
// XDG_RUNTIME_DIR, or under /tmp when that is unset. Anything else is refused rather than
// trusted — and the ownership check is what stops one user naming another user's directory.
bool stagingDirectoryIsOurs(const std::string &dir)
{
    if (dir.empty() || dir[0] != '/') return false;
    if (dir.find("..") != std::string::npos) return false;

    const std::string leaf = dir.substr(dir.find_last_of('/') + 1);
    const std::string prefix = "artmoon-install-";
    if (leaf.size() < prefix.size() || leaf.compare(0, prefix.size(), prefix) != 0) return false;

    struct stat st {};
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    return st.st_uid == invokingUid();
}

bool makeParentDirectories(const std::string &path)
{
    std::string sofar;
    size_t pos = path.find('/');
    while (pos != std::string::npos && pos + 1 < path.size()) {
        pos = path.find('/', pos + 1);
        if (pos == std::string::npos) break;
        sofar = path.substr(0, pos);
        if (sofar.empty()) continue;

        struct stat st {};
        if (stat(sofar.c_str(), &st) == 0) {
            if (!S_ISDIR(st.st_mode)) return false;
            continue;
        }
        if (mkdir(sofar.c_str(), 0755) != 0 && errno != EEXIST) return false;
        if (chown(sofar.c_str(), 0, 0) != 0) return false;
    }
    return true;
}

// Copies one staged file into place.
//
// Written beside the destination and renamed over it, so the file never exists at its system
// path with the wrong content, mode or owner — not even for an instant. That property was
// claimed by the app's comment when it shelled out to install(1); doing the copy here had to
// keep it, and install(1) is not available to us now that we do not spawn one.
bool placeFile(const std::string &from, const std::string &to, unsigned mode)
{
    const int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        std::cerr << "artmoon-input-service: cannot read " << from << ": "
                  << std::strerror(errno) << "\n";
        return false;
    }

    struct stat src {};
    if (fstat(in, &src) != 0 || !S_ISREG(src.st_mode) || src.st_size == 0) {
        std::cerr << "artmoon-input-service: " << from << " is not a usable file\n";
        close(in);
        return false;
    }

    const std::string temp = to + ".artmoon-new";
    const int out = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (out < 0) {
        std::cerr << "artmoon-input-service: cannot write " << temp << ": "
                  << std::strerror(errno) << "\n";
        close(in);
        return false;
    }

    bool ok = true;
    char buffer[65536];
    for (;;) {
        const ssize_t got = read(in, buffer, sizeof(buffer));
        if (got == 0) break;
        if (got < 0) { ok = false; break; }

        ssize_t written = 0;
        while (written < got) {
            const ssize_t put = write(out, buffer + written, static_cast<size_t>(got - written));
            if (put <= 0) { ok = false; break; }
            written += put;
        }
        if (!ok) break;
    }
    close(in);

    // Explicit, and after the write: neither the umask nor the staging copy's own permissions
    // get a say in what lands.
    if (ok) ok = fchmod(out, static_cast<mode_t>(mode)) == 0;
    if (ok) ok = fchown(out, 0, 0) == 0;
    if (ok) ok = fsync(out) == 0;
    close(out);

    if (!ok) {
        std::cerr << "artmoon-input-service: could not write " << to << ": "
                  << std::strerror(errno) << "\n";
        unlink(temp.c_str());
        return false;
    }
    if (rename(temp.c_str(), to.c_str()) != 0) {
        std::cerr << "artmoon-input-service: could not place " << to << ": "
                  << std::strerror(errno) << "\n";
        unlink(temp.c_str());
        return false;
    }
    return true;
}

int installFrom(const std::string &dir)
{
    if (!stagingDirectoryIsOurs(dir)) {
        std::cerr << "artmoon-input-service: refusing to install from '" << dir
                  << "' — that is not a directory this app staged\n";
        return 2;
    }

    int failures = 0;
    for (const Placement &placement : kPlacements) {
        const std::string source = dir + "/" + placement.staged;

        struct stat st {};
        if (stat(source.c_str(), &st) != 0) {
            std::cerr << "artmoon-input-service: " << placement.staged
                      << " was not staged\n";
            ++failures;
            continue;
        }
        if (!makeParentDirectories(placement.destination)) {
            std::cerr << "artmoon-input-service: cannot create the directory for "
                      << placement.destination << "\n";
            ++failures;
            continue;
        }
        if (!placeFile(source, placement.destination, placement.mode)) {
            ++failures;
            continue;
        }
        std::cout << placement.staged << " installed\n";
    }
    return failures == 0 ? 0 : 1;
}
#endif

int main(int argc, char **argv)
{
    // ── Fix what execvp will find, before anything can run ────────────────────────────
    //
    // This program spawns `usbip` and `modprobe` by bare name, and execvp resolves a bare
    // name through PATH. It runs as root and, on Linux, is started by an unprivileged
    // process — so without this line, what actually executes would be decided by the
    // environment it was handed, and a caller who can influence PATH would be choosing the
    // program that runs as root.
    //
    // pkexec already sanitises PATH, so this is not closing a live hole; it is refusing to
    // depend on the caller for it. The distinction is the difference between a fact about
    // this file and a promise about pkexec — and it is what would still be true if anyone
    // ever revisited the setuid question.
    //
    // setenv(..., 1) so it applies whether or not the caller set a PATH, and it sits above
    // the argument parsing so no path through this program can precede it.
    setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);

#ifndef _WIN32
    // Our own usbip needs nothing set here, and that is deliberate.
    //
    // We compile it now, and the bundle gives it an $ORIGIN rpath (scripts/build-appimage.sh,
    // whose gate then proves it by running the copy with a CLEAN environment), so it finds the
    // libusbip.so.0 sitting beside it without being told.
    //
    // The variable this used to set was worse than redundant. LD_LIBRARY_PATH applies to every
    // child, so on a machine that HAS its own usbip - the one usbipProgram() prefers - it put
    // our libusbip on that host tool's search path, which is the opposite of the "a machine
    // running its own usbip keeps its own library resolution exactly as it was" that this block
    // claimed. A property of the file we install is the right place for this; the environment of
    // a root process is not.
#endif

    std::vector<std::string> args(argv + 1, argv + argc);

    bool dryRun = false;
    std::vector<std::string> wantList;
    std::vector<std::string> localList;
    std::string verb;
    std::string fromDirectory;

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
        } else if (arg == "--from") {
            if (i + 1 >= args.size()) {
                std::cerr << "artmoon-input-service: --from needs a directory\n";
                return 2;
            }
            fromDirectory = args[++i];
        } else if (verb.empty()) {
            verb = arg;
        } else {
            std::cerr << "artmoon-input-service: unexpected argument '" << arg << "'\n";
            return 2;
        }
    }

    if (verb != "reconcile" && verb != "status" && verb != "install") {
        std::cerr << "usage: artmoon-input-service reconcile [--want <busid>]... "
                     "[--local <busid>]... [--dry-run]\n"
                     "       artmoon-input-service status\n"
                     "       artmoon-input-service install --from <staged-directory>\n";
        return 2;
    }

    // Each verb gets the arguments it takes and no others, so a flag that belongs to one cannot
    // quietly do nothing on another.
    if (verb == "install" && (!wantList.empty() || !localList.empty() || dryRun)) {
        std::cerr << "artmoon-input-service: install takes only --from\n";
        return 2;
    }
    if (verb != "install" && !fromDirectory.empty()) {
        std::cerr << "artmoon-input-service: --from belongs to install\n";
        return 2;
    }
    if (verb == "install" && fromDirectory.empty()) {
        std::cerr << "artmoon-input-service: install needs --from <staged-directory>\n";
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
#ifndef _WIN32
        // Answered unprivileged, because it only reads /proc — which is what lets the app ask
        // "can another machine reach me?" without being able to change the answer.
        //
        // It cannot read the firewall without root, so it reports the two things it can prove
        // and leaves the rule itself to `reconcile`, which does run as root. The wording is a
        // code and an address rather than a sentence: the sentence belongs on the screen that
        // shows it, where it can be translated and can name the remedy.
        const Peer peer = streamingPeer();

        /*
         * The peer is reported BEFORE the listener, and that order is load-bearing.
         *
         * A stream can start while nothing on this machine is bound yet — that is the ordinary
         * case now, because a tick binds nothing and the bind waits for the session. With nothing
         * bound there is no daemon, so asking about the listener first answered `no-listener` and
         * never mentioned the peer at all: the app was told there was nobody streaming at the one
         * moment somebody was, so it never bound, so there was never a listener. A deadlock that
         * looked exactly like the bug it was meant to fix.
         *
         * "Is anyone streaming at us?" and "are we serving the export?" are different questions,
         * and this verb has to answer the first one even when the second is no.
         */
        if (peer.found) {
            std::cout << "reachable: " << peer.address << "\n";
        } else if (!daemonIsListening()) {
            std::cout << "unreachable: no-listener\n";
        } else {
            std::cout << "unreachable: no-client\n";
        }
#endif
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

    // One privileged run places every file, which is what makes the first setup a single
    // approval rather than one per file. Placed here, above the reconcile work, because it
    // shares the root gate and nothing else with it.
    if (verb == "install") {
        return installFrom(fromDirectory);
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

#ifndef _WIN32
    // Both of the things below are keyed on being wanted, not on there being anything left to
    // bind — and that is why they sit above the shortcut that follows. An empty bind list means
    // there is nothing to do to the *devices*; it says nothing at all about the listener or the
    // firewall. The case that made this obvious: a user toggles sharing off. Nothing is bound
    // any more, so the plan is empty, and the firewall was never told — leaving a port open for a
    // machine they had just finished with. The check is only ever about bindings.

    // The listener, whenever this machine is being asked to offer anything at all.
    //
    // Keyed on the WANT list and not on the bind list, and the difference matters: after a
    // reboot the bindings are gone, so a reconcile will have binds to do and this would run
    // anyway — but a user who has toggled sharing on, had the daemon stopped underneath them,
    // and toggled nothing since, has nothing left to bind. What they asked for is sharing, so
    // sharing is what gets made true.
    if (!wantList.empty()) {
        if (!ensureDaemon()) {
            std::cerr << "the sharing service could not be started, so nothing switched on here "
                         "can be reached. Restart ArtMoon to try again.\n";
            // Deliberately not fatal: the bind below is still worth doing, and the app reports
            // the listener separately through `status`. Failing here would turn a reachability
            // problem into a feature that looks entirely broken.
        }
    }

    // And the other half of reachability, for a machine that filters. A listener being up is not
    // the same as the port being open, and the exporter showed exactly that: reachable from the
    // far end only after a rule, with the daemon already running.
    //
    // This runs whether or not anything is wanted, and that is the point of it rather than an
    // accident of where it sits. A rule exists to serve a live session. With no session there is
    // nothing to serve, so it goes — including when the user has just turned sharing off, which
    // is the moment they most expect the port to stop being open. Leaving the rule in place
    // would mean a port standing open for a machine that stopped talking to us some time ago,
    // and one more of those every time sharing is toggled.
    const Firewall firewall = detectFirewall();
    if (firewall == Firewall::None) {
        // Nothing is filtering, so the listener above is already the whole story.
    } else {
        const Peer peer = wantList.empty() ? Peer() : streamingPeer();
        if (peer.found) {
            revokeStaleRules(peer.address, firewall);
            if (!allowPortFor(peer.address, firewall)) {
                std::cerr << "could not open the connection for " << peer.address << "\n";
            }
        } else {
            // We will not open a port for a machine we cannot name, and "anywhere" is a different
            // feature with a different risk. An empty `keep` means take back every rule we wrote
            // and leave the user's own alone — which is the right end state for both cases here:
            // nothing is being shared, or nothing is connected to be served.
            revokeStaleRules(std::string(), firewall);
            if (!wantList.empty()) {
                std::cerr << "waiting for the other machine — the connection opens by itself "
                             "when a stream to this PC starts.\n";
            }
        }
    }
#endif

    // Only the bindings are settled by this point — the listener and the firewall above are done.
    // "Already in the state you asked for" is true of the devices, and only of the devices.
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
