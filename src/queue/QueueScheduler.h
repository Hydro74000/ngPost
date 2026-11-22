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
    //! Side-effect free VPN check of a "post now" switch; Blocked drops the intent.
    void setSwitchPreflight(
        std::function<Admission(const QueueEntry &target, const QueueEntry &active)> preflight);

    //! Estimated temporary bytes a preparation of @p entry needs, margin excluded.
    static qint64 estimatedPreparationBytes(const QueueEntry &entry);

public slots:
    //! Looks at the queue and starts what may start. Idempotent.
    void reevaluate();

private slots:
    void scheduleReevaluate();
    void onStepFinished(qint64 postId,
                        const QString &runId,
                        const QString &setId,
                        queue::PrepStep step,
                        bool ok,
                        const QString &error,
                        qint64 bytes);
    void onTransferFinished(qint64 postId, const QString &runId, bool success);
    void onFinalized(qint64 postId, const QString &runId, bool ok, const QString &error);
    void onQuiesced(qint64 postId, const QString &runId, bool ok, bool manifestEngaged);
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
    void requestStop(const QueueEntry &entry, PendingStop why, Disposition disposition);
    bool isCurrent(qint64 postId, const QString &runId) const;

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
    bool _scheduled = false;
};

} // namespace queue

#endif // QUEUE_QUEUESCHEDULER_H
