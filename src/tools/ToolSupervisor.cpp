// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// The tool supervisor (see ToolSupervisor.h). Plain POSIX, no Qt: it runs
// before any QCoreApplication, and its only job is to prove that the tool and
// all its descendants are gone before it says so.
//
//========================================================================

#include "tools/ToolSupervisor.h"

#if defined(__linux__)

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
#include <sys/prctl.h>
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

int decodeStatus(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

//! Reaps whatever ended. False once no child is left at all (ECHILD).
bool reap(Tool &tool)
{
    for (;;) {
        int status = 0;
        const pid_t pid = ::waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            if (pid == tool.pid) {
                tool.status = decodeStatus(status);
                tool.pid = -1; // its group may now be recycled: no longer signalled
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

void signalTree(const Tool &tool, int sig)
{
    if (tool.group > 0)
        ::kill(-tool.group, sig);
    for (const pid_t child : children())
        ::kill(child, sig);
}

//! Forks and execs the tool, its success confirmed by a pipe closed on exec.
void launch(Tool &tool, char **toolArgv)
{
    // The one attempt is spent whatever happens: never retried, never late.
    tool.launched = true;
    int status[2];
    int failed = ::pipe2(status, O_CLOEXEC) != 0 ? errno : 0;
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
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    bool lifeline = ::getppid() == controller;
    if (::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
        say("setup_failed subreaper " + std::to_string(errno));
        return kSetupFailed;
    }
    const std::string boot = readSmallFile("/proc/sys/kernel/random/boot_id");
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
        if (stop && stoppingSince < 0) {
            stoppingSince = nowMs();
            signalTree(tool, SIGTERM);
        } else if (stop) {
            // Repeated: a child adopted meanwhile, or one that ignores TERM.
            signalTree(tool, nowMs() - stoppingSince < kTermGraceMs ? SIGTERM : SIGKILL);
        }
        // Only no child at all is a proof; an empty listing never is. Without a
        // "go", no tool was ever started.
        const bool childrenLeft = reap(tool);
        if (!childrenLeft && (stop || tool.launched))
            break;
    }
    // The child is gone: its answer is there to read. Only a stop makes an
    // end of file ambiguous; a tool that ran and ended is announced as started.
    if (tool.execPipe >= 0)
        readExecResult(tool, stop);
    if (!writeProof(proof, nonce, boot, start, tool)) {
        say("proof_failed " + std::to_string(errno));
        return kProofFailed;
    }
    say("finished " + std::to_string(tool.status));
    return kProven;
}

#else // not Linux

int toolsupervisor::run(int, char **)
{
    return kSetupFailed; // no proven supervision here (lots C5.4, C5.5)
}

#endif
