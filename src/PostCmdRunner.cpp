// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>

#include "PostCmdRunner.h"

#include "FileUploader.h"
#include "postinfo/PostInfoTemplate.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSaveFile>
#include <QTimer>

namespace
{
//! Head and tail of a chatty command, so a script looping on stderr cannot
//! flood the log panel.
constexpr int kMaxOutputBytes = 4096;
} // namespace

PostCmdRunner::PostCmdRunner(QNetworkAccessManager &netMgr, QObject *parent)
    : QObject(parent), _netMgr(netMgr)
{
    _timer = new QTimer(this);
    _timer->setSingleShot(true);
    connect(_timer, &QTimer::timeout, this, &PostCmdRunner::onTimeout);

    // Covers every way out: Ctrl+C (routed to quit()), the GUI being closed,
    // and a normal end of run.
    connect(qApp, &QCoreApplication::aboutToQuit, this, &PostCmdRunner::cancelAll);
}

PostCmdRunner::~PostCmdRunner() { cancelAll(); }

void PostCmdRunner::setSettings(Settings const &settings) { _settings = settings; }

void PostCmdRunner::enqueue(PostInfoData const &data,
                            QStringList const  &commands,
                            QUrl const         &uploadUrl)
{
    if (commands.isEmpty() && uploadUrl.isEmpty())
        return;

    Task task;
    task.data      = data; // a copy: the job is deleted well before we are done
    task.commands  = commands;
    task.uploadUrl = uploadUrl;
    _queue.enqueue(task);

    if (!_hasCurrent)
        _startNextTask();
}

bool PostCmdRunner::isIdle() const { return !_hasCurrent && _queue.isEmpty(); }

void PostCmdRunner::cancelAll()
{
    if (isIdle() && !_process && !_uploader)
        return;

    _canceled = true;
    _queue.clear();
    _timer->stop();

    if (_process) {
        _process->disconnect(this);
        _process->kill();
        _process->waitForFinished(2000);
        _process->deleteLater();
        _process = nullptr;
    }
    if (_uploader) {
        _uploader->disconnect(this);
        _uploader->deleteLater();
        _uploader = nullptr;
    }
    if (_hasCurrent) {
        if (!_current.jsonPath.isEmpty())
            QFile::remove(_current.jsonPath);
        _hasCurrent = false;
        _current    = Task();
    }
    _canceled = false;
    emit idle();
}

void PostCmdRunner::_startNextTask()
{
    if (_queue.isEmpty()) {
        _hasCurrent = false;
        emit idle();
        return;
    }

    _current      = _queue.dequeue();
    _hasCurrent   = true;
    _commandIndex = 0;

    if (!_current.commands.isEmpty()) {
        _current.jsonPath      = _writeJsonFile(_current.data);
        _current.data.jsonPath = _current.jsonPath;
    }

    // Upload first: a command that moves or renames the nzb would otherwise
    // race the uploader reading it.
    if (!_current.uploadUrl.isEmpty())
        _startUpload();
    else
        _runNextCommand();
}

void PostCmdRunner::_startUpload()
{
    _uploader = new FileUploader(_netMgr, _current.data.nzbPath);
    connect(_uploader, &FileUploader::error, this, [this](QString const &msg) {
        // Same reporting as before: an upload failure marks the run in error.
        emit error(msg);
    });
    connect(_uploader, &FileUploader::log, this, [this](QString const &msg, bool) {
        emit log(msg);
    });
    connect(_uploader, &FileUploader::readyToDie, this, &PostCmdRunner::onUploadDone);

    if (_settings.uploadTimeoutSec > 0)
        _timer->start(_settings.uploadTimeoutSec * 1000);
    _uploader->startUpload(_current.uploadUrl);
}

void PostCmdRunner::onUploadDone() { _finishUpload(QString()); }

void PostCmdRunner::_finishUpload(QString const &errorMsg)
{
    _timer->stop();
    if (_uploader) {
        _uploader->disconnect(this);
        _uploader->deleteLater();
        _uploader = nullptr;
    }
    if (!errorMsg.isEmpty())
        emit error(errorMsg);

    if (_canceled)
        return;
    _runNextCommand();
}

void PostCmdRunner::_runNextCommand()
{
    if (_commandIndex >= _current.commands.size()) {
        _finishTask();
        return;
    }

    QString const tmpl = _current.commands.at(_commandIndex++);

    // Split FIRST, substitute after: a value holding spaces or quotes then
    // stays exactly one argument instead of breaking the command line.
    QStringList args = QProcess::splitCommand(tmpl);
    if (args.isEmpty()) {
        _runNextCommand();
        return;
    }

    QStringList unknown;
    args = PostInfoTemplate::renderArguments(args, _current.data, true, &unknown);

    QString const cmd = args.takeFirst();
    _runningCmd       = _redact(tmpl);

    _process = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    PostInfoTemplate::applyEnvironment(env, _current.data, _settings.exposeSecrets);
    _process->setProcessEnvironment(env);
    _process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(_process, &QProcess::errorOccurred, this, &PostCmdRunner::onProcessError);
    connect(_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus status) {
                onProcessFinished(exitCode, static_cast<int>(status));
            });

    emit log(tr("NZB_POST_CMD: %1").arg(_runningCmd));
    _process->start(cmd, args);

    if (_settings.cmdTimeoutSec > 0)
        _timer->start(_settings.cmdTimeoutSec * 1000);
}

void PostCmdRunner::onProcessError()
{
    if (!_process || _canceled)
        return;
    // The most common real world failure: a typo in the path, or a script
    // without the executable bit. It used to produce absolutely nothing.
    QString const msg = tr("NZB_POST_CMD could not run '%1': %2")
                            .arg(_runningCmd, _process->errorString());
    _timer->stop();
    _process->disconnect(this);
    _process->deleteLater();
    _process = nullptr;
    _reportFailure(msg);
    _runNextCommand();
}

void PostCmdRunner::onProcessFinished(int exitCode, int exitStatus)
{
    if (!_process || _canceled)
        return;

    _timer->stop();
    QString const err = _tail(_process->readAllStandardError());
    QString const out = _tail(_process->readAllStandardOutput());
    _process->disconnect(this);
    _process->deleteLater();
    _process = nullptr;

    if (exitStatus == static_cast<int>(QProcess::CrashExit))
        _reportFailure(tr("NZB_POST_CMD crashed: %1\n%2").arg(_runningCmd, _redact(err)));
    else if (exitCode != 0)
        _reportFailure(tr("NZB_POST_CMD failed (exit code %1): %2\n%3")
                           .arg(exitCode)
                           .arg(_runningCmd, _redact(err)));
    else if (!out.isEmpty())
        emit log(_redact(out));

    _runNextCommand();
}

void PostCmdRunner::onTimeout()
{
    if (_process) {
        QString const msg = tr("NZB_POST_CMD timed out after %1s: %2")
                                .arg(_settings.cmdTimeoutSec)
                                .arg(_runningCmd);
        _process->disconnect(this);
        _process->kill();
        _process->waitForFinished(2000);
        _process->deleteLater();
        _process = nullptr;
        _reportFailure(msg);
        _runNextCommand();
    } else if (_uploader) {
        // Without this the whole shutdown waits forever on a stalled upload.
        _finishUpload(tr("NZB_UPLOAD_URL timed out after %1s").arg(_settings.uploadTimeoutSec));
    }
}

void PostCmdRunner::_reportFailure(QString const &msg)
{
    if (_settings.failIsError)
        emit error(msg);
    else
        emit log(msg); // visible, but the post itself did go out
}

void PostCmdRunner::_finishTask()
{
    if (!_current.jsonPath.isEmpty())
        QFile::remove(_current.jsonPath);
    _current    = Task();
    _hasCurrent = false;
    _startNextTask();
}

QString PostCmdRunner::_writeJsonFile(PostInfoData const &data) const
{
    QString const path =
        QDir::temp().filePath(QStringLiteral("ngPost_%1_%2.json")
                                  .arg(QCoreApplication::applicationPid())
                                  .arg(data.historyPostId));

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    // Owner only, set before the first byte: this file describes the post and
    // may hold its password.
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(PostInfoTemplate::toJson(data, _settings.exposeSecrets));
    if (!file.commit())
        return QString();
    return path;
}

QString PostCmdRunner::_redact(QString const &text) const
{
    return PostInfoTemplate::redactSecrets(text, _current.data);
}

QString PostCmdRunner::_tail(QByteArray const &output) const
{
    if (output.size() <= kMaxOutputBytes)
        return QString::fromLocal8Bit(output).trimmed();

    QString const head = QString::fromLocal8Bit(output.left(kMaxOutputBytes / 2));
    QString const tail = QString::fromLocal8Bit(output.right(kMaxOutputBytes / 2));
    return head + tr("\n[... %1 bytes omitted ...]\n").arg(output.size() - kMaxOutputBytes) + tail;
}
