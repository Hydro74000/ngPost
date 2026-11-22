// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// QueueExecutor without any I/O: it records the commands and lets a test
// (or the J0 milestone) send the acknowledgements. Nothing is renamed,
// compressed, deleted or posted.
//
//========================================================================

#ifndef QUEUE_FAKEQUEUEEXECUTOR_H
#define QUEUE_FAKEQUEUEEXECUTOR_H

#include "queue/QueueExecutor.h"

#include <QList>

namespace queue
{

class FakeQueueExecutor : public QueueExecutor
{
    Q_OBJECT

public:
    struct Command
    {
        enum class Kind {
            Prepare,
            Activate,
            Pause,
            Resume,
            Interrupt,
            RetryFinalization,
            Abandon
        };

        Kind kind = Kind::Prepare;
        qint64 postId = 0;
        QString runId;
        QString setId;
        PrepStep step = PrepStep::Compress;
        Disposition disposition = Disposition::Keep;
    };

    using QueueExecutor::QueueExecutor;

    void prepareStep(qint64 postId,
                     const QString &runId,
                     const QString &setId,
                     PrepStep step) override;
    void activate(qint64 postId, const QString &runId, const QString &setId) override;
    void pauseTransfer(qint64 postId) override;
    void resumeTransfer(qint64 postId) override;
    void interrupt(qint64 postId, const QString &runId, Disposition disposition) override;
    void retryFinalization(qint64 postId, const QString &runId) override;
    void abandonPlanning(qint64 postId, const QString &runId) override;

    const QList<Command> &commands() const { return _commands; }
    void clearCommands() { _commands.clear(); }
    //! Last command of @p kind for @p postId, or nullptr.
    const Command *last(Command::Kind kind, qint64 postId) const;
    int count(Command::Kind kind, qint64 postId = 0) const;

    // Acknowledgements, built from the last matching command.
    bool ackStep(qint64 postId, bool ok = true, qint64 bytes = 0, const QString &error = {});
    bool ackTransferStarted(qint64 postId);
    bool ackTransferFinished(qint64 postId, bool success = true);
    bool ackFinalized(qint64 postId, bool ok = true, const QString &error = {});
    bool ackQuiesced(qint64 postId, bool ok = true, bool manifestEngaged = false);

private:
    QList<Command> _commands;
};

} // namespace queue

#endif // QUEUE_FAKEQUEUEEXECUTOR_H
