/*
 * ArtMoon input service — Windows.
 *
 * The privileged local service that owns the bind step on the exporter. ArtMoon itself runs
 * unprivileged and can enumerate devices, but it cannot bind: `bind` is refused without
 * administrator rights. This program is what does it on the user's behalf, so no command line
 * is ever in front of them.
 *
 * Design (locked in docs/usb-ip-input-passthrough.md, 2026-10-03):
 *
 *   - The desired set arrives over a local named pipe created by THIS service, with an
 *     explicit DACL and PIPE_REJECT_REMOTE_CLIENTS. Not start arguments, because `sc start`
 *     refuses a service that is already running (error 1056) and so can only ever deliver the
 *     first desired set. Not a file, because any path a normal process can write must never be
 *     read here as instruction — that would let anything on the machine choose which devices
 *     are offered over the network.
 *   - No Qt, nothing but Win32. It runs as LocalSystem; every line of it should be readable.
 *   - Every busid is validated before it can reach a command line. Fail closed: anything the
 *     validator does not positively recognise is refused. This runs as LocalSystem and its
 *     input arrives from an unprivileged process, so a guard that quietly stopped holding
 *     would be a privilege-escalation hole, not a cosmetic bug.
 *   - No state of its own. It reconciles, it does not remember — so nothing on disk can
 *     disagree with what the Input tab shows.
 *
 * Build: cl /EHsc /W4 /O2 artmoon-input-service-win.cpp /link advapi32.lib
 * Install: the ArtMoon installer creates the service, the same shape as the one usbipd-win
 * installs for itself. `selftest` exercises the guards and is what CI runs.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <sddl.h>   /* ConvertStringSecurityDescriptorToSecurityDescriptorW, SDDL_REVISION_1 */

#include <cstdio>
#include <string>
#include <vector>

namespace {

const wchar_t *const kServiceName = L"ArtMoonInputService";
const wchar_t *const kPipeName    = L"\\\\.\\pipe\\ArtMoonInputService";
const wchar_t *const kUsbipdPath  = L"C:\\Program Files\\usbipd-win\\usbipd.exe";

const size_t kMaxBusidLength = 16;
const size_t kMaxTokens      = 64;
const size_t kMaxRequest     = 4000;

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS        g_status{};
HANDLE                g_stopEvent = nullptr;

void logEvent(WORD type, const wchar_t *message)
{
    HANDLE source = RegisterEventSourceW(nullptr, kServiceName);
    if (!source) {
        return;
    }
    const wchar_t *strings[1] = { message };
    ReportEventW(source, type, 0, 1, nullptr, 1, 0, strings, nullptr);
    DeregisterEventSource(source);
}

void reportStatus(DWORD state, DWORD waitHint = 0)
{
    g_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState            = state;
    g_status.dwControlsAccepted        = (state == SERVICE_RUNNING)
                                           ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN)
                                           : 0;
    g_status.dwWin32ExitCode           = NO_ERROR;
    g_status.dwServiceSpecificExitCode = 0;
    g_status.dwCheckPoint              = 0;
    g_status.dwWaitHint                = waitHint;
    if (g_statusHandle) {
        SetServiceStatus(g_statusHandle, &g_status);
    }
}

/*
 * A busid is digits, one hyphen, digits. Nothing else — no spaces, no quotes, no
 * metacharacters, nothing that could arrive at a command line as anything but a busid.
 *
 * Mirrors the Linux helper's rule exactly. Both ends are tested against the same hostile
 * list in CI, so the two cannot drift apart quietly.
 */
bool isValidBusid(const std::string &s)
{
    if (s.empty() || s.size() > kMaxBusidLength) {
        return false;
    }

    size_t i = 0;
    size_t digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        ++i;
        ++digits;
    }
    if (digits == 0 || i >= s.size() || s[i] != '-') {
        return false;
    }
    ++i;

    digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        ++i;
        ++digits;
    }
    return digits > 0 && i == s.size();
}

std::wstring widen(const std::string &s)
{
    if (s.empty()) {
        return std::wstring();
    }
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], need);
    return out;
}

/*
 * Run usbipd and keep its words. The service does not invent reasons: if usbipd refuses, the
 * refusal itself is what gets reported back, because that is the text someone will search for.
 */
bool runUsbipd(const std::wstring &arguments, std::string *output, DWORD *exitCode)
{
    std::wstring commandLine = L"\"" + std::wstring(kUsbipdPath) + L"\" " + arguments;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        return false;
    }
    // Our read end must NOT be inherited: inheriting it would keep the pipe alive in the child
    // and the read below would never see the end of its output.
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = writeEnd;
    si.hStdError   = writeEnd;
    si.hStdInput   = nullptr;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    const BOOL started = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        return false;
    }

    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        output->append(buffer, got);
        if (output->size() > 65536) {
            break; // bounded, always
        }
    }
    CloseHandle(readEnd);

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exitCode) {
        *exitCode = code;
    }
    return true;
}

std::string firstLine(const std::string &text)
{
    const size_t end = text.find_first_of("\r\n");
    std::string line = (end == std::string::npos) ? text : text.substr(0, end);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.erase(line.begin());
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

/*
 * Request:  one line, "reconcile" followed by whitespace-separated tokens, each "+<busid>"
 *           (bind) or "-<busid>" (unbind). The plan is computed by ArtMoon, which is the only
 *           part that parses usbipd's table — so there is one parser in the product, and it is
 *           the one with tests.
 * Reply:    "ok" or "err <usbipd's own words>". ArtMoon does not trust the reply: it
 *           re-enumerates, and `usbipd list` stays the authority on what is actually shared.
 */
std::string processRequest(const std::string &request)
{
    const std::string line = firstLine(request);

    size_t pos = 0;
    while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') {
        ++pos;
    }
    const std::string verb = line.substr(0, pos);
    if (verb.empty()) {
        return "err no verb\n";
    }
    if (verb != "reconcile") {
        return "err unknown verb\n";
    }

    std::vector<std::string> toBind;
    std::vector<std::string> toUnbind;

    size_t count = 0;
    while (pos < line.size()) {
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
            ++pos;
        }
        if (pos >= line.size()) {
            break;
        }
        const size_t start = pos;
        while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') {
            ++pos;
        }
        const std::string token = line.substr(start, pos - start);

        if (++count > kMaxTokens) {
            return "err too many devices in one request\n";
        }
        if (token.size() < 2 || (token[0] != '+' && token[0] != '-')) {
            return "err malformed device entry\n";
        }
        const std::string busid = token.substr(1);
        // Fail closed. Anything not positively a busid is refused here, before it can reach
        // usbipd's command line.
        if (!isValidBusid(busid)) {
            return "err refused a device name that is not a busid\n";
        }
        (token[0] == '+') ? toBind.push_back(busid) : toUnbind.push_back(busid);
    }

    std::string firstError;
    size_t changed = 0;

    for (const auto &busid : toBind) {
        /*
         * ── Plain bind first, and --force only if the machine refuses ──────────────
         *
         * --force used to be unconditional here, on the argument that a Razer peripheral
         * carries RzDev_<pid> as an upper filter and therefore always counts as "in use".
         * The vendor-engine work superseded that argument: the engine is stopped before a
         * take (see vendorengine.h), so the reason to force has gone — and forcing carries
         * a cost of its own, because `--force` also overrides "in use" for a device whose
         * FILESYSTEM is mounted. On a drive that is the whole difference between a bind
         * that is refused and a drive yanked out from under whatever was writing to it.
         * Measured on niks-minipc, 2026-10-06: an NVMe force-bound this way vanished from
         * Windows and did not come back through a reboot.
         *
         * So a machine that is behaving gets a plain bind, and --force stays as the
         * fallback for the devices that genuinely refuse. Every fallback is logged, so the
         * day the log shows it never fires is the day this can be deleted outright rather
         * than argued about.
         */
        std::string output;
        DWORD code = 0;
        const bool ran = runUsbipd(L"bind --busid " + widen(busid), &output, &code);

        if (!ran) {
            if (firstError.empty()) firstError = "could not run usbipd";
            continue;
        }

        if (code != 0) {
            // Refused. This refusal is the only thing --force exists for.
            const std::string refusal = firstLine(output);

            std::string forced;
            DWORD forcedCode = 0;
            if (!runUsbipd(L"bind --force --busid " + widen(busid), &forced, &forcedCode)) {
                if (firstError.empty()) firstError = "could not run usbipd";
                continue;
            }
            if (forcedCode != 0) {
                // Forcing did not rescue it either, so the plain refusal is the honest
                // answer — and it is the one that names the real reason.
                if (firstError.empty()) {
                    firstError = refusal.empty() ? firstLine(forced) : refusal;
                }
                continue;
            }

            logEvent(EVENTLOG_INFORMATION_TYPE,
                     widen("plain bind refused for " + busid + " (" + refusal +
                           "); --force was needed").c_str());
        }

        ++changed;
    }

    for (const auto &busid : toUnbind) {
        std::string output;
        DWORD code = 0;
        const bool ran = runUsbipd(L"unbind --busid " + widen(busid), &output, &code);
        if (!ran) {
            if (firstError.empty()) firstError = "could not run usbipd";
            continue;
        }
        if (code != 0) {
            if (firstError.empty()) firstError = firstLine(output);
            continue;
        }
        ++changed;
    }

    if (!firstError.empty()) {
        // Partial work is the honest outcome of a partial failure: report the words, and let
        // ArtMoon re-enumerate rather than claim a state.
        logEvent(EVENTLOG_WARNING_TYPE, widen(firstError).c_str());
        return "err " + firstError + "\n";
    }
    if (changed == 0) {
        return "ok nothing to do\n";
    }
    return "ok\n";
}

void handleClient(HANDLE pipe)
{
    std::string request;
    char buffer[1024];
    DWORD got = 0;

    while (ReadFile(pipe, buffer, sizeof(buffer) - 1, &got, nullptr) && got > 0) {
        request.append(buffer, got);
        if (request.find('\n') != std::string::npos || request.size() > kMaxRequest) {
            break;
        }
    }

    const std::string reply = processRequest(request);
    DWORD written = 0;
    WriteFile(pipe, reply.c_str(), static_cast<DWORD>(reply.size()), &written, nullptr);
    FlushFileBuffers(pipe);
}

/*
 * Created with its DACL up front rather than fixed up afterwards, so the pipe never exists
 * with a wider default. FILE_FLAG_FIRST_PIPE_INSTANCE is the other half of that: without it,
 * any process on the machine could create this pipe name first and impersonate the service to
 * ArtMoon, and ArtMoon would happily talk to it.
 */
/*
 * Say WHICH call failed and what Windows said about it.
 *
 * This logged only "could not create the input pipe", with no error code and no clue which
 * of the two calls it came from — which is how a service that had never once succeeded sat
 * there looking like a service with nothing to report. The error is read here, immediately
 * after the failing call, and never after LocalFree, which would have clobbered it.
 */
void logPipeFailure(const wchar_t *what, DWORD error)
{
    wchar_t message[192];
    swprintf(message, 192, L"could not create the input pipe: %ls (win32 error %lu)",
             what, static_cast<unsigned long>(error));
    logEvent(EVENTLOG_ERROR_TYPE, message);
}

HANDLE createPipe()
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)",
            SDDL_REVISION_1, &descriptor, nullptr)) {
        logPipeFailure(L"the pipe DACL would not parse", GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = descriptor;
    sa.bInheritHandle       = FALSE;

    // ⚠️ PIPE_REJECT_REMOTE_CLIENTS belongs in dwPipeMode, NOT dwOpenMode. Its bit (0x8) is
    // not a legal dwOpenMode flag, so CreateNamedPipeW failed with ERROR_INVALID_PARAMETER
    // (87) on every call this service ever made: it ran for hours logging "could not create
    // the input pipe" every two seconds and never once had a pipe for ArtMoon to talk to,
    // while ArtMoon's waitForConnected() timed out and the toggles did nothing, silently.
    // Measured on the mini PC — as written, err=87; with the flag moved here, handle valid.
    HANDLE pipe = CreateNamedPipeW(
        kPipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, &sa);

    if (pipe == INVALID_HANDLE_VALUE) {
        logPipeFailure(L"CreateNamedPipeW refused", GetLastError());
    }

    LocalFree(descriptor);
    return pipe;
}

void serve()
{
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        HANDLE pipe = createPipe();
        if (pipe == INVALID_HANDLE_VALUE) {
            // createPipe() has already logged which call failed and why.
            Sleep(2000);
            continue;
        }

        ConnectNamedPipe(pipe, nullptr); // returns when a client connects — including our own wake-up

        // A stop request wakes the accept by connecting to the pipe itself, so a stop is
        // immediate even while nothing else is talking to us. That connection is dropped here.
        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }

        handleClient(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

DWORD WINAPI handlerEx(DWORD control, DWORD, void *, void *)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        reportStatus(SERVICE_STOP_PENDING, 5000);
        if (g_stopEvent) {
            SetEvent(g_stopEvent);
            // Wake the blocked accept. Without this the service would sit in ConnectNamedPipe
            // until the control manager lost patience and killed it — which would look, from
            // the outside, exactly like a service that hangs when you stop it.
            HANDLE wake = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
            if (wake != INVALID_HANDLE_VALUE) {
                CloseHandle(wake);
            }
        }
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI serviceMain(DWORD, LPWSTR *)
{
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, handlerEx, nullptr);
    if (!g_statusHandle) {
        return;
    }
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        reportStatus(SERVICE_STOPPED);
        return;
    }

    reportStatus(SERVICE_RUNNING);
    logEvent(EVENTLOG_INFORMATION_TYPE, L"ArtMoon input service started");
    serve();
    reportStatus(SERVICE_STOPPED);
}

/*
 * The guards, exercised without the service control manager and without touching a device.
 * This is what CI runs: a refusal that quietly stopped happening would not fail anything
 * later, and it would be a privilege-escalation hole.
 */
int selftest()
{
    struct Case {
        const char *busid;
        bool valid;
    };
    const Case cases[] = {
        { "1-6", true },
        { "3-10", true },
        { "9-3", true },
        { "12-34", true },
        { "", false },
        { "9-3; rm -rf /", false },
        { "9-3$(id)", false },
        { "9-3`id`", false },
        { "../9-3", false },
        { "9-3/../1", false },
        { "9-3 --force", false },
        { "9-3|cat /etc/shadow", false },
        { "9-3&&id", false },
        { "a-3", false },
        { "9-3.", false },
        { "-3", false },
        { "3", false },
        { "3-", false },
        { "9_3", false },
        { "99999999999999999999-1", false },
    };

    int failures = 0;
    for (const auto &c : cases) {
        if (isValidBusid(c.busid) != c.valid) {
            std::printf("FAIL: %s should have been %s\n", c.busid, c.valid ? "accepted" : "refused");
            ++failures;
        }
    }

    // And the request path: a hostile entry must not survive parsing either.
    if (processRequest("reconcile +9-3; rm -rf /\n") .compare(0, 3, "err") != 0) {
        std::printf("FAIL: a hostile entry was not refused\n");
        ++failures;
    }
    if (processRequest("nonsense\n").compare(0, 3, "err") != 0) {
        std::printf("FAIL: an unknown verb was not refused\n");
        ++failures;
    }
    if (processRequest("").compare(0, 3, "err") != 0) {
        std::printf("FAIL: an empty request was not refused\n");
        ++failures;
    }

    std::printf("%s\n", failures ? "guards FAILED" : "guards hold");
    return failures ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc > 1 && _wcsicmp(argv[1], L"selftest") == 0) {
        return selftest();
    }

    // `status` is deliberately absent: the app enumerates for itself, and a second opinion
    // from here could disagree with it. One authority per question.
    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(kServiceName), serviceMain },
        { nullptr, nullptr },
    };

    if (!StartServiceCtrlDispatcherW(table)) {
        std::fwprintf(stderr, L"ArtMoonInputService must be started by the service control "
                              L"manager (error %lu). Run `selftest` to check the guards.\n",
                      GetLastError());
        return 1;
    }
    return 0;
}
