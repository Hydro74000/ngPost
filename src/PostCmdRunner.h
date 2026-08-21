// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Runs what has to happen after a post: the nzb upload, then the post
// commands.
//
// This exists because firing the processes and forgetting about them does not
// work: in command line mode ngPost used to quit right after starting them, so
// the processes were destroyed mid-flight, and a command that failed to even
// start said nothing at all.
//
//========================================================================

#ifndef POSTCMDRUNNER_H
#define POSTCMDRUNNER_H

#include "postinfo/PostInfoData.h"

#include <QObject>
#include <QQueue>
#include <QStringList>
#include <QUrl>

class QNetworkAccessManager;
class QProcess;
class QTimer;
class FileUploader;

class PostCmdRunner : public QObject
{
    Q_OBJECT

public:
    struct Settings
    {
        int  cmdTimeoutSec    = 0;   //!< per command, 0 = no limit
        int  uploadTimeoutSec = 300; //!< the only guard of the shutdown barrier
        bool failIsError      = false; //!< should a failed command fail the run?
        bool exposeSecrets    = false; //!< put the password in the env and the json?
    };

    explicit PostCmdRunner(QNetworkAccessManager &netMgr, QObject *parent = nullptr);
    ~PostCmdRunner() override;

    void setSettings(Settings const &settings);

    //! Queues the end of one post. Everything needed is copied: the job that
    //! produced it is deleted long before its commands are done.
    void enqueue(PostInfoData const &data, QStringList const &commands, QUrl const &uploadUrl);

    //! Nothing left to upload and nothing left to run.
    bool isIdle() const;

    //! Kills what is running and drops what is queued. Used when the user
    //! interrupts ngPost or closes the window.
    void cancelAll();

signals:
    void log(QString msg);
    void error(QString msg);
    //! The queue just drained: whoever waits to quit or to shut down the
    //! machine can now go ahead.
    void idle();

private slots:
    void onUploadDone();
    void onReadyReadStdOut();
    void onReadyReadStdErr();
    void onProcessFinished(int exitCode, int exitStatus);
    void onProcessError();
    void onTimeout();

private:
    struct Task
    {
        PostInfoData data;
        QStringList  commands;
        QUrl         uploadUrl;
        QString      jsonPath; //!< temporary, deleted when the task is over
    };

    void _startNextTask();
    void _startUpload();
    void _finishUpload(QString const &errorMsg);
    void _runNextCommand();
    void _finishTask();
    void _reportFailure(QString const &msg);

    //! Head and tail of a stream, bounded whatever the command decides to
    //! print. Reading only at the end would let a chatty hook grow the pipe
    //! buffers until memory runs out.
    struct BoundedOutput
    {
        QByteArray head;
        QByteArray tail;
        qint64     total = 0;

        void    clear();
        void    append(QByteArray const &chunk);
        QString toString() const;
    };
    QString _writeJsonFile(PostInfoData const &data) const;
    QString _redact(QString const &text) const;

    QNetworkAccessManager &_netMgr;
    Settings               _settings;

    QQueue<Task> _queue;
    bool         _hasCurrent = false;
    Task         _current;
    int          _commandIndex = 0;

    FileUploader *_uploader = nullptr;
    QProcess     *_process  = nullptr;
    QTimer       *_timer    = nullptr;
    QString       _runningCmd;
    BoundedOutput _stdout;
    BoundedOutput _stderr;
    bool          _canceled = false;
};

#endif // POSTCMDRUNNER_H
