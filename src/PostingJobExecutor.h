// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// The QueueExecutor that drives real PostingJobs (plan_queue.md §3.4, lot C3).
//
// A runtime exists only while it works: from the first preparation step to
// Collect, then again from the activation to the end of the transfer. What a
// preparation produced is kept as a PreparedManifest in between, so an entry
// waiting in Prepared holds no job. Every signal of a job is checked against
// the (post, run, artifact set, phase, job instance) it was issued for, and a
// run ends once even though PostingJob may say it twice.
//
// Lot C3 limits: keeping or abandoning a running TRANSFER is not supported
// before C4 (quiesced(false)), nor are the two finalization commands; the
// local end of a transfer is the historical _finishPosting().
//
//========================================================================

#ifndef POSTINGJOBEXECUTOR_H
#define POSTINGJOBEXECUTOR_H

#include "PostingJobExecution.h"
#include "queue/QueueEntry.h"
#include "queue/QueueExecutor.h"

#include <QHash>
#include <QPointer>

#include <functional>

class PostingJob;

namespace queue
{
class PostQueue;
}

class PostingJobExecutor : public queue::QueueExecutor
{
    Q_OBJECT

public:
    //! Builds the job of an entry: frozen options, secret, no new draw.
    using Factory =
        std::function<PostingJob *(const queue::QueueEntry &, const JobExecutionContext &)>;

    PostingJobExecutor(const queue::PostQueue *queue,
                       Factory factory,
                       QString ownerId = {},
                       QObject *parent = nullptr);
    ~PostingJobExecutor() override;

    void prepareStep(qint64 postId,
                     const QString &runId,
                     const QString &setId,
                     queue::PrepStep step) override;
    void activate(qint64 postId, const QString &runId, const QString &setId) override;
    void pauseTransfer(qint64 postId) override;
    void resumeTransfer(qint64 postId) override;
    void interrupt(qint64 postId, const QString &runId, queue::Disposition disposition) override;
    void retryFinalization(qint64 postId, const QString &runId) override;
    void abandonPlanning(qint64 postId, const QString &runId) override;

    //! The live job of @p postId, or null.
    PostingJob *runtime(qint64 postId) const;
    //! What the preparation of (post, run, set) left, if it is still held.
    PreparedManifest manifest(qint64 postId, const QString &runId, const QString &setId) const;

private:
    enum class Phase {
        Preparing,
        Transferring,
        Quiescing
    };
    struct Runtime
    {
        QPointer<PostingJob> job;
        QString runId;
        QString setId;
        Phase phase = Phase::Preparing;
        bool ended = false; //!< the transfer end was already reported
    };

    static QString key(qint64 postId, const QString &runId, const QString &setId);
    PostingJob *createJob(qint64 postId, const QString &runId, const QString &setId);
    bool isCurrent(qint64 postId, const PostingJob *job, const QString &runId, Phase phase) const;
    void releaseRuntime(qint64 postId);
    void onStepEnded(qint64 postId,
                     PostingJob *job,
                     const QString &runId,
                     const QString &setId,
                     queue::PrepStep step,
                     bool ok,
                     const QString &error,
                     qint64 bytes);
    void onTransferEnded(qint64 postId, PostingJob *job, const QString &runId);
    //! Acknowledgements leave on the event loop: never inside the command.
    void later(std::function<void()> emitter);

    const queue::PostQueue *_queue;
    Factory _factory;
    QString _ownerId;
    QHash<qint64, Runtime> _runtimes;
    QHash<QString, PreparedManifest> _manifests;
};

#endif // POSTINGJOBEXECUTOR_H
