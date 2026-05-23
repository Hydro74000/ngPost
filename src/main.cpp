//========================================================================
//
// Copyright (C) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
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
#include <csignal>
#include <iostream>

#if defined(__USE_HMI__) && defined(Q_OS_WIN)
#include <windows.h>
#endif

#if defined(Q_OS_UNIX)
#  include <QSocketDescriptor>
#  include <QSocketNotifier>
#  include <fcntl.h>
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

extern "C" void posixSignalHandler(int sig)
{
    // The only thing we are allowed to do from signal context. write() is on
    // the POSIX signal-safety list; the pipe is O_NONBLOCK so a wedged reader
    // can never make us block. We ignore the return value: if the pipe is
    // full because a previous signal hasn't been drained yet, dropping a
    // duplicate Ctrl-C is harmless (same intent as the first one).
    unsigned char s = static_cast<unsigned char>(sig);
    ssize_t r = ::write(sSigPipe[1], &s, 1);
    (void)r;
}

bool installPosixSignalRouter()
{
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

#else // Q_OS_WIN

// Windows note: signal() under the MSVC CRT runs the handler on a *fresh*
// Win32 thread, not by preempting the main thread mid-mutex. The cout/quit()
// race is therefore much less severe than on POSIX — we keep the simple form
// here. A future hardening pass could switch to SetConsoleCtrlHandler if
// needed.
extern "C" void handleShutdownWin(int)
{
    std::cout << "Closing the application...\n";
    std::cout.flush();
    QCoreApplication::quit();
}

#endif // Q_OS_UNIX vs Q_OS_WIN

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
    // disable SSL warnings
    QLoggingCategory::setFilterRules("qt.network.ssl.warning=false");

#if defined(Q_OS_UNIX)
    // Install the signal router *before* NgPost's constructor runs — NgPost
    // wires up VpnManager, UpdateChecker, PostHistoryService and the network
    // access manager, any of which can spawn Qt internal threads. Doing this
    // first ensures every later thread inherits the blocked signal mask, so
    // the kernel always delivers our signals to the main thread.
    if (!installPosixSignalRouter())
        std::cerr << "warning: failed to install POSIX signal router\n";
#else
    ::signal(SIGINT,  &handleShutdownWin); // shut down on ctrl-c
    ::signal(SIGTERM, &handleShutdownWin); // shut down on killall
#endif

    //    qDebug() << "argc: " << argc;
    app = new NgPost(argc, argv);

#if defined(Q_OS_UNIX)
    // Plug the read end of the self-pipe into a QSocketNotifier owned by the
    // NgPost object. The notifier lives in the main thread, so its slot runs
    // in main-thread context — making it safe to call std::cout, quit() and
    // QWidget methods from there.
    if (sSigPipe[0] >= 0) {
        auto *sn = new QSocketNotifier(sSigPipe[0], QSocketNotifier::Read, app);
        QObject::connect(sn, &QSocketNotifier::activated, app,
            [](QSocketDescriptor fd, QSocketNotifier::Type) {
                // Drain the pipe — several signals could pile up between two
                // event-loop ticks. Stop on EAGAIN.
                unsigned char s;
                while (::read(static_cast<int>(fd), &s, 1) == 1)
                    dispatchSignal(static_cast<int>(s));
            });
        // Safe to unblock on the main thread now: the kernel will deliver
        // signals here (workers have them blocked), the handler writes a byte
        // to the pipe, the notifier fires on the next event-loop iteration.
        unblockSignalsOnMainThread();
    }
#endif

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

            if (app->nzbCheck())
                exitCode = app->nbMissingArticles();
        }
#ifdef __DEBUG__
        std::cout << app->appName() << " closed properly!\n";
        std::cout.flush();
#endif
    }
    else
    {
#ifdef __DEBUG__
        std::cout << "Nothing to do...\n";
        std::cout.flush();
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
    return exitCode;
}
