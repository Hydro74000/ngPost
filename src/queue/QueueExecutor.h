// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// What the scheduler asks of a runtime, and the acknowledgements it waits
// for (plan_queue.md §3.4). Every acknowledgement carries (post id, run id)
// so one from a replaced run or artifact set can be rejected. A slot stays
// taken until its runtime acknowledged quiescence or the end of its work.
//
// Implemented by the PostingJob adapter (lot C3) and FakeQueueExecutor.
//
//========================================================================

#ifndef QUEUE_QUEUEEXECUTOR_H
#define QUEUE_QUEUEEXECUTOR_H

#include "queue/QueueTypes.h"

#include <QObject>
#include <QString>

namespace queue
{

class QueueExecutor : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~QueueExecutor() override = default;

    //! Runs one preparation step of artifact set @p setId.
    virtual void prepareStep(qint64 postId,
                             const QString &runId,
                             const QString &setId,
                             PrepStep step) = 0;
    //! Starts the transfer of a ready entry (its verified set, if any).
    virtual void activate(qint64 postId, const QString &runId, const QString &setId) = 0;
    virtual void pauseTransfer(qint64 postId) = 0;
    virtual void resumeTransfer(qint64 postId) = 0;
    //! Asks the runtime of @p postId to stop. Keep and Abandon end with
    //! quiesced(); Stop goes through transferFinished() and finalized().
    virtual void interrupt(qint64 postId, const QString &runId, Disposition disposition) = 0;
    //! A blocked finalization: try F2/F3 again, or abandon the planning (facts
    //! kept, no deliverable). Both end with finalized().
    virtual void retryFinalization(qint64 postId, const QString &runId) = 0;
    virtual void abandonPlanning(qint64 postId, const QString &runId) = 0;

signals:
    void stepFinished(qint64 postId,
                      const QString &runId,
                      const QString &setId,
                      queue::PrepStep step,
                      bool ok,
                      const QString &error,
                      qint64 bytes);
    void transferStarted(qint64 postId, const QString &runId);
    void transferFinished(qint64 postId, const QString &runId, bool success);
    void finalized(qint64 postId, const QString &runId, bool ok, const QString &error);
    //! Barriers passed: tools stopped, connections closed, article events flushed.
    void quiesced(qint64 postId, const QString &runId, bool ok, bool manifestEngaged);
    //! Live progress, for views only; never used for a decision.
    void progress(qint64 postId, int percent, qint64 bytesPerSecond);
};

} // namespace queue

#endif // QUEUE_QUEUEEXECUTOR_H
