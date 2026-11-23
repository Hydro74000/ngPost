// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// The tool supervisor (see ToolSupervisor.h). Plain POSIX, no Qt: it runs
// before any QCoreApplication, and its only job is to prove that the tool and
// all its descendants are gone before it says so.
//
//========================================================================

#include "tools/ToolSupervisor.h"

#if defined(__linux__) || defined(__APPLE__)

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <string>
#if defined(__linux__)
#include <sys/prctl.h>
#else
#include <sys/sysctl.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace
{

constexpr int kToController = 3;
constexpr int kFromController = 4;
constexpr int kTickMs = 50;
constexpr int kTermGraceMs = 2000;

volatile sig_atomic_t sStopSignal = 0;

extern "C" void onStopSignal(int)
{
    sStopSignal = 1;
}

//! A line to the controller. Its absence (EPIPE, closed) never stops the work.
void say(const std::string &line)
{
    const std::string text = line + "\n";
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(kToController, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        done += static_cast<std::size_t>(n);
    }
}

#if defined(__linux__)
std::string readSmallFile(const char *path)
{
    std::string text;
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return text;
    char buffer[512];
    ssize_t n = 0;
    while ((n = ::read(fd, buffer, sizeof buffer)) > 0) // flawfinder: ignore
        text.append(buffer, static_cast<std::size_t>(n));
    ::close(fd);
    while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
        text.pop_back();
    return text;
}

//! Field 22 of /proc/<pid>/stat: start time in clock ticks since boot. With the
//! boot id it names one process, whatever pid recycling does.
std::string startTime(pid_t pid)
{
    const std::string stat = readSmallFile(("/proc/" + std::to_string(pid) + "/stat").c_str());
    const std::size_t comm = stat.rfind(')');
    if (comm == std::string::npos)
        return {};
    // The fields after the command name, from field 3 (the state) on: the
    // start time, field 22, is the 20th of them.
    std::vector<std::string> fields;
    std::size_t pos = comm + 2;
    while (pos < stat.size()) {
        std::size_t end = stat.find(' ', pos);
        if (end == std::string::npos)
            end = stat.size();
        fields.push_back(stat.substr(pos, end - pos));
        pos = end + 1;
    }
    return fields.size() > 19 ? fields[19] : std::string();
}

//! A decimal pid at the start of @p text; -1 when there is none.
pid_t toPid(const char *text)
{
    char *end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || value <= 0)
        return -1;
    return static_cast<pid_t>(value);
}

pid_t parentOf(const char *pidDir)
{
    const std::string stat = readSmallFile((std::string("/proc/") + pidDir + "/stat").c_str());
    const std::size_t comm = stat.rfind(')');
    if (comm == std::string::npos)
        return -1;
    // ") S <ppid> ..."
    const std::size_t field = stat.find(' ', comm + 2);
    return field == std::string::npos ? -1 : toPid(stat.c_str() + field + 1);
}

#else
//! Start time of @p pid from the kernel, to the microsecond: with the boot
//! time it names one process.
std::string startTime(pid_t pid)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    struct kinfo_proc info = {};
    size_t size = sizeof info;
    if (::sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0)
        return {};
    const timeval &t = info.kp_proc.p_starttime;
    return std::to_string(t.tv_sec) + "." + std::to_string(t.tv_usec);
}

#endif

#if defined(__linux__)
//! Our direct children, adopted orphans included. /proc may miss one that
//! changes at the same time: the caller keeps looping until ECHILD.
std::vector<pid_t> children()
{
    std::vector<pid_t> found;
    DIR *proc = ::opendir("/proc");
    if (!proc)
        return found;
    const pid_t self = ::getpid();
    while (const dirent *entry = ::readdir(proc)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        const pid_t pid = toPid(entry->d_name);
        if (pid > 0 && parentOf(entry->d_name) == self)
            found.push_back(pid);
    }
    ::closedir(proc);
    return found;
}
#endif

//! The boot this process belongs to: with a pid and a start time, one process.
std::string bootIdentity()
{
#if defined(__linux__)
    return readSmallFile("/proc/sys/kernel/random/boot_id");
#else
    int mib[2] = { CTL_KERN, KERN_BOOTTIME };
    timeval boot = {};
    size_t size = sizeof boot;
    if (::sysctl(mib, 2, &boot, &size, nullptr, 0) != 0)
        return {};
    return std::to_string(boot.tv_sec) + "." + std::to_string(boot.tv_usec);
#endif
}

//! A pipe whose ends are not inherited by the tool.
bool makePipe(int fds[2])
{
#if defined(__linux__)
    return ::pipe2(fds, O_CLOEXEC) == 0;
#else
    if (::pipe(fds) != 0)
        return false;
    for (int i = 0; i < 2; ++i)
        ::fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

struct Tool
{
    pid_t pid = -1;        //!< the tool, our child, while not reaped
    pid_t group = -1;      //!< its process group, signalled only while the tool is alive
    int status = -1;       //!< -1 never started; 127 exec failed; exit code; 128 + signal
    bool launched = false; //!< forked once: never again
    bool started = false;  //!< exec succeeded
    int execPipe = -1;     //!< closed by a successful exec, or carries its errno
    pid_t launchedPid = -1;
};

#if !defined(__linux__)
//! macOS: whether the tool has ended, seen without reaping it. Unreaped, its
//! zombie keeps its pid, hence its group id: the group may be signalled
//! without ever reaching a group that took the same number since.
bool leaderEnded(Tool &tool)
{
    if (tool.pid < 0)
        return true; // never forked
    siginfo_t info = {};
    if (::waitid(P_PID, static_cast<id_t>(tool.pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0
        || info.si_pid != tool.pid)
        return false;
    if (tool.status < 0) {
        tool.status = info.si_code == CLD_EXITED ? info.si_status : 128 + info.si_status;
        say("tool_exited " + std::to_string(tool.status));
#ifdef NGPOST_TESTING
        // Stands for a supervisor lost right after its tool ended (lot C5.6).
        if (::getenv("NGPOST_TEST_STALL_AFTER_TOOL_EXIT"))
            ::pause();
#endif
    }
    return true;
}
#endif

#if defined(__linux__)
int decodeStatus(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}
#endif

#if defined(__linux__)
//! Reaps whatever ended. False once no child is left at all (ECHILD).
bool reap(Tool &tool)
{
    for (;;) {
        int status = 0;
        const pid_t pid = ::waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            if (pid == tool.pid) {
                tool.status = decodeStatus(status);
                tool.pid = -1;
                // Its group may now be recycled: no longer signalled. Every
                // descendant is ours anyway (subreaper).
                tool.group = -1;
                say("tool_exited " + std::to_string(tool.status));
            }
            continue;
        }
        if (pid == 0)
            return true; // children left, still running
        if (errno == EINTR)
            continue;
        return errno != ECHILD;
    }
}
#endif

#if defined(__linux__)
void signalTree(const Tool &tool, int sig)
{
    if (tool.group > 0)
        ::kill(-tool.group, sig);
    for (const pid_t child : children())
        ::kill(child, sig);
}
#endif

//! Forks and execs the tool, its success confirmed by a pipe closed on exec.
void launch(Tool &tool, char **toolArgv)
{
    // The one attempt is spent whatever happens: never retried, never late.
    tool.launched = true;
    int status[2];
    int failed = makePipe(status) ? 0 : errno;
#ifdef NGPOST_TESTING
    if (!failed && ::getenv("NGPOST_TEST_FAIL_EXEC_PIPE")) {
        ::close(status[0]);
        ::close(status[1]);
        failed = EMFILE;
    }
#endif
    if (failed) {
        say("exec_failed " + std::to_string(failed));
        tool.status = 127;
        return;
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        // The tool: its own group, our private channels closed, signals as usual.
        ::setpgid(0, 0);
        ::close(kToController);
        ::close(kFromController);
        ::signal(SIGPIPE, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
#ifdef NGPOST_TESTING
        // Stands for an exec that hangs (a file system that does not answer).
        if (::getenv("NGPOST_TEST_STALL_BEFORE_EXEC"))
            ::pause();
#endif
        ::execvp(toolArgv[0], toolArgv);
        const int error = errno;
        ssize_t written = ::write(status[1], &error, sizeof error);
        (void)written;
        ::_exit(127);
    }
    ::close(status[1]);
    if (pid < 0) {
        say("exec_failed " + std::to_string(errno));
        ::close(status[0]);
        tool.status = 127;
        return;
    }
    // Also from here, before the child's own setpgid: no race on the group.
    ::setpgid(pid, pid);
    tool.pid = pid;
    tool.group = pid;
    tool.launchedPid = pid;
    // Its answer is read by the loop: an exec that hangs never deafens us.
    ::fcntl(status[0], F_SETFL, ::fcntl(status[0], F_GETFL) | O_NONBLOCK);
    tool.execPipe = status[0];
}

//! The child has ended, reaped or not.
bool ended(const Tool &tool)
{
    if (tool.pid < 0)
        return true; // reaped already
    siginfo_t info = {};
    return ::waitid(P_PID, static_cast<id_t>(tool.pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0
        && info.si_pid == tool.pid;
}

//! The exec confirmation, once it comes: end of file is a success, an errno
//! a failure (the child then exits 127 and is reaped as any). While stopping,
//! the end of file of a child already gone may be its death before exec: it
//! is not claimed as a start.
void readExecResult(Tool &tool, bool stopping)
{
    int error = 0;
    const ssize_t n = ::read(tool.execPipe, &error, sizeof error); // flawfinder: ignore
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    ::close(tool.execPipe);
    tool.execPipe = -1;
    if (n == static_cast<ssize_t>(sizeof error)) {
        say("exec_failed " + std::to_string(error));
        return;
    }
    if (stopping && ended(tool))
        return;
    tool.started = true;
    say("started " + std::to_string(tool.launchedPid));
}

#if defined(__linux__)
//! The proof, published atomically and durably: no descendant is left.
bool writeProof(const std::string &path,
                const std::string &nonce,
                const std::string &boot,
                const std::string &start,
                const Tool &tool)
{
    const std::string temporary = path + ".tmp";
    const std::string text = "ngpost-tool-proof 1\nnonce " + nonce + "\nboot " + boot + "\npid "
        + std::to_string(::getpid()) + "\nstarttime " + start + "\ntool_status "
        + std::to_string(tool.status) + "\n";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size())
        && ::fsync(fd) == 0;
    if (::close(fd) != 0 || !written || ::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    const std::size_t slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
    const int dirFd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd < 0)
        return false;
    const bool synced = ::fsync(dirFd) == 0;
    ::close(dirFd);
    return synced;
}
#endif

//! Reads "go" / "stop" lines; false once the controller is gone (end of file).
bool readCommands(std::string &buffer, bool &go, bool &stop)
{
    char chunk[64];
    const ssize_t n = ::read(kFromController, chunk, sizeof chunk); // flawfinder: ignore
    if (n < 0)
        return errno == EINTR || errno == EAGAIN;
    if (n == 0)
        return false;
    buffer.append(chunk, static_cast<std::size_t>(n));
    std::size_t eol = 0;
    while ((eol = buffer.find('\n')) != std::string::npos) {
        const std::string line = buffer.substr(0, eol);
        buffer.erase(0, eol + 1);
        go = go || line == "go";
        stop = stop || line == "stop";
    }
    return true;
}

long long nowMs()
{
    timespec ts = {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

} // namespace

int toolsupervisor::run(int argc, char **argv)
{
    if (argc < 6 || std::strcmp(argv[4], "--") != 0)
        return kSetupFailed;
    const std::string proof = argv[2];
    const std::string nonce = argv[3];
    char **toolArgv = argv + 5;

    // Never killed by a vanished controller; stopping first, death signal second.
    ::signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {};
    sa.sa_handler = &onStopSignal;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);
    const pid_t controller = ::getppid();
#if defined(__linux__)
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    bool lifeline = ::getppid() == controller;
#if defined(__linux__)
    if (::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
        say("setup_failed subreaper " + std::to_string(errno));
        return kSetupFailed;
    }
#endif
    const std::string boot = bootIdentity();
    const std::string start = startTime(::getpid());
    if (boot.empty() || start.empty()) {
        say("setup_failed identity");
        return kSetupFailed;
    }
    ::fcntl(kFromController, F_SETFL, ::fcntl(kFromController, F_GETFL) | O_NONBLOCK);
    say("ready " + std::to_string(::getpid()) + " " + start + " " + boot);

    Tool tool;
#ifdef NGPOST_TESTING
    // Stands for a tool so short that it is reaped before its exec answer is read.
    const bool slowExecAnswer = ::getenv("NGPOST_TEST_SLOW_EXEC_ANSWER") != nullptr;
#else
    const bool slowExecAnswer = false;
#endif
    std::string commands;
    bool go = false;
    bool stop = false;
    long long stoppingSince = -1;
    for (;;) {
        // Never blocked on one thing: commands, the exec answer and the tick.
        pollfd watched[2] = { { lifeline ? kFromController : -1, POLLIN, 0 },
                              { tool.execPipe, POLLIN, 0 } };
        ::poll(watched, 2, kTickMs);
        if (lifeline && (watched[0].revents & (POLLIN | POLLHUP))
            && !readCommands(commands, go, stop))
            lifeline = false; // the controller is gone: its tree goes too
        if (tool.execPipe >= 0 && (watched[1].revents & (POLLIN | POLLHUP)) && !slowExecAnswer)
            readExecResult(tool, stop || sStopSignal != 0 || !lifeline);
        stop = stop || sStopSignal != 0 || !lifeline;
        if (go && !stop && !tool.launched)
            launch(tool, toolArgv);
        if (stop && stoppingSince < 0)
            stoppingSince = nowMs();
        const int sig = nowMs() - stoppingSince < kTermGraceMs ? SIGTERM : SIGKILL;
#if defined(__linux__)
        if (stop) // repeated: a child adopted meanwhile, or one that ignores TERM
            signalTree(tool, sig);
        // Only no child at all is a proof; an empty listing never is. Without a
        // "go", no tool was ever started.
        const bool childrenLeft = reap(tool);
        if (!childrenLeft && (stop || tool.launched))
            break;
#else
        const bool ended = leaderEnded(tool);
        if (stop && tool.group > 0) {
            // Its zombie still holds the group: one last KILL reaches what is
            // left in it, and nothing else.
            ::kill(-tool.group, ended ? SIGKILL : sig);
            if (ended)
                break;
        } else if (stop || (tool.launched && ended)) {
            break;
        }
#endif
    }
#if !defined(__linux__)
    int reaped = 0;
    if (tool.pid > 0)
        ::waitpid(tool.pid, &reaped, 0); // ended already: only now its pid goes
#endif
    // The child is gone: its answer is there to read. Only a stop makes an
    // end of file ambiguous; a tool that ran and ended is announced as started.
    if (tool.execPipe >= 0)
        readExecResult(tool, stop);
#if !defined(__linux__)
    // Nothing proves that what the tool started is gone: never a proof here.
    (void)proof;
    (void)nonce;
    say("unproven " + std::to_string(tool.status));
    return kUnproven;
#else
    if (!writeProof(proof, nonce, boot, start, tool)) {
        say("proof_failed " + std::to_string(errno));
        return kProofFailed;
    }
    say("finished " + std::to_string(tool.status));
    return kProven;
#endif
}

#elif defined(_WIN32) // ------------------------------------------------------------------

// Windows (lot C5.4): the tool is created suspended, put in a job object
// before its first instruction, then resumed. The job lets nothing break
// away and kills everything in it when its last handle closes; the proof is
// written once the job has no active process left. The controller speaks
// through a named pipe whose name carries the nonce; its end is the lifeline.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "utils/WindowsCommandLine.h"

#include <QString>
#include <QStringList>

#include <algorithm>
#include <string>
#include <vector>
#include <windows.h>
#include <shellapi.h>

namespace
{

constexpr DWORD kTickMs = 50;
constexpr UINT kStoppedCode = 128 + 15; // what a stopped tool reports, as SIGTERM would

HANDLE sPipe = INVALID_HANDLE_VALUE;

void say(const std::string &line)
{
    if (sPipe == INVALID_HANDLE_VALUE)
        return;
    const std::string text = line + "\n";
    DWORD written = 0;
    ::WriteFile(sPipe, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

//! Creation time of a process: with its pid, it names one process.
std::string creationTime(HANDLE process)
{
    FILETIME created = {};
    FILETIME exited = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (!::GetProcessTimes(process, &created, &exited, &kernel, &user))
        return {};
    ULARGE_INTEGER value = {};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    return std::to_string(value.QuadPart);
}

struct Tool
{
    HANDLE process = nullptr;
    DWORD pid = 0;
    int status = -1;
    bool launched = false;
    bool exited = false;
};

//! False once the controller is gone (pipe broken).
bool readCommands(std::string &buffer, bool &go, bool &stop)
{
    DWORD available = 0;
    if (!::PeekNamedPipe(sPipe, nullptr, 0, nullptr, &available, nullptr))
        return false;
    if (available == 0)
        return true;
    char chunk[64];
    DWORD n = 0;
    if (!::ReadFile(sPipe, chunk, std::min<DWORD>(available, sizeof chunk), &n, nullptr))
        return false;
    buffer.append(chunk, n);
    std::size_t eol = 0;
    while ((eol = buffer.find('\n')) != std::string::npos) {
        const std::string line = buffer.substr(0, eol);
        buffer.erase(0, eol + 1);
        go = go || line == "go";
        stop = stop || line == "stop";
    }
    return true;
}

//! Only the standard handles are inherited: never the pipe to the controller.
std::vector<HANDLE> inheritedHandles()
{
    std::vector<HANDLE> handles;
    for (const DWORD which : { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE }) {
        const HANDLE h = ::GetStdHandle(which);
        if (h == nullptr || h == INVALID_HANDLE_VALUE
            || std::find(handles.begin(), handles.end(), h) != handles.end())
            continue;
        ::SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        handles.push_back(h);
    }
    return handles;
}

void failLaunch(Tool &tool, DWORD error)
{
    say("exec_failed " + std::to_string(error));
    tool.status = 127;
}

void launch(Tool &tool, HANDLE job, const QStringList &toolArgs)
{
    tool.launched = true; // one attempt, whatever happens
    std::wstring command = WindowsCommandLine::serialize(toolArgs).toStdWString();
    std::vector<HANDLE> handles = inheritedHandles();
    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> storage(size);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &size)
        || (!handles.empty()
            && !::UpdateProcThreadAttribute(attributes,
                                            0,
                                            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                            handles.data(),
                                            handles.size() * sizeof(HANDLE),
                                            nullptr,
                                            nullptr))) {
        failLaunch(tool, ::GetLastError());
        return;
    }
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.StartupInfo.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    startup.StartupInfo.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION info = {};
    const BOOL created = ::CreateProcessW(nullptr,
                                          command.data(),
                                          nullptr,
                                          nullptr,
                                          handles.empty() ? FALSE : TRUE,
                                          CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT
                                              | CREATE_NEW_PROCESS_GROUP,
                                          nullptr,
                                          nullptr,
                                          &startup.StartupInfo,
                                          &info);
    const DWORD createError = ::GetLastError();
    ::DeleteProcThreadAttributeList(attributes);
    if (!created) {
        failLaunch(tool, createError);
        return;
    }
    // In the job before its first instruction: nothing of it runs outside.
    if (!::AssignProcessToJobObject(job, info.hProcess)) {
        const DWORD error = ::GetLastError();
        ::TerminateProcess(info.hProcess, 127);
        ::WaitForSingleObject(info.hProcess, INFINITE);
        ::CloseHandle(info.hThread);
        ::CloseHandle(info.hProcess);
        failLaunch(tool, error);
        return;
    }
    tool.process = info.hProcess;
    tool.pid = info.dwProcessId;
    if (::ResumeThread(info.hThread) == static_cast<DWORD>(-1)) {
        const DWORD error = ::GetLastError();
        ::TerminateJobObject(job, 127); // never left suspended
        ::CloseHandle(info.hThread);
        failLaunch(tool, error);
        return;
    }
    ::CloseHandle(info.hThread);
    say("started " + std::to_string(tool.pid));
}

DWORD activeProcesses(HANDLE job)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info = {};
    if (!::QueryInformationJobObject(job,
                                     JobObjectBasicAccountingInformation,
                                     &info,
                                     sizeof info,
                                     nullptr))
        return 1; // not known: never taken for none
    return info.ActiveProcesses;
}

void noteToolExit(Tool &tool)
{
    if (!tool.process || tool.exited || ::WaitForSingleObject(tool.process, 0) != WAIT_OBJECT_0)
        return;
    DWORD code = 0;
    ::GetExitCodeProcess(tool.process, &code);
    tool.exited = true;
    if (tool.status < 0)
        tool.status = static_cast<int>(code);
    say("tool_exited " + std::to_string(tool.status));
}

bool writeProof(const QString &path,
                const std::string &nonce,
                const std::string &start,
                const Tool &tool)
{
    const std::string text = "ngpost-tool-proof 1\nnonce " + nonce + "\nboot -\npid "
        + std::to_string(::GetCurrentProcessId()) + "\nstarttime " + start + "\ntool_status "
        + std::to_string(tool.status) + "\n";
    const std::wstring target = path.toStdWString();
    const std::wstring temporary = target + L".tmp";
    const HANDLE file = ::CreateFileW(temporary.c_str(),
                                      GENERIC_WRITE,
                                      0,
                                      nullptr,
                                      CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = ::WriteFile(file,
                                text.data(),
                                static_cast<DWORD>(text.size()),
                                &written,
                                nullptr)
        && written == text.size() && ::FlushFileBuffers(file);
    ::CloseHandle(file);
    if (!ok
        || !::MoveFileExW(temporary.c_str(),
                          target.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool connectToController(const QString &nonce)
{
    const std::wstring name = (QStringLiteral("\\\\.\\pipe\\ngpost-tool-") + nonce).toStdWString();
    for (int attempt = 0; attempt < 100; ++attempt) {
        sPipe = ::CreateFileW(name.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              0,
                              nullptr,
                              OPEN_EXISTING,
                              0,
                              nullptr);
        if (sPipe != INVALID_HANDLE_VALUE)
            return true;
        if (!::WaitNamedPipeW(name.c_str(), kTickMs))
            ::Sleep(kTickMs);
    }
    return false;
}

} // namespace

int toolsupervisor::run(int, char **)
{
    int count = 0;
    LPWSTR *wide = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    QStringList args;
    for (int i = 0; wide && i < count; ++i)
        args << QString::fromWCharArray(wide[i]);
    ::LocalFree(wide);
    if (args.size() < 6 || args.at(4) != QStringLiteral("--"))
        return kSetupFailed;
    const QString proof = args.at(2);
    const std::string nonce = args.at(3).toStdString();
    const QStringList toolArgs = args.mid(5);
    if (!connectToController(args.at(3)))
        return kSetupFailed;
    const HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    // No breakaway allowed; everything in it dies with its last handle.
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job
        || !::SetInformationJobObject(job,
                                      JobObjectExtendedLimitInformation,
                                      &limits,
                                      sizeof limits)) {
        say("setup_failed job " + std::to_string(::GetLastError()));
        return kSetupFailed;
    }
    const std::string start = creationTime(::GetCurrentProcess());
    say("ready " + std::to_string(::GetCurrentProcessId()) + " " + start + " -");

    Tool tool;
    std::string commands;
    bool go = false;
    bool stop = false;
    bool lifeline = true;
    for (;;) {
        if (lifeline && !readCommands(commands, go, stop))
            lifeline = false; // the controller is gone: its tree goes too
        stop = stop || !lifeline;
        if (go && !stop && !tool.launched)
            launch(tool, job, toolArgs);
        if (stop && activeProcesses(job) > 0) {
            if (tool.status < 0 && tool.process && !tool.exited)
                tool.status = static_cast<int>(kStoppedCode);
            ::TerminateJobObject(job, kStoppedCode);
        }
        noteToolExit(tool);
        // Only an empty job is a proof; without a "go", nothing was started.
        if ((stop || tool.launched) && activeProcesses(job) == 0 && (!tool.process || tool.exited))
            break;
        ::Sleep(kTickMs);
    }
    if (!writeProof(proof, nonce, start, tool)) {
        say("proof_failed " + std::to_string(::GetLastError()));
        return kProofFailed;
    }
    say("finished " + std::to_string(tool.status));
    if (tool.process)
        ::CloseHandle(tool.process);
    ::CloseHandle(job);
    return kProven;
}

#else // neither POSIX supported here nor Windows

int toolsupervisor::run(int, char **)
{
    return kSetupFailed;
}

#endif
