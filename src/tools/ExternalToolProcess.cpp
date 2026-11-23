// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Controller side of a supervised external tool (see ExternalToolProcess.h).
//
//========================================================================

#include "tools/ExternalToolProcess.h"

#include "tools/ToolSupervisor.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSocketNotifier>
#include <QUuid>

#include <optional>

#if defined(Q_OS_LINUX)
#include <cerrno>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{

#if defined(Q_OS_LINUX)
QString readSmall(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromLatin1(file.readAll()).trimmed()
                                          : QString();
}

//! Field 22 of /proc/<pid>/stat (see ToolSupervisor.cpp).
QString startTimeOf(qint64 pid)
{
    const QString stat = readSmall(QStringLiteral("/proc/%1/stat").arg(pid));
    const qsizetype comm = stat.lastIndexOf(QLatin1Char(')'));
    if (comm < 0)
        return {};
    const QStringList fields = stat.mid(comm + 2).split(QLatin1Char(' '));
    return fields.size() > 19 ? fields.at(19) : QString();
}
#endif

//! The proof file of @p identity, read: its fields when it is that
//! supervision's, nothing otherwise.
std::optional<int> provenStatus(const QJsonObject &identity)
{
    QFile file(identity.value(QStringLiteral("proof")).toString());
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QStringList lines = QString::fromLatin1(file.readAll()).split(QLatin1Char('\n'));
    if (lines.value(0) != QStringLiteral("ngpost-tool-proof 1"))
        return std::nullopt;
    QHash<QString, QString> fields;
    for (const QString &line : lines) {
        const qsizetype space = line.indexOf(QLatin1Char(' '));
        if (space > 0)
            fields.insert(line.left(space), line.mid(space + 1));
    }
    for (const char *key : { "nonce", "boot", "pid", "starttime" })
        if (fields.value(QLatin1String(key)) != identity.value(QLatin1String(key)).toString())
            return std::nullopt;
    bool ok = false;
    const int status = fields.value(QStringLiteral("tool_status")).toInt(&ok);
    return ok ? std::optional<int>(status) : std::nullopt;
}

//! The supervisor of @p identity is still alive: the same process, not one
//! that got its pid since.
bool supervisorAlive(const QJsonObject &identity)
{
#if defined(Q_OS_LINUX)
    const qint64 pid = identity.value(QStringLiteral("pid")).toString().toLongLong();
    return pid > 0
        && readSmall(QStringLiteral("/proc/sys/kernel/random/boot_id"))
        == identity.value(QStringLiteral("boot")).toString()
        && startTimeOf(pid) == identity.value(QStringLiteral("starttime")).toString();
#elif defined(Q_OS_WIN)
    const auto pid = static_cast<DWORD>(identity.value(QStringLiteral("pid")).toString().toULong());
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    FILETIME created = {};
    FILETIME exited = {};
    FILETIME kernel = {};
    FILETIME user = {};
    DWORD code = 0;
    const bool live = ::GetProcessTimes(process, &created, &exited, &kernel, &user)
        && ::GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    ::CloseHandle(process);
    ULARGE_INTEGER start = {};
    start.LowPart = created.dwLowDateTime;
    start.HighPart = created.dwHighDateTime;
    return live
        && QString::number(start.QuadPart)
        == identity.value(QStringLiteral("starttime")).toString();
#else
    Q_UNUSED(identity)
    return false;
#endif
}

} // namespace

QString ExternalToolProcess::capability()
{
#if defined(Q_OS_LINUX)
    return QStringLiteral("linux_subreaper");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows_job");
#else
    return QStringLiteral("unavailable");
#endif
}

ExternalToolProcess::ExternalToolProcess(QObject *parent)
    : QObject(parent)
    , _supervisorProgram(QCoreApplication::applicationFilePath())
    , _supervisor(new QProcess)
{
    _supervisor->setProcessChannelMode(QProcess::SeparateChannels);
    connect(_supervisor, &QProcess::readyReadStandardOutput, this, [this] {
        emit standardOutput(_supervisor->readAllStandardOutput());
    });
    connect(_supervisor, &QProcess::readyReadStandardError, this, [this] {
        emit standardError(_supervisor->readAllStandardError());
    });
    connect(_supervisor, &QProcess::finished, this, &ExternalToolProcess::onSupervisorFinished);
}

ExternalToolProcess::~ExternalToolProcess()
{
    disconnect(_supervisor, nullptr, this, nullptr);
    closeChannels(); // the end of the lifeline: the supervisor stops the tree
    if (_supervisor->state() == QProcess::NotRunning) {
        delete _supervisor;
        return;
    }
    // Never killed here: it ends once every descendant is gone and its proof
    // written, then goes. Should ngPost leave first, it is not killed either.
    connect(_supervisor, &QProcess::finished, _supervisor, &QObject::deleteLater);
}

bool ExternalToolProcess::start(const QString &program,
                                const QStringList &arguments,
                                const QString &workingDirectory,
                                const QString &proofPath)
{
    if (_state != State::Idle || program.isEmpty() || proofPath.isEmpty() || !openChannels())
        return false;
    _proofPath = proofPath;
    _supervisor->setWorkingDirectory(workingDirectory);
    _state = State::Launching;
    _supervisor->start(_supervisorProgram,
                       QStringList{ QString::fromLatin1(toolsupervisor::kSwitch),
                                    proofPath,
                                    _nonce,
                                    QStringLiteral("--"),
                                    program }
                           + arguments);
    if (!_supervisor->waitForStarted(5000)) {
        closeChannels();
        _state = State::Idle;
        return false;
    }
    listenToSupervisor();
    return true;
}

#if defined(Q_OS_LINUX)
bool ExternalToolProcess::openChannels()
{
    int fromSupervisor[2];
    int toSupervisor[2];
    if (::pipe2(fromSupervisor, O_CLOEXEC) != 0)
        return false;
    if (::pipe2(toSupervisor, O_CLOEXEC) != 0) {
        ::close(fromSupervisor[0]);
        ::close(fromSupervisor[1]);
        return false;
    }
    _childWrite = fromSupervisor[1];
    _childRead = toSupervisor[0];
    const int childWrite = _childWrite;
    const int childRead = _childRead;
    // In the child, between fork and exec: async-signal-safe calls only. The
    // ends are first moved above 3 and 4 so that placing one never closes the
    // other; dup2() clears close-on-exec on 3 and 4 only. The controller's ends
    // keep close-on-exec: the lifeline is held by this process alone.
    _supervisor->setChildProcessModifier([childWrite, childRead] {
        const int write = ::fcntl(childWrite, F_DUPFD_CLOEXEC, 10);
        const int read = ::fcntl(childRead, F_DUPFD_CLOEXEC, 10);
        if (write < 0 || read < 0 || ::dup2(write, 3) != 3 || ::dup2(read, 4) != 4)
            ::_exit(toolsupervisor::kSetupFailed);
    });
    _fromSupervisor = fromSupervisor[0];
    _toSupervisor = toSupervisor[1];
    _nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return true;
}

void ExternalToolProcess::listenToSupervisor()
{
    // The child holds its own copies by now.
    ::close(_childWrite);
    ::close(_childRead);
    _childWrite = -1;
    _childRead = -1;
    ::fcntl(_fromSupervisor, F_SETFL, ::fcntl(_fromSupervisor, F_GETFL) | O_NONBLOCK);
    _notifier = new QSocketNotifier(_fromSupervisor, QSocketNotifier::Read, this);
    connect(_notifier, &QSocketNotifier::activated, this, &ExternalToolProcess::onProtocol);
}
#elif defined(Q_OS_WIN)
bool ExternalToolProcess::openChannels()
{
    _nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Its name carries the nonce; only the supervisor's own pid is accepted.
    _server = new QLocalServer(this);
    _server->setSocketOptions(QLocalServer::UserAccessOption);
    _server->setMaxPendingConnections(1);
    if (!_server->listen(QStringLiteral("ngpost-tool-") + _nonce)) {
        closeChannels();
        return false;
    }
    connect(_server, &QLocalServer::newConnection, this, &ExternalToolProcess::onConnection);
    return true;
}

void ExternalToolProcess::listenToSupervisor()
{
    // The supervisor connects to the server opened by openChannels().
}
#else
bool ExternalToolProcess::openChannels()
{
    return false; // no proven supervision here yet (lot C5.5)
}

void ExternalToolProcess::listenToSupervisor()
{
}
#endif

void ExternalToolProcess::onConnection()
{
#if defined(Q_OS_WIN)
    while (QLocalSocket *socket = _server ? _server->nextPendingConnection() : nullptr) {
        ULONG client = 0;
        const auto pipe = reinterpret_cast<HANDLE>(socket->socketDescriptor());
        if (_socket || !::GetNamedPipeClientProcessId(pipe, &client)
            || static_cast<qint64>(client) != _supervisor->processId()) {
            socket->abort(); // not our supervisor: never listened to
            socket->deleteLater();
            continue;
        }
        _socket = socket;
        connect(_socket, &QLocalSocket::readyRead, this, &ExternalToolProcess::onProtocol);
        _server->close(); // nobody else gets in
        // What was asked before it connected (a stop) is not lost.
        if (!_queuedCommands.isEmpty()) {
            _socket->write(_queuedCommands);
            _socket->flush();
            _queuedCommands.clear();
        }
        onProtocol();
    }
#endif
}

void ExternalToolProcess::allowExecution()
{
    if (_state == State::AwaitingGo)
        sendCommand("go\n");
}

void ExternalToolProcess::stop()
{
    if (_state == State::Idle || _state == State::StoppedVerified || _state == State::Unproven)
        return;
    _state = State::Quiescing;
    sendCommand("stop\n");
}

void ExternalToolProcess::onProtocol()
{
#if defined(Q_OS_LINUX)
    char buffer[512];
    for (;;) {
        const ssize_t n = _fromSupervisor < 0
            ? -1
            : ::read(_fromSupervisor, buffer, sizeof buffer); // flawfinder: ignore
        if (n > 0) {
            _pending.append(buffer, n);
            continue;
        }
        if (n == 0 && _notifier)
            _notifier->setEnabled(false); // the supervisor closed it, or ended
        break;
    }
#elif defined(Q_OS_WIN)
    if (_socket)
        _pending += _socket->readAll();
#endif
    qsizetype eol = 0;
    while ((eol = _pending.indexOf('\n')) >= 0) {
        handleLine(_pending.left(eol));
        _pending.remove(0, eol + 1);
    }
}

void ExternalToolProcess::handleLine(const QByteArray &line)
{
    const QList<QByteArray> words = line.split(' ');
    const QByteArray &verb = words.value(0);
    if (verb == "ready" && words.size() >= 4) {
        const QJsonObject identity{ { QStringLiteral("capability"), capability() },
                                    { QStringLiteral("pid"), QString::fromLatin1(words.at(1)) },
                                    { QStringLiteral("starttime"),
                                      QString::fromLatin1(words.at(2)) },
                                    { QStringLiteral("boot"), QString::fromLatin1(words.at(3)) },
                                    { QStringLiteral("nonce"), _nonce },
                                    { QStringLiteral("proof"), _proofPath } };
        _identityJson = QString::fromUtf8(QJsonDocument(identity).toJson(QJsonDocument::Compact));
        _readyReceived = true;
        if (_state == State::Launching)
            _state = State::AwaitingGo;
        emit ready(capability(), _identityJson);
    } else if (verb == "started" && words.size() >= 2) {
        if (_state == State::AwaitingGo)
            _state = State::Running;
        emit started(words.at(1).toLongLong());
    } else if (verb == "exec_failed" && words.size() >= 2) {
        emit execFailed(words.at(1).toInt());
    } else if ((verb == "tool_exited" || verb == "finished") && words.size() >= 2) {
        _toolStatus = words.at(1).toInt();
        if (verb == "tool_exited")
            emit toolExited(_toolStatus);
    }
}

void ExternalToolProcess::onSupervisorFinished()
{
    onProtocol(); // what it said last
    closeChannels();
    bool verified = false;
    if (!_readyReceived) {
        // It ended before being able to run anything: the tool never existed.
        verified = true;
    } else {
        const auto status = provenStatus(QJsonDocument::fromJson(_identityJson.toUtf8()).object());
        verified = status.has_value();
        if (status)
            _toolStatus = *status;
    }
    _state = verified ? State::StoppedVerified : State::Unproven;
    emit stopped(verified, _toolStatus);
}

ExternalToolProcess::State ExternalToolProcess::proofState(const QString &identityJson)
{
    const QJsonObject identity = QJsonDocument::fromJson(identityJson.toUtf8()).object();
    if (identity.isEmpty())
        return State::Unproven;
    if (provenStatus(identity))
        return State::StoppedVerified;
    return supervisorAlive(identity) ? State::Running : State::Unproven;
}

void ExternalToolProcess::closeChannels()
{
    delete _notifier;
    _notifier = nullptr;
    // Windows: the end of the connection is the end of the lifeline.
    if (_socket)
        _socket->abort();
    delete _socket;
    _socket = nullptr;
    delete _server;
    _server = nullptr;
#if defined(Q_OS_LINUX)
    for (const int end : { _childWrite, _childRead })
        if (end >= 0)
            ::close(end);
    if (_fromSupervisor >= 0)
        ::close(_fromSupervisor);
    if (_toSupervisor >= 0)
        ::close(_toSupervisor);
#endif
    _fromSupervisor = -1;
    _toSupervisor = -1;
    _childWrite = -1;
    _childRead = -1;
    _queuedCommands.clear();
}

void ExternalToolProcess::sendCommand(const QByteArray &line)
{
#if defined(Q_OS_LINUX)
    if (_toSupervisor < 0)
        return;
    // A supervisor already gone must not take ngPost down with a SIGPIPE.
    sigset_t pipeSignal;
    sigset_t previous;
    sigemptyset(&pipeSignal);
    sigaddset(&pipeSignal, SIGPIPE);
    ::pthread_sigmask(SIG_BLOCK, &pipeSignal, &previous);
    const ssize_t n = ::write(_toSupervisor, line.constData(), static_cast<size_t>(line.size()));
    if (n < 0 && errno == EPIPE) {
        const timespec none = {};
        ::sigtimedwait(&pipeSignal, nullptr, &none);
    }
    ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
#elif defined(Q_OS_WIN)
    if (_socket) {
        _socket->write(line);
        _socket->flush();
    } else {
        _queuedCommands += line; // sent once the supervisor connects
    }
#else
    Q_UNUSED(line)
#endif
}

#ifdef NGPOST_TESTING
void ExternalToolProcess::dropProtocolForTest()
{
#if defined(Q_OS_LINUX)
    delete _notifier;
    _notifier = nullptr;
    if (_fromSupervisor >= 0)
        ::close(_fromSupervisor);
    _fromSupervisor = -1;
#elif defined(Q_OS_WIN)
    if (_socket) // its messages now go unread; commands and the lifeline stay
        disconnect(_socket, nullptr, this, nullptr);
#endif
}
#endif
