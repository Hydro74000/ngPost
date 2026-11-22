// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// See QueueScheduler.h. Handlers of acknowledgements only record facts in the
// queue and schedule a re-evaluation; commands to the runtimes are issued from
// reevaluate() alone, so an acknowledgement emitted synchronously can never
// re-enter a decision half-made.
//
//========================================================================

#include "QueueScheduler.h"

#include <QUuid>

namespace queue
{

QueueScheduler::QueueScheduler(PostQueue *queue,
                               QueueExecutor *executor,
                               Budget budget,
                               QObject *parent)
    : QObject(parent)
    , _queue(queue)
    , _executor(executor)
    , _budget(std::move(budget))
{
    connect(_queue, &PostQueue::changed, this, &QueueScheduler::scheduleReevaluate);
    connect(_queue,
            &PostQueue::runtimeActionRequested,
            this,
            &QueueScheduler::onRuntimeActionRequested);
    connect(_queue, &PostQueue::pausedChanged, this, &QueueScheduler::onPausedChanged);
    connect(_executor, &QueueExecutor::stepFinished, this, &QueueScheduler::onStepFinished);
    connect(_executor, &QueueExecutor::transferFinished, this, &QueueScheduler::onTransferFinished);
    connect(_executor, &QueueExecutor::finalized, this, &QueueScheduler::onFinalized);
    connect(_executor, &QueueExecutor::quiesced, this, &QueueScheduler::onQuiesced);
}

void QueueScheduler::setAdmission(std::function<Admission(const QueueEntry &)> admission)
{
    _admission = std::move(admission);
}

void QueueScheduler::setSwitchPreflight(
    std::function<Admission(const QueueEntry &, const QueueEntry &)> preflight)
{
    _preflight = std::move(preflight);
}

qint64 QueueScheduler::estimatedPreparationBytes(const QueueEntry &entry)
{
    const PostingJobOptions &job = entry.options.job;
    qint64 bytes = 0;
    if (job.doCompress)
        bytes += entry.sourceBytes;
    if (job.doPar2)
        bytes += entry.sourceBytes * static_cast<qint64>(job.par2Pct) / 100;
    return bytes;
}

void QueueScheduler::scheduleReevaluate()
{
    if (_scheduled)
        return;
    _scheduled = true;
    QMetaObject::invokeMethod(this, &QueueScheduler::reevaluate, Qt::QueuedConnection);
}

void QueueScheduler::reevaluate()
{
    _scheduled = false;
    handleIntent();
    if (_queue->isPaused())
        return; // a running step finishes; nothing new starts, not even a preparation
    if (const QueueEntry *prep = _queue->entryInSlot(Slot::Preparation))
        continuePreparation(*prep);
    else
        startPreparation();
    activateHead();
}

bool QueueScheduler::isCurrent(qint64 postId, const QString &runId) const
{
    const QueueEntry *entry = _queue->find(postId);
    return entry && entry->runId == runId;
}

// --- "post now" -------------------------------------------------------------------

bool QueueScheduler::handleIntent()
{
    const PostNowIntent intent = _queue->intent();
    if (intent.isNull() || intent.state == PostNowIntent::State::Switching)
        return false;
    const QueueEntry *active = _queue->activeEntry();
    if (!active || active->runId != intent.activeRunId || active->lifecycle != Lifecycle::Posting) {
        _queue->engineDropIntent(QStringLiteral("active_changed"));
        return false;
    }
    if (!_queue->intentTargetValid()) {
        _queue->engineDropIntent(QStringLiteral("target_invalid"));
        return false;
    }
    const QueueEntry *target = _queue->find(intent.targetPostId);
    if (!target->isReady()) {
        // The target needs the preparation slot: a speculative preparation
        // gives way, but only once the target is known to fit.
        const QueueEntry *prep = _queue->entryInSlot(Slot::Preparation);
        if (prep && prep->postId != target->postId && !_pendingStops.contains(prep->postId)
            && spaceAllows(*target))
            requestStop(*prep, PendingStop::Preempt, Disposition::Keep);
        return false;
    }
    if (_queue->isPaused())
        return false;
    if (_preflight) {
        const Admission admission = _preflight(*target, *active);
        if (admission == Admission::Wait)
            return false;
        if (admission == Admission::Blocked) {
            _queue->engineDropIntent(QStringLiteral("vpn_unavailable"));
            return false;
        }
    }
    const QueueEntry activeCopy = *active;
    if (!_queue->engineSetIntentSwitching())
        return false;
    requestStop(activeCopy, PendingStop::SwitchActive, Disposition::Keep);
    return true;
}

// --- preparation ---------------------------------------------------------------------

int QueueScheduler::preparedCount() const
{
    int n = 0;
    for (const QueueEntry &entry : _queue->snapshot(Domain::Session))
        if (entry.lifecycle == Lifecycle::Prepared)
            ++n;
    return n;
}

bool QueueScheduler::spaceAllows(const QueueEntry &entry) const
{
    if (!_budget.freeBytes)
        return true;
    const QString root = _budget.rootFor ? _budget.rootFor(entry) : QString();
    return _budget.freeBytes(root) >= estimatedPreparationBytes(entry) + _budget.marginBytes;
}

QList<PrepStep> QueueScheduler::stepsFor(const QueueEntry &entry) const
{
    QList<PrepStep> steps;
    if (entry.options.job.doCompress)
        steps << PrepStep::Compress;
    if (entry.options.job.doPar2)
        steps << PrepStep::Par2;
    steps << PrepStep::Collect; // NFO copy and explicit list of files to publish
    return steps;
}

ArtifactSetRef QueueScheduler::newArtifactSet(const QueueEntry &entry) const
{
    ArtifactSetRef set;
    set.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    set.generation = entry.lastGeneration + 1;
    set.recipeRevision = entry.recipeRevision;
    set.root = _budget.rootFor ? _budget.rootFor(entry) : QString();
    return set;
}

const QueueEntry *QueueScheduler::requiredPreparation() const
{
    const PostNowIntent &intent = _queue->intent();
    if (!intent.isNull() && _queue->intentTargetValid()) {
        const QueueEntry *target = _queue->find(intent.targetPostId);
        if (target->lifecycle == Lifecycle::Queued && target->needsPreparation())
            return target;
    }
    // With a transfer running, preparing what comes next is anticipation, under
    // budget. With the transfer slot idle, the head's preparation is required:
    // the head is never starved by the count of prepared entries.
    if (_queue->activeEntry())
        return nullptr;
    const QueueEntry *head = _queue->headEntry();
    if (head && head->lifecycle == Lifecycle::Queued && head->needsPreparation())
        return head;
    return nullptr;
}

const QueueEntry *QueueScheduler::speculativePreparation() const
{
    for (qint64 id : _queue->order(Domain::Session)) {
        const QueueEntry *entry = _queue->find(id);
        if (entry->isEligible() && entry->lifecycle == Lifecycle::Queued
            && entry->needsPreparation())
            return entry;
    }
    return nullptr;
}

void QueueScheduler::startPreparation()
{
    const QueueEntry *candidate = requiredPreparation();
    const bool required = candidate != nullptr;
    if (!candidate)
        candidate = speculativePreparation();
    if (!candidate || (!required && preparedCount() >= _budget.preparedMax))
        return;
    const qint64 postId = candidate->postId;
    if (!spaceAllows(*candidate)) {
        if (required) {
            const qint64 needed = estimatedPreparationBytes(*candidate) + _budget.marginBytes;
            _queue->engineBlock(postId,
                                { QStringLiteral("resources"),
                                  { { QStringLiteral("needed"), needed } } },
                                Lifecycle::Queued);
        }
        return;
    }
    const ArtifactSetRef set = newArtifactSet(*candidate);
    _remainingSteps.insert(postId, stepsFor(*candidate));
    _preparedBytes.insert(postId, 0);
    if (!_queue->engineBeginPreparation(postId, set)) {
        _remainingSteps.remove(postId);
        _preparedBytes.remove(postId);
        return;
    }
    continuePreparation(*_queue->find(postId));
}

void QueueScheduler::continuePreparation(const QueueEntry &entry)
{
    if (_stepRunning == entry.postId || _pendingStops.contains(entry.postId))
        return;
    const QList<PrepStep> steps = _remainingSteps.value(entry.postId);
    if (steps.isEmpty())
        return;
    _stepRunning = entry.postId;
    _executor->prepareStep(entry.postId, entry.runId, entry.artifacts.id, steps.first());
}

void QueueScheduler::onStepFinished(qint64 postId,
                                    const QString &runId,
                                    const QString &setId,
                                    PrepStep step,
                                    bool ok,
                                    const QString &error,
                                    qint64 bytes)
{
    const QueueEntry *entry = _queue->find(postId);
    if (!entry || entry->runId != runId || entry->artifacts.id != setId)
        return; // acknowledgement of a replaced run or artifact set
    if (_stepRunning == postId)
        _stepRunning = 0;
    if (_pendingStops.contains(postId))
        return; // its quiescence decides
    QList<PrepStep> &steps = _remainingSteps[postId];
    if (!steps.isEmpty() && steps.first() == step)
        steps.removeFirst();
    _preparedBytes[postId] += bytes;
    if (!ok) {
        _remainingSteps.remove(postId);
        _preparedBytes.remove(postId);
        _queue->engineBlock(postId,
                            { QStringLiteral("preparation_failed"),
                              { { QStringLiteral("step"), static_cast<int>(step) },
                                { QStringLiteral("error"), error } } },
                            Lifecycle::Queued);
    } else if (steps.isEmpty()) {
        ArtifactSetRef set = entry->artifacts;
        set.bytes = _preparedBytes.take(postId);
        _remainingSteps.remove(postId);
        _queue->engineRecordPrepared(postId, set);
    }
    scheduleReevaluate();
}

// --- transfer ---------------------------------------------------------------------------

void QueueScheduler::activateHead()
{
    if (_queue->activeEntry())
        return;
    const QueueEntry *head = _queue->headEntry();
    if (!head || !head->isReady())
        return; // strict order: never start a ready entry behind a head still preparing
    if (_admission) {
        const Admission admission = _admission(*head);
        if (admission == Admission::Wait)
            return;
        if (admission == Admission::Blocked) {
            _queue->engineBlock(head->postId,
                                { QStringLiteral("vpn_unavailable"), {} },
                                head->lifecycle);
            return;
        }
    }
    const qint64 postId = head->postId;
    const QString setId = head->artifacts.id;
    if (!_queue->engineActivate(postId))
        return;
    _executor->activate(postId, _queue->find(postId)->runId, setId);
}

void QueueScheduler::onTransferFinished(qint64 postId, const QString &runId, bool success)
{
    Q_UNUSED(success) // the result itself is recorded by F1, in the history
    if (!isCurrent(postId, runId))
        return;
    _queue->engineSetLifecycle(postId, Lifecycle::Finalizing, Slot::Transfer);
}

void QueueScheduler::onFinalized(qint64 postId, const QString &runId, bool ok, const QString &error)
{
    if (!isCurrent(postId, runId))
        return;
    if (ok)
        _queue->engineRetire(postId);
    else
        _queue->engineBlock(postId,
                            { QStringLiteral("finalization_failed"),
                              { { QStringLiteral("error"), error } } },
                            Lifecycle::Finalizing);
    scheduleReevaluate();
}

// --- stops --------------------------------------------------------------------------------

void QueueScheduler::requestStop(const QueueEntry &entry, PendingStop why, Disposition disposition)
{
    const qint64 postId = entry.postId;
    const QString runId = entry.runId;
    const Slot slot = entry.slot;
    _pendingStops.insert(postId, why);
    _queue->engineSetLifecycle(postId, Lifecycle::Quiescing, slot);
    _executor->interrupt(postId, runId, disposition);
}

void QueueScheduler::onQuiesced(qint64 postId, const QString &runId, bool ok, bool manifestEngaged)
{
    const QueueEntry *entry = _queue->find(postId);
    if (!entry || entry->runId != runId)
        return;
    const bool wasTransfer = entry->slot == Slot::Transfer;
    // A runtime that stopped on its own (technical failure) keeps its entry.
    const PendingStop why = _pendingStops.value(postId, PendingStop::Technical);
    _pendingStops.remove(postId);
    if (_stepRunning == postId)
        _stepRunning = 0;
    _remainingSteps.remove(postId);
    _preparedBytes.remove(postId);
    if (!ok) {
        // Stop not proven: keep everything, block, never clean up.
        if (why == PendingStop::SwitchActive)
            _queue->engineDropIntent(QStringLiteral("switch_failed"));
        _queue->engineBlock(postId, { QStringLiteral("quiesce_failed"), {} }, Lifecycle::Quiescing);
        scheduleReevaluate();
        return;
    }
    switch (why) {
    case PendingStop::SwitchActive: _queue->engineCommitSwitch(manifestEngaged); break;
    case PendingStop::Hold:
        _queue->engineInterrupt(postId, manifestEngaged, true, wasTransfer);
        break;
    case PendingStop::Preempt:
        _queue->engineInterrupt(postId, manifestEngaged, false, false);
        break;
    case PendingStop::Technical:
        _queue->engineInterrupt(postId, manifestEngaged, false, wasTransfer);
        break;
    case PendingStop::CancelPreparation: _queue->engineRetire(postId); break;
    }
    scheduleReevaluate();
}

void QueueScheduler::onRuntimeActionRequested(qint64 postId, RuntimeRequest request)
{
    const QueueEntry *entry = _queue->find(postId);
    if (!entry || _pendingStops.contains(postId))
        return;
    const QueueEntry copy = *entry;
    switch (request) {
    case RuntimeRequest::HoldInterrupt:
        if (copy.hasRuntime())
            requestStop(copy, PendingStop::Hold, Disposition::Keep);
        break;
    case RuntimeRequest::CancelPreparation:
        if (copy.slot == Slot::Preparation)
            requestStop(copy, PendingStop::CancelPreparation, Disposition::Keep);
        break;
    case RuntimeRequest::StopTransfer:
        if (copy.slot == Slot::Transfer) {
            _queue->engineSetLifecycle(postId, Lifecycle::Quiescing, Slot::Transfer);
            _executor->interrupt(postId, copy.runId, Disposition::Stop);
        }
        break;
    case RuntimeRequest::RetryFinalization:
        _queue->engineBlock(postId, {}, Lifecycle::Finalizing); // unblocked while it retries
        _executor->retryFinalization(postId, copy.runId);
        break;
    case RuntimeRequest::AbandonPlanning: _executor->abandonPlanning(postId, copy.runId); break;
    }
}

void QueueScheduler::onPausedChanged(bool paused)
{
    const QueueEntry *active = _queue->activeEntry();
    if (paused && active && active->lifecycle == Lifecycle::Posting) {
        _executor->pauseTransfer(active->postId);
        _pausedTransfer = active->postId;
    } else if (!paused && _pausedTransfer != 0) {
        if (active && active->postId == _pausedTransfer)
            _executor->resumeTransfer(_pausedTransfer);
        _pausedTransfer = 0;
    }
    scheduleReevaluate();
}

} // namespace queue
