//========================================================================
//
// Copyright (C) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3..
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>
//
//========================================================================

#include <QCoreApplication>
#include <QLoggingCategory>
#include "NgPost.h"
#include "tools/ToolSupervisor.h"
#include <csignal>
#include <cstring>
#include <iostream>

#if defined(Q_OS_WIN)
#include <QWinEventNotifier>
#include <atomic>
#include <cstring>
#include <windows.h>
#ifdef __USE_HMI__
#include <QGuiApplication>
#include <QSessionManager>
#endif
#endif

#if defined(Q_OS_UNIX)
#  include <QSocketDescriptor>
#  include <QSocketNotifier>
#include <cerrno>
#include <cstring>
#  include <fcntl.h>
#include <poll.h>
#  include <pthread.h>
#  include <unistd.h>
#endif

static NgPost *app = nullptr;

// ----------------------------------------------------------------------------
//  POSIX signal routing — self-pipe trick
// ----------------------------------------------------------------------------
//  The previous implementation called std::cout, QCoreApplication::quit() and
//  CmdOrGuiApp::hideOrShowGUI() directly from the signal handler. None of those
//  are async-signal-safe: std::cout takes a mutex (and may malloc), Qt's event
//  queue is mutex-protected too, and touching QWidget from a signal that
//  interrupted the GUI thread mid-paint is a recipe for hard-to-reproduce
//  deadlocks and crashes — especially under load (verbose CLI logging,
//  SIGUSR1 while the progress bar is repainting, systemd shutdown during a
//  PostingJob::_finishPosting flush).
//
//  The fix is the standard self-pipe trick: the handler does *only* a write()
//  of the signal number (the one POSIX-listed AS-safe primitive that fits the
//  bill), and a QSocketNotifier living on the main thread reads the byte and
//  dispatches the real work back into Qt's event loop. From there everything
//  runs in main-thread context where cout / quit() / GUI calls are safe.
//
//  Worker threads spawned by NgPost (Posters, ArticleBuilder, monitor,
//  history service, network manager) inherit the blocked signal mask we set
//  before NgPost's constructor runs — so the kernel routes SIGINT/SIGTERM/
//  SIGUSR1 exclusively to the main thread, where our handler is the only
//  signal-context code that executes.
// ----------------------------------------------------------------------------

namespace
{

#if defined(Q_OS_UNIX)

int sSigPipe[2] = {-1, -1};

// Stopping on SIGINT / SIGTERM (plan_queue.md, lot C4.8). The first signal
// starts the graceful exit on the event loop and arms a native watchdog; the
// second, or the watchdog, ends the process at once with _exit(): no
// destructor, no Qt, no SQLite -- a blocked main thread cannot delay it.
volatile sig_atomic_t sStopSignals = 0;
volatile sig_atomic_t sForcing = 0; //!< a forced exit is under way
unsigned sExitWatchdogSeconds = 30; //!< NGPOST_EXIT_WATCHDOG_SECONDS

//! Best effort, never blocking: a full or stalled stderr (a pipe nobody reads)
//! loses the message, never the exit. poll(), strlen() and write() are
//! async-signal-safe, and the message is shorter than PIPE_BUF, so a pipe with
//! room takes it whole.
void writeDiagnostic(const char *message)
{
    pollfd err = {};
    err.fd = STDERR_FILENO;
    err.events = POLLOUT;
    if (::poll(&err, 1, 0) == 1 && (err.revents & POLLOUT)) {
        ssize_t r = ::write(STDERR_FILENO, message, std::strlen(message));
        (void)r;
    }
}

//! Ends the process now. A write that blocks anyway (a stalled file system)
//! is cut by one more alarm, whose handler sees sForcing and exits at once.
[[noreturn]] void forceExit(const char *message, int code)
{
    sForcing = 1;
    ::alarm(1);
    writeDiagnostic(message);
    ::_exit(code);
}

extern "C" void exitWatchdogHandler(int)
{
    if (sForcing != 0)
        ::_exit(128 + SIGALRM);
    forceExit("ngPost: not stopped in time after the stop signal: forced exit\n", 128 + SIGALRM);
}

void readExitWatchdog()
{
    // How long the graceful exit may take after the first stop signal.
    bool set = false;
    const int seconds = qEnvironmentVariableIntValue("NGPOST_EXIT_WATCHDOG_SECONDS", &set);
    if (set && seconds > 0 && seconds <= 3600)
        sExitWatchdogSeconds = static_cast<unsigned>(seconds);
}

extern "C" void posixSignalHandler(int sig)
{
    const int savedErrno = errno;
    if (sig == SIGINT || sig == SIGTERM) {
        if (sStopSignals != 0)
            forceExit("ngPost: second stop signal: forced exit\n", 128 + sig);
        sStopSignals = 1;
        ::alarm(sExitWatchdogSeconds); // independent of the event loop
    }
    // The only other thing we are allowed to do from signal context. write()
    // is on the POSIX signal-safety list; the pipe is O_NONBLOCK so a wedged
    // reader can never make us block. We ignore the return value: if the pipe
    // is full, dropping a duplicate signal is harmless (same intent).
    unsigned char s = static_cast<unsigned char>(sig);
    ssize_t r = ::write(sSigPipe[1], &s, 1);
    (void)r;
    errno = savedErrno;
}

bool installPosixSignalRouter()
{
    readExitWatchdog();
    // Atomic creation with O_CLOEXEC so QProcess children we spawn (rar, par2,
    // openvpn helper) don't inherit these fds, and O_NONBLOCK so the signal
    // handler's write() can never block.
#if defined(__linux__)
    if (::pipe2(sSigPipe, O_CLOEXEC | O_NONBLOCK) != 0)
        return false;
#else
    // pipe2() is Linux/BSD-only. On macOS we fall back to pipe() + fcntl()
    // — there's a microscopic race window where a fork between pipe() and
    // the fcntl(FD_CLOEXEC) could leak the fd, but this code runs before any
    // QProcess is started, so in practice it's fine.
    if (::pipe(sSigPipe) != 0)
        return false;
    for (int i = 0; i < 2; ++i) {
        int fdFlags = ::fcntl(sSigPipe[i], F_GETFD);
        ::fcntl(sSigPipe[i], F_SETFD, fdFlags | FD_CLOEXEC);
        int statusFlags = ::fcntl(sSigPipe[i], F_GETFL);
        ::fcntl(sSigPipe[i], F_SETFL, statusFlags | O_NONBLOCK);
    }
#endif

    // Block these signals on the current (main) thread *now*, before any
    // QThread is spawned. Any thread created after this point inherits the
    // blocked mask, which guarantees the kernel will deliver SIGINT / SIGTERM
    // / SIGUSR1 to the main thread only — where our QSocketNotifier lives.
    // We unblock on the main thread once the notifier is wired up.
    sigset_t toBlock;
    sigemptyset(&toBlock);
    sigaddset(&toBlock, SIGINT);
    sigaddset(&toBlock, SIGTERM);
#ifdef __linux__
    sigaddset(&toBlock, SIGUSR1);
#endif
    ::pthread_sigmask(SIG_BLOCK, &toBlock, nullptr);

    struct sigaction sa{};
    sa.sa_handler = &posixSignalHandler;
    sigemptyset(&sa.sa_mask);
    // A SIGTERM landing in the middle of the SIGINT handler (or the reverse)
    // waits for it: the "first signal" test cannot be interleaved.
    sigaddset(&sa.sa_mask, SIGINT);
    sigaddset(&sa.sa_mask, SIGTERM);
    // SA_RESTART so a signal arriving during a worker thread's blocking
    // syscall (NNTP socket read/write, SQLite I/O, file read in
    // ArticleBuilder) doesn't surface EINTR all over the codebase. The
    // signal stays queued on the pipe; real handling happens on the main
    // thread's event loop tick.
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
#ifdef __linux__
    ::sigaction(SIGUSR1, &sa, nullptr);
#endif
    // The watchdog stays installed until the very end, destructors included.
    // Re-entrant: the alarm armed by forceExit() must reach a handler that is
    // itself stuck in write().
    struct sigaction watchdog = {};
    watchdog.sa_handler = &exitWatchdogHandler;
    sigemptyset(&watchdog.sa_mask);
    watchdog.sa_flags = SA_NODEFER;
    ::sigaction(SIGALRM, &watchdog, nullptr);
    return true;
}

void unblockSignalsOnMainThread()
{
    sigset_t toUnblock;
    sigemptyset(&toUnblock);
    sigaddset(&toUnblock, SIGINT);
    sigaddset(&toUnblock, SIGTERM);
#ifdef __linux__
    sigaddset(&toUnblock, SIGUSR1);
#endif
    ::pthread_sigmask(SIG_UNBLOCK, &toUnblock, nullptr);
}

void teardownPosixSignalRouter()
{
    // Restore defaults so any late signal (between here and process exit)
    // takes the standard path instead of trying to write to a pipe that's
    // about to be closed.
    ::signal(SIGINT,  SIG_DFL);
    ::signal(SIGTERM, SIG_DFL);
#ifdef __linux__
    ::signal(SIGUSR1, SIG_DFL);
#endif
    if (sSigPipe[0] >= 0) { ::close(sSigPipe[0]); sSigPipe[0] = -1; }
    if (sSigPipe[1] >= 0) { ::close(sSigPipe[1]); sSigPipe[1] = -1; }
}

void dispatchSignal(int sig)
{
    switch (sig) {
    case SIGINT:
    case SIGTERM:
        std::cout << "Closing the application...\n";
        std::cout.flush();
        if (app)
            app->requestGracefulExit();
        else
            QCoreApplication::quit();
        break;
#ifdef __linux__
    case SIGUSR1:
        std::cout << "intercept SIGUSR1 :)\n";
        std::cout.flush();
#ifdef __USE_HMI__
        if (app) app->hideOrShowGUI();
#endif
        break;
#endif
    default:
        break;
    }
}

//! Once NgPost exists: the self-pipe is read on its thread.
void attachPosixSignalRouter(NgPost *ngPost)
{
    // Plug the read end of the self-pipe into a QSocketNotifier owned by the
    // NgPost object. The notifier lives in the main thread, so its slot runs
    // in main-thread context — making it safe to call std::cout, quit() and
    // QWidget methods from there.
    if (sSigPipe[0] >= 0) {
        auto *sn = new QSocketNotifier(sSigPipe[0], QSocketNotifier::Read, ngPost);
        QObject::connect(sn,
                         &QSocketNotifier::activated,
                         ngPost,
                         [](QSocketDescriptor fd, QSocketNotifier::Type) {
            // Drain the pipe — several signals can pile up between two
            // event-loop ticks — with one bounded read rather than an
            // open-ended byte-at-a-time loop. read() never returns more
            // than sizeof(buf), so the dispatch below is bounded by
            // construction; the notifier is level-triggered, so anything
            // still queued comes back on the next tick. That bound is what
            // keeps a signal flood (a process spamming SIGUSR1, a user
            // leaning on Ctrl-C) from starving the event loop — each byte
            // costs a cout + flush, so an unbounded loop can be fed faster
            // than it drains. A negative or zero return leaves the loop
            // body unexecuted, which is the wanted behaviour for both
            // EAGAIN (pipe empty) and EOF (write end gone).
            unsigned char buf[32];
            ssize_t const n = ::read(int(fd), buf, sizeof buf); // flawfinder: ignore
            for (ssize_t i = 0; i < n; ++i)
                dispatchSignal(static_cast<int>(buf[i]));
        });
        // Safe to unblock on the main thread now: the kernel will deliver
        // signals here (workers have them blocked), the handler writes a byte
        // to the pipe, the notifier fires on the next event-loop iteration.
        unblockSignalsOnMainThread();
    }
}

#else // Q_OS_WIN

// Stopping on Ctrl+C / Ctrl+Break, console close and session end (plan_queue.md,
// lot C4.9), the POSIX protocol: the first request runs the graceful exit on
// the main thread (an event a QWinEventNotifier watches) and arms a native
// watchdog; a second Ctrl+C/Break, or the watchdog, ends the process with
// TerminateProcess() -- no CRT or DLL teardown, nothing a blocked main thread
// could hold up. Console close and session end wait at most 4 s for the end
// of main(), then let Windows end the process.
std::atomic<int> sInterrupts{ 0 };
std::atomic<bool> sStopRequested{ false };
HANDLE sStopRequest = nullptr;   //!< auto reset: the main thread runs the protocol
HANDLE sWatchdogArmed = nullptr; //!< manual reset: the countdown has started
HANDLE sExitDone = nullptr;      //!< manual reset: main() is over, app deleted
DWORD sExitWatchdogMs = 30000;   //!< NGPOST_EXIT_WATCHDOG_SECONDS
constexpr DWORD kSystemStopBudgetMs = 4000;

DWORD WINAPI writeDiagnosticThread(LPVOID message)
{
    const char *text = static_cast<const char *>(message);
    DWORD written = 0;
    ::WriteFile(::GetStdHandle(STD_ERROR_HANDLE),
                text,
                static_cast<DWORD>(std::strlen(text)),
                &written,
                nullptr);
    return 0;
}

//! Ends the process now. The diagnostic is tried on the side and waited for a
//! moment only: a full pipe on stderr never holds the exit.
[[noreturn]] void forceExit(const char *message, UINT code)
{
    if (HANDLE writer = ::CreateThread(nullptr,
                                       0,
                                       &writeDiagnosticThread,
                                       const_cast<char *>(message),
                                       0,
                                       nullptr)) {
        ::WaitForSingleObject(writer, 200);
        ::CloseHandle(writer);
    }
    ::TerminateProcess(::GetCurrentProcess(), code);
    ::_exit(static_cast<int>(code)); // not reached
}

DWORD WINAPI exitWatchdogThread(LPVOID)
{
    ::WaitForSingleObject(sWatchdogArmed, INFINITE);
    if (::WaitForSingleObject(sExitDone, sExitWatchdogMs) == WAIT_TIMEOUT)
        forceExit("ngPost: not stopped in time after the stop request: forced exit\n", 142);
    return 0;
}

//! Any thread: the first request starts the protocol and the watchdog.
void requestStop()
{
    if (sStopRequested.exchange(true))
        return;
    ::SetEvent(sWatchdogArmed);
    ::SetEvent(sStopRequest);
}

BOOL WINAPI consoleControlHandler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        if (sInterrupts.fetch_add(1) > 0)
            forceExit("ngPost: second stop signal: forced exit\n", 130);
        requestStop();
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        // Windows ends the process when this returns: the protocol gets its
        // budget. Not a second interruption.
        requestStop();
        ::WaitForSingleObject(sExitDone, kSystemStopBudgetMs);
        return TRUE;
    default: return FALSE;
    }
}

LRESULT CALLBACK sessionEndWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_QUERYENDSESSION: requestStop(); return TRUE;
    case WM_ENDSESSION:
        if (wParam)
            ::WaitForSingleObject(sExitDone, kSystemStopBudgetMs);
        return 0;
    default: return ::DefWindowProcW(window, message, wParam, lParam);
    }
}

//! A command line run has no top-level window, and only those hear of the end
//! of the session (a process with user32 gets no CTRL_LOGOFF/SHUTDOWN). A
//! hidden one, on its own thread: waiting in it never blocks main().
DWORD WINAPI sessionEndThread(LPVOID)
{
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = &sessionEndWindowProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"ngPostSessionEnd";
    if (!::RegisterClassW(&windowClass)
        || !::CreateWindowExW(0,
                              windowClass.lpszClassName,
                              L"ngPost",
                              WS_OVERLAPPED,
                              0,
                              0,
                              0,
                              0,
                              nullptr,
                              nullptr,
                              windowClass.hInstance,
                              nullptr))
        return 1;
    MSG message;
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return 0;
}

//! Before NgPost exists: events and watchdog first, then the handler.
bool installWindowsStopRouter()
{
    bool set = false;
    const int seconds = qEnvironmentVariableIntValue("NGPOST_EXIT_WATCHDOG_SECONDS", &set);
    if (set && seconds > 0 && seconds <= 3600)
        sExitWatchdogMs = static_cast<DWORD>(seconds) * 1000;
    sStopRequest = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    sWatchdogArmed = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    sExitDone = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!sStopRequest || !sWatchdogArmed || !sExitDone)
        return false;
    HANDLE watchdog = ::CreateThread(nullptr, 0, &exitWatchdogThread, nullptr, 0, nullptr);
    if (!watchdog)
        return false;
    ::CloseHandle(watchdog);
    return ::SetConsoleCtrlHandler(&consoleControlHandler, TRUE) != 0;
}

//! Once NgPost exists: the protocol on its thread, and the end of session.
void attachWindowsStopRouter(NgPost *ngPost)
{
    if (!sStopRequest)
        return;
    auto *notifier = new QWinEventNotifier(sStopRequest, ngPost);
    QObject::connect(notifier, &QWinEventNotifier::activated, ngPost, [ngPost] {
        std::cout << "Closing the application...\n";
        std::cout.flush();
        ngPost->requestGracefulExit();
    });
#ifdef __USE_HMI__
    if (ngPost->useHMI()) {
        // Qt hears the end of a GUI session: no dialog, the protocol starts.
        QObject::connect(qGuiApp,
                         &QGuiApplication::commitDataRequest,
                         ngPost,
                         [ngPost](QSessionManager &) {
            requestStop();
            ngPost->requestGracefulExit();
        },
                         Qt::DirectConnection);
        return;
    }
#endif
    if (HANDLE thread = ::CreateThread(nullptr, 0, &sessionEndThread, nullptr, 0, nullptr))
        ::CloseHandle(thread);
}

#endif // Q_OS_UNIX vs Q_OS_WIN

//! Before NgPost's constructor runs. On POSIX, NgPost wires up VpnManager,
//! UpdateChecker, PostHistoryService and the network access manager, any of
//! which can spawn Qt internal threads: installing first ensures every later
//! thread inherits the blocked signal mask, so the kernel always delivers our
//! signals to the main thread.
void installStopRouter()
{
#if defined(Q_OS_UNIX)
    if (!installPosixSignalRouter())
        std::cerr << "warning: failed to install POSIX signal router\n";
#else
    if (!installWindowsStopRouter())
        std::cerr << "warning: failed to install the Windows stop handler\n";
#endif
}

//! Once NgPost exists: stop requests reach it on its thread.
void attachStopRouter(NgPost *ngPost)
{
#if defined(Q_OS_UNIX)
    attachPosixSignalRouter(ngPost);
#else
    attachWindowsStopRouter(ngPost);
#endif
}

} // anonymous namespace

#if defined(__USE_TMP_RAM__) && defined(__DEBUG__)
#include "PostingJob.h"
void dispFolderSize(const QFileInfo &folderPath)
{
    qint64 size = NgPost::recursiveSize(folderPath);
    qDebug() << "size " << folderPath.absoluteFilePath() << " : "
             << PostingJob::humanSize(static_cast<double>(size)) << " (" << size << ")";
}
#endif

int main(int argc, char* argv[])
{
    // A preparation tool's supervisor (lot C5.3): ngPost itself, before any
    // Qt object, signal router or configuration.
    if (argc > 1 && std::strcmp(argv[1], toolsupervisor::kSwitch) == 0)
        return toolsupervisor::run(argc, argv);

    // disable SSL warnings
    QLoggingCategory::setFilterRules("qt.network.ssl.warning=false");

    installStopRouter();
    //    qDebug() << "argc: " << argc;
    app = new NgPost(argc, argv);
    attachStopRouter(app);

    int exitCode = 0;
#ifdef __USE_HMI__
    if (app->useHMI())
    {
#if defined( Q_OS_WIN )
        ::ShowWindow(::GetConsoleWindow(), SW_HIDE); //hide console window
#endif
        app->checkSupportSSL();
        exitCode = app->startHMI();
    }
    else if (app->parseCommandLine(argc, argv))
#else
    if (app->parseCommandLine(argc, argv))
#endif
    {
        if (app->checkSupportSSL())
        {
            exitCode = app->startEventLoop();

            // The exit code of --check is a verdict, not a count: 0 complete,
            // 1 articles missing, 3 inconclusive. It used to be the raw number
            // of missing articles, which POSIX truncates modulo 256 -- 256
            // missing articles reported success.
            if (app->nzbCheck())
                exitCode = app->nzbCheckExitCode();
        }
        else if (app->nzbCheck())
        {
            // A TLS preflight failure happens after the NZB was parsed but
            // before the event loop. It still needs a real check verdict and,
            // with --check_json, its promised machine-readable report.
            app->reportNzbCheckSslUnavailable();
            exitCode = app->nzbCheckExitCode();
        }
#ifdef __DEBUG__
        if (!app->stdoutIsData()) {
            std::cout << app->appName() << " closed properly!\n";
            std::cout.flush();
        }
#endif
    }
    else
    {
        // A --check that could not even start still owes the caller its
        // verdict: 3, not a silent 0.
        if (app->nzbCheck())
            exitCode = app->nzbCheckExitCode();
#ifdef __DEBUG__
        if (!app->stdoutIsData()) {
            std::cout << "Nothing to do...\n";
            std::cout.flush();
        }
#endif
    }

    if (app->errCode() != 0)
        exitCode = app->errCode();

#if defined(Q_OS_UNIX)
    // Restore SIG_DFL and close the pipe before tearing down `app`, so any
    // late signal between here and exit() takes the kernel default path
    // instead of trying to write into a doomed QSocketNotifier.
    teardownPosixSignalRouter();
#endif

    delete app;
#if defined(Q_OS_WIN)
    if (sExitDone)
        ::SetEvent(sExitDone); // what a console close or session end waits for
#endif
    return exitCode;
}
