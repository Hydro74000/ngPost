// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// The only authority that starts work (invariant 9, plan_queue.md §3.4):
//   - one transfer slot, given in strict order to the first eligible entry,
//     and only when that entry is ready;
//   - one preparation slot: first the preparation the head (or the "post
//     now" target) requires, otherwise a speculative one under budget;
//   - the "post now" intent: prepare the target while the active entry keeps
//     posting, then interrupt-and-keep the active one and switch.
//
// Runtimes are reached through a QueueExecutor; every acknowledgement is
// checked against the run it was issued for.
//
//========================================================================

#ifndef QUEUE_QUEUESCHEDULER_H
#define QUEUE_QUEUESCHEDULER_H

#include "queue/PostQueue.h"
#include "queue/QueueExecutor.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>

#include <functional>

namespace queue
{

class QueueScheduler : public QObject
{
    Q_OBJECT

public:
    //! Answer of the VPN gate for the entry about to transfer.
    enum class Admission {
        Proceed,
        Wait,
        Blocked
    };

    //! What a failed preparation step does to its entry. Block keeps it,
    //! durably, for a retry from the queue view; Retire ends it as failed, as
    //! long as no view can show a blocked entry (plan_queue.md, until C7a).
    enum class PreparationFailure {
        Block,
        Retire
    };

    struct Budget
    {
        int preparedMax = 2;                       //!< QUEUE_PREPARED_MAX
        qint64 marginBytes = 1024LL * 1024 * 1024; //!< QUEUE_TMP_MARGIN
        //! Free bytes under a temporary root; unset means "unlimited".
        std::function<qint64(const QString &root)> freeBytes;
        //! Temporary root an entry would prepare under (TMP_DIR or TMP_RAM).
        std::function<QString(const QueueEntry &entry)> rootFor;
    };

    QueueScheduler(PostQueue *queue,
                   QueueExecutor *executor,
                   Budget budget,
                   QObject *parent = nullptr);

    void setAdmission(std::function<Admission(const QueueEntry &)> admission);
    void setPreparationFailurePolicy(PreparationFailure policy) { _preparationFailure = policy; }
    //! Side-effect free VPN check of a "post now" switch; Blocked drops the intent.
    void setSwitchPreflight(
        std::function<Admission(const QueueEntry &target, const QueueEntry &active)> preflight);

    //! Estimated temporary bytes a preparation of @p entry needs, margin excluded.
    static qint64 estimatedPreparationBytes(const QueueEntry &entry);

    //! A queue write failed: nothing new starts until retryAfterTechnicalPause()
    //! has applied every acknowledgement left waiting. Distinct from the user's
    //! pause, which the queue itself records.
    bool isTechnicallyPaused() const { return _technicalPause; }
    //! A close that keeps the queue: nothing new starts, every running
    //! preparation, and the transfer unless @p letTransferFinish, is stopped
    //! and kept for a restart; engaged finalizations go on. Unlike a pause, a
    //! transfer left to finish is not held.
    void keepRunningForClose(bool letTransferFinish = false);
    //! The close failed: the queue may start things again (once resumed).
    void cancelClose();
    bool isClosing() const { return _closing; }
    QString technicalPauseReason() const { return _technicalReason; }

signals:
    void technicalPauseChanged(bool paused, const QString &reason);

public slots:
    //! Looks at the queue and starts what may start. Idempotent.
    void reevaluate();
    //! Applies, in order, the acknowledgements a failed write left waiting; the
    //! technical pause ends once they all went through.
    void retryAfterTechnicalPause();

private slots:
    void scheduleReevaluate();
    void onStepFinished(qint64 postId,
                        const QString &runId,
                        const QString &setId,
                        queue::PrepStep step,
                        bool ok,
                        const QString &error,
                        qint64 bytes);
    void onTransferFinished(qint64 postId,
                            const QString &runId,
                            const queue::AttemptResult &result);
    void onFinalized(qint64 postId, const QString &runId, bool ok, const QString &error);
    void onQuiesced(qint64 postId,
                    const QString &runId,
                    bool ok,
                    bool manifestEngaged,
                    qint64 activeMs);
    void onRuntimeActionRequested(qint64 postId, queue::RuntimeRequest request);
    void onPausedChanged(bool paused);

private:
    //! Why a runtime was asked to stop, to know what its quiescence means.
    enum class PendingStop {
        Hold,
        SwitchActive,
        Preempt,
        CancelPreparation,
        Technical
    };

    bool handleIntent();
    void continuePreparation(const QueueEntry &entry);
    void startPreparation();
    void activateHead();
    const QueueEntry *requiredPreparation() const;
    const QueueEntry *speculativePreparation() const;
    bool spaceAllows(const QueueEntry &entry) const;
    int preparedCount() const;
    QList<PrepStep> stepsFor(const QueueEntry &entry) const;
    ArtifactSetRef newArtifactSet(const QueueEntry &entry) const;
    bool requestStop(const QueueEntry &entry, PendingStop why, Disposition disposition);
    bool isCurrent(qint64 postId, const QString &runId) const;
    bool recordStep(qint64 postId, PrepStep step, bool ok, const QString &error, qint64 bytes);
    void holdUnprovenStop(qint64 postId,
                          const QString &runId,
                          PendingStop why,
                          const QString &reason);
    bool recordQuiesced(qint64 postId,
                        const QString &runId,
                        PendingStop why,
                        bool manifest,
                        bool wasTransfer,
                        qint64 activeMs);
    void enterTechnicalPause(const QString &reason);
    //! Applies @p write now, or after the acknowledgements already waiting;
    //! a failure keeps it waiting and pauses the queue technically.
    void applyInOrder(std::function<bool()> write);

    PostQueue *_queue;
    QueueExecutor *_executor;
    Budget _budget;
    std::function<Admission(const QueueEntry &)> _admission;
    std::function<Admission(const QueueEntry &, const QueueEntry &)> _preflight;

    QHash<qint64, QList<PrepStep>> _remainingSteps; //!< preparation slot: steps left
    QHash<qint64, qint64> _preparedBytes;           //!< bytes produced so far
    qint64 _stepRunning = 0;                        //!< post id whose step is running
    QHash<qint64, PendingStop> _pendingStops;
    qint64 _pausedTransfer = 0;
    bool _closing = false; //!< keepRunningForClose(): nothing new starts
    bool _scheduled = false;
    PreparationFailure _preparationFailure = PreparationFailure::Block;
    bool _technicalPause = false;
    QString _technicalReason;
    QList<std::function<bool()>> _waitingWrites; //!< acknowledgements not yet durable
    QSet<qint64> _stuck; //!< runtimes whose stop is not proven: they keep their slot
    QSet<QString> _startedRuns; //!< runs whose transfer really started
};

} // namespace queue

#endif // QUEUE_QUEUESCHEDULER_H
