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

#include <limits>

namespace
{
//! Head and tail of a chatty command, so a script looping on stderr cannot
//! flood the log panel, nor grow in memory without bound.
constexpr int kMaxOutputBytes = 4096;
constexpr int kHalfOutput     = kMaxOutputBytes / 2;

int timerIntervalMs(int seconds)
{
    if (seconds <= 0)
        return 0;
    constexpr int kMaxSeconds = std::numeric_limits<int>::max() / 1000;
    return seconds > kMaxSeconds ? std::numeric_limits<int>::max() : seconds * 1000;
}
} // namespace

void PostCmdRunner::BoundedOutput::clear()
{
    head.clear();
    tail.clear();
    total = 0;
}

void PostCmdRunner::BoundedOutput::append(QByteArray const &chunk)
{
    total += chunk.size();

    qsizetype consumed = 0;
    if (head.size() < kHalfOutput) {
        consumed = qMin<qsizetype>(chunk.size(), kHalfOutput - head.size());
        head.append(chunk.left(consumed));
    }
    if (consumed < chunk.size()) {
        tail.append(chunk.mid(consumed));
        if (tail.size() > kHalfOutput)
            tail = tail.right(kHalfOutput); // only the end of a long stream matters
    }
}

QString PostCmdRunner::BoundedOutput::toString() const
{
    QString const headText = QString::fromLocal8Bit(head);
    if (tail.isEmpty())
        return headText.trimmed();

    const qint64 omitted = total - head.size() - tail.size();
    QString text = headText;
    if (omitted > 0)
        text += QCoreApplication::translate("PostCmdRunner", "\n[... %1 bytes omitted ...]\n")
                    .arg(omitted);
    text += QString::fromLocal8Bit(tail);
    return text.trimmed();
}

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
    task.settings  = _settings;
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
        _uploader->release();
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
        QString jsonError;
        _current.jsonPath = _writeJsonFile(_current.data, _current.settings, &jsonError);
        _current.data.jsonPath = _current.jsonPath;
        if (_current.jsonPath.isEmpty()) {
            _reportFailure(tr("Could not create the temporary post JSON: %1").arg(jsonError));
            // A hook which was promised NGPOST_JSON must not be launched with
            // an empty or insecure path. An independent NZB upload can still
            // proceed, then this task is drained normally.
            _current.commands.clear();
        }
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

    if (_current.settings.uploadTimeoutSec > 0)
        _timer->start(timerIntervalMs(_current.settings.uploadTimeoutSec));
    _uploader->startUpload(_current.uploadUrl);
}

void PostCmdRunner::onUploadDone() { _finishUpload(QString()); }

void PostCmdRunner::_finishUpload(QString const &errorMsg)
{
    _timer->stop();
    if (_uploader) {
        _uploader->disconnect(this);
        // Closed here and not by the destructor: deleteLater() only frees it on
        // the next event loop turn, and the next command may well be the one
        // that moves the nzb. Deleting it outright is not an option, we may be
        // standing in one of its own slots.
        _uploader->release();
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

    // Log what is really run, not the template: seeing the substituted values
    // is the whole point of the line, and it is what ngPost used to print.
    _runningCmd = _redact(args.join(QLatin1Char(' ')));

    QString const cmd = args.takeFirst();

    _process = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    PostInfoTemplate::applyEnvironment(env, _current.data, _current.settings.exposeSecrets);
    _process->setProcessEnvironment(env);
    _process->setProcessChannelMode(QProcess::SeparateChannels);

    _stdout.clear();
    _stderr.clear();
    // Drained as it comes, not at the end: a hook printing in a loop would
    // otherwise fill the pipe buffers until something gives.
    connect(_process, &QProcess::readyReadStandardOutput, this, &PostCmdRunner::onReadyReadStdOut);
    connect(_process, &QProcess::readyReadStandardError, this, &PostCmdRunner::onReadyReadStdErr);
    connect(_process, &QProcess::errorOccurred, this, &PostCmdRunner::onProcessError);
    connect(_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus status) {
                onProcessFinished(exitCode, static_cast<int>(status));
            });

    emit log(tr("NZB_POST_CMD: %1").arg(_runningCmd));
    QProcess *startedProcess = _process;
    startedProcess->start(cmd, args);

    // FailedToStart may be delivered synchronously on some Qt/platform
    // combinations. Its slot can already have advanced the FIFO and replaced
    // _process, in which case this frame must not reset the new task's timer.
    if (_process != startedProcess)
        return;

    if (_current.settings.cmdTimeoutSec > 0)
        _timer->start(timerIntervalMs(_current.settings.cmdTimeoutSec));
}

void PostCmdRunner::onReadyReadStdOut()
{
    if (_process)
        _stdout.append(_process->readAllStandardOutput());
}

void PostCmdRunner::onReadyReadStdErr()
{
    if (_process)
        _stderr.append(_process->readAllStandardError());
}

void PostCmdRunner::onProcessError()
{
    if (!_process || _canceled)
        return;
    // A crashed process also emits finished(); let that path drain and report
    // both channels with the proper crash diagnostic. FailedToStart does not.
    const QProcess::ProcessError processError = _process->error();
    if (processError == QProcess::Crashed)
        return;
    // The most common real world failure: a typo in the path, or a script
    // without the executable bit. It used to produce absolutely nothing.
    QString const msg = tr("NZB_POST_CMD could not run '%1': %2")
                            .arg(_runningCmd, _process->errorString());
    _timer->stop();
    _process->disconnect(this);
    if (processError != QProcess::FailedToStart) {
        // A read/write error can be delivered while the child is still alive.
        // Do not overlap it with the next FIFO entry.
        _process->kill();
        _process->waitForFinished(2000);
        _stdout.append(_process->readAllStandardOutput());
        _stderr.append(_process->readAllStandardError());
    }
    QStringList details;
    const QString err = _stderr.toString();
    const QString out = _stdout.toString();
    if (!err.isEmpty())
        details << err;
    if (!out.isEmpty())
        details << out;
    _process->deleteLater();
    _process = nullptr;
    const QString output = _redact(details.join(QLatin1Char('\n')));
    _reportFailure(output.isEmpty() ? msg : msg + QLatin1Char('\n') + output);
    _runNextCommand();
}

void PostCmdRunner::onProcessFinished(int exitCode, int exitStatus)
{
    if (!_process || _canceled)
        return;

    _timer->stop();
    _stdout.append(_process->readAllStandardOutput()); // whatever is left
    _stderr.append(_process->readAllStandardError());
    QString const err = _stderr.toString();
    QString const out = _stdout.toString();
    QStringList details;
    if (!err.isEmpty())
        details << err;
    if (!out.isEmpty())
        details << out;
    QString const output = _redact(details.join(QLatin1Char('\n')));
    _process->disconnect(this);
    _process->deleteLater();
    _process = nullptr;

    if (exitStatus == static_cast<int>(QProcess::CrashExit))
        _reportFailure(tr("NZB_POST_CMD crashed: %1\n%2").arg(_runningCmd, output));
    else if (exitCode != 0)
        _reportFailure(tr("NZB_POST_CMD failed (exit code %1): %2\n%3")
                           .arg(exitCode)
                           .arg(_runningCmd, output));
    else {
        if (!out.isEmpty())
            emit log(_redact(out));
        if (!err.isEmpty())
            emit log(_redact(err));
    }

    _runNextCommand();
}

void PostCmdRunner::onTimeout()
{
    if (_process) {
        QString const msg = tr("NZB_POST_CMD timed out after %1s: %2")
                                .arg(_current.settings.cmdTimeoutSec)
                                .arg(_runningCmd);
        _process->disconnect(this);
        _process->kill();
        _process->waitForFinished(2000);
        _stdout.append(_process->readAllStandardOutput());
        _stderr.append(_process->readAllStandardError());
        QStringList details;
        QString const err = _stderr.toString();
        QString const out = _stdout.toString();
        if (!err.isEmpty())
            details << err;
        if (!out.isEmpty())
            details << out;
        _process->deleteLater();
        _process = nullptr;
        QString const output = _redact(details.join(QLatin1Char('\n')));
        _reportFailure(output.isEmpty() ? msg : msg + QLatin1Char('\n') + output);
        _runNextCommand();
    } else if (_uploader) {
        // Without this the whole shutdown waits forever on a stalled upload.
        _finishUpload(tr("NZB_UPLOAD_URL timed out after %1s")
                          .arg(_current.settings.uploadTimeoutSec));
    }
}

void PostCmdRunner::_reportFailure(QString const &msg)
{
    if (_current.settings.failIsError)
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

QString PostCmdRunner::_writeJsonFile(PostInfoData const &data,
                                      Settings const     &settings,
                                      QString            *error) const
{
    QString const path =
        QDir::temp().filePath(QStringLiteral("ngPost_%1_%2.json")
                                  .arg(QCoreApplication::applicationPid())
                                  .arg(data.historyPostId));

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return QString();
    }
    // Owner only, set before the first byte: this file describes the post and
    // may hold its password.
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (error)
            *error = tr("could not restrict the file permissions");
        file.cancelWriting();
        return QString();
    }
    PostInfoData jsonData = data;
    jsonData.jsonPath = path;
    const QByteArray payload = PostInfoTemplate::toJson(jsonData, settings.exposeSecrets);
    if (file.write(payload) != payload.size()) {
        if (error)
            *error = file.errorString();
        file.cancelWriting();
        return QString();
    }
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return QString();
    }
    return path;
}

QString PostCmdRunner::_redact(QString const &text) const
{
    return PostInfoTemplate::redactSecrets(text, _current.data);
}
