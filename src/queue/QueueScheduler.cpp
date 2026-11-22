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
    connect(_executor, &QueueExecutor::transferStarted, this, [this](qint64, const QString &runId) {
        _startedRuns.insert(runId);
        scheduleReevaluate(); // what comes next may now be prepared
    });
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
    if (_technicalPause)
        return; // only the last durable state is known: nothing starts
    handleIntent();
    if (_queue->isPaused() || _closing)
        return; // a running step finishes; nothing new starts, not even a preparation
    // The head first: anticipation never delays a transfer that can start.
    activateHead();
    // A write just failed, or the admission's handlers closed or paused the
    // queue: not one more decision.
    if (_technicalPause || _closing || _queue->isPaused())
        return;
    if (const QueueEntry *prep = _queue->entryInSlot(Slot::Preparation))
        continuePreparation(*prep);
    else
        startPreparation();
    if (!_technicalPause)
        activateHead(); // a preparation that could not start may leave the head ready
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
    if (intent.isNull())
        return false;
    const QueueEntry *active = _queue->activeEntry();
    const bool activeMatches = active && active->runId == intent.activeRunId
        && active->lifecycle == Lifecycle::Posting;
    if (intent.state == PostNowIntent::State::Switching) {
        // A switch whose stop could not be written is asked again.
        if (activeMatches && !_pendingStops.contains(active->postId))
            requestStop(QueueEntry(*active), PendingStop::SwitchActive, Disposition::Keep);
        return false;
    }
    if (!activeMatches) {
        if (!_queue->engineDropIntent(QStringLiteral("active_changed")))
            enterTechnicalPause(QStringLiteral("store_failed"));
        return false;
    }
    if (!_queue->intentTargetValid()) {
        if (!_queue->engineDropIntent(QStringLiteral("target_invalid")))
            enterTechnicalPause(QStringLiteral("store_failed"));
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
    if (_queue->isPaused() || _closing)
        return false;
    if (_preflight) {
        const qint64 activeId = active->postId;
        const Admission admission = _preflight(*target, *active);
        if (admission == Admission::Wait)
            return false;
        if (admission == Admission::Blocked) {
            if (!_queue->engineDropIntent(QStringLiteral("vpn_unavailable")))
                enterTechnicalPause(QStringLiteral("store_failed"));
            return false;
        }
        // Its handlers may change the queue: everything is checked again.
        active = _queue->activeEntry();
        if (_closing || _technicalPause || !active || active->postId != activeId
            || active->lifecycle != Lifecycle::Posting || _queue->intent().isNull())
            return false;
    }
    const QueueEntry activeCopy = *active;
    if (!_queue->engineSetIntentSwitching()) {
        enterTechnicalPause(QStringLiteral("store_failed"));
        return false;
    }
    return requestStop(activeCopy, PendingStop::SwitchActive, Disposition::Keep);
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
    // Anticipation waits for the current transfer to have really started.
    const QueueEntry *active = _queue->activeEntry();
    if (active && !_startedRuns.contains(active->runId))
        return nullptr;
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
            if (!_queue->engineBlock(postId,
                                     { QStringLiteral("resources"),
                                       { { QStringLiteral("needed"), needed } } },
                                     Lifecycle::Queued))
                enterTechnicalPause(QStringLiteral("store_failed"));
        }
        return;
    }
    const ArtifactSetRef set = newArtifactSet(*candidate);
    const QList<PrepStep> steps = stepsFor(*candidate);
    if (!_queue->engineBeginPreparation(postId, set)) {
        enterTechnicalPause(QStringLiteral("store_failed"));
        return;
    }
    _remainingSteps.insert(postId, steps);
    _preparedBytes.insert(postId, 0);
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
    applyInOrder([this, postId, runId, setId, step, ok, error, bytes] {
        const QueueEntry *current = _queue->find(postId);
        if (!current || current->runId != runId || current->artifacts.id != setId)
            return true; // replaced meanwhile: nothing left to record
        return recordStep(postId, step, ok, error, bytes);
    });
    scheduleReevaluate();
}

//! Local progress changes only once the durable transition went through.
bool QueueScheduler::recordStep(qint64 postId,
                                PrepStep step,
                                bool ok,
                                const QString &error,
                                qint64 bytes)
{
    QList<PrepStep> steps = _remainingSteps.value(postId);
    if (!steps.isEmpty() && steps.first() == step)
        steps.removeFirst();
    const qint64 total = _preparedBytes.value(postId) + bytes;
    if (ok && !steps.isEmpty()) {
        _remainingSteps.insert(postId, steps);
        _preparedBytes.insert(postId, total);
        return true;
    }
    bool written = false;
    if (!ok && _preparationFailure == PreparationFailure::Retire
        && !_executor->holdsUnsettledWork(postId)) {
        written = _queue->engineFailPreparation(postId, step, error);
    } else if (!ok) {
        written = _queue->engineBlock(postId,
                                      { QStringLiteral("preparation_failed"),
                                        { { QStringLiteral("step"), static_cast<int>(step) },
                                          { QStringLiteral("error"), error } } },
                                      Lifecycle::Queued);
    } else {
        ArtifactSetRef set = _queue->find(postId)->artifacts;
        set.bytes = total;
        written = _queue->engineRecordPrepared(postId, set);
    }
    if (!written)
        return false;
    _remainingSteps.remove(postId);
    _preparedBytes.remove(postId);
    return true;
}

// --- transfer ---------------------------------------------------------------------------

void QueueScheduler::activateHead()
{
    if (_queue->activeEntry())
        return;
    const QueueEntry *head = _queue->headEntry();
    if (!head || !head->isReady())
        return; // strict order: never start a ready entry behind a head still preparing
    const qint64 postId = head->postId;
    if (_admission) {
        const Admission admission = _admission(*head);
        // Its handlers may change the queue (a refused VPN closes it): look again.
        head = _queue->find(postId);
        if (!head || admission == Admission::Wait)
            return;
        if (admission == Admission::Blocked) {
            if (!_queue->engineBlock(postId,
                                     { QStringLiteral("vpn_unavailable"), {} },
                                     head->lifecycle))
                enterTechnicalPause(QStringLiteral("store_failed"));
            return;
        }
        if (_closing || _technicalPause || _queue->isPaused() || _queue->activeEntry()
            || !head->isReady())
            return;
    }
    const QString setId = head->artifacts.id;
    if (!_queue->engineActivate(postId)) {
        enterTechnicalPause(QStringLiteral("store_failed"));
        return; // no transfer starts without its durable activation
    }
    _executor->activate(postId, _queue->find(postId)->runId, setId);
}

void QueueScheduler::onTransferFinished(qint64 postId,
                                        const QString &runId,
                                        const AttemptResult &result)
{
    if (!isCurrent(postId, runId))
        return;
    // F1 and the move to Finalizing together; F2 is asked only once F1 holds.
    applyInOrder([this, postId, runId, result] {
        if (!isCurrent(postId, runId))
            return true;
        AttemptResult attempt = result;
        attempt.postId = postId;
        attempt.runId = runId;
        if (!_queue->engineRecordResult(attempt))
            return false;
        _executor->finalize(postId, runId);
        return true;
    });
}

void QueueScheduler::onFinalized(qint64 postId, const QString &runId, bool ok, const QString &error)
{
    if (!isCurrent(postId, runId))
        return;
    applyInOrder([this, postId, runId, ok, error] {
        if (!isCurrent(postId, runId))
            return true;
        if (ok) {
            // F3: only now may its post commands, upload and cleanup run.
            if (!_queue->engineFinishFinalization(postId, runId, FinalizationStep::Kind::Complete))
                return false;
            _executor->completeLocalWork(postId, runId);
            return true;
        }
        return _queue->engineBlock(postId,
                                   { QStringLiteral("finalization_failed"),
                                     { { QStringLiteral("error"), error } } },
                                   Lifecycle::Finalizing);
    });
    scheduleReevaluate();
}

// --- stops --------------------------------------------------------------------------------

bool QueueScheduler::requestStop(const QueueEntry &entry, PendingStop why, Disposition disposition)
{
    const qint64 postId = entry.postId;
    const QString runId = entry.runId;
    if (!_queue->engineSetLifecycle(postId, Lifecycle::Quiescing, entry.slot)) {
        enterTechnicalPause(QStringLiteral("store_failed"));
        return false; // the runtime is only asked once Quiescing is durable
    }
    _pendingStops.insert(postId, why);
    _executor->interrupt(postId, runId, disposition);
    return true;
}

void QueueScheduler::onQuiesced(qint64 postId,
                                const QString &runId,
                                bool ok,
                                bool manifestEngaged,
                                qint64 activeMs)
{
    const QueueEntry *entry = _queue->find(postId);
    if (!entry || entry->runId != runId)
        return;
    const bool wasTransfer = entry->slot == Slot::Transfer;
    // A runtime that stopped on its own (technical failure) keeps its entry.
    const PendingStop why = _pendingStops.value(postId, PendingStop::Technical);
    if (_stepRunning == postId)
        _stepRunning = 0;
    if (!ok) {
        // Stop not proven: the runtime may still write. It keeps its slot,
        // nothing is cleaned up, and the whole queue waits.
        _pendingStops.insert(postId, why);
        _stuck.insert(postId);
        applyInOrder([this, postId, runId] {
            return !isCurrent(postId, runId)
                || _queue->engineMarkStuck(postId, { QStringLiteral("quiesce_failed"), {} });
        });
        enterTechnicalPause(QStringLiteral("quiesce_failed"));
        return;
    }
    applyInOrder([this, postId, runId, why, manifestEngaged, wasTransfer, activeMs] {
        return !isCurrent(postId, runId)
            || recordQuiesced(postId, runId, why, manifestEngaged, wasTransfer, activeMs);
    });
    if (_stuck.remove(postId) && _stuck.isEmpty())
        retryAfterTechnicalPause(); // the stop is proven after all
    scheduleReevaluate();
}

bool QueueScheduler::recordQuiesced(qint64 postId,
                                    const QString &runId,
                                    PendingStop why,
                                    bool manifest,
                                    bool wasTransfer,
                                    qint64 activeMs)
{
    bool written = false;
    // A kept run ends interrupted with its transfer time; its successor resumes
    // the transfer manifest when one was engaged (plan_queue.md §6.2).
    switch (why) {
    case PendingStop::SwitchActive: written = _queue->engineCommitSwitch(manifest); break;
    case PendingStop::Hold:
        written = _queue
                      ->engineKeepForRestart(postId, runId, activeMs, manifest, true, wasTransfer);
        break;
    case PendingStop::Preempt:
        written = _queue->engineKeepForRestart(postId, runId, activeMs, manifest, false, false);
        break;
    case PendingStop::Technical:
        written = _queue
                      ->engineKeepForRestart(postId, runId, activeMs, manifest, false, wasTransfer);
        break;
    case PendingStop::CancelPreparation: written = _queue->engineRetire(postId); break;
    }
    if (!written)
        return false;
    _pendingStops.remove(postId);
    _remainingSteps.remove(postId);
    _preparedBytes.remove(postId);
    return true;
}

void QueueScheduler::keepRunningForClose(bool letTransferFinish)
{
    _closing = true;
    // Writes still wait (an F1, maybe): an entry may look Posting while its
    // runtime already finalizes. No stop is asked on a state not known.
    if (_technicalPause)
        return;
    // A "post now" not yet switching is dropped; a switch under way ends as a
    // switch, and nothing starts after it while closing.
    const PostNowIntent intent = _queue->intent();
    if (!intent.isNull() && intent.state == PostNowIntent::State::Pending
        && !_queue->engineDropIntent(QStringLiteral("closing"))) {
        enterTechnicalPause(QStringLiteral("store_failed"));
        return;
    }
    for (const QueueEntry &entry : _queue->snapshot(Domain::Session)) {
        if (_pendingStops.contains(entry.postId))
            continue;
        // A switch whose stop could not be written is asked again as a switch.
        if (entry.lifecycle == Lifecycle::Posting && intent.state == PostNowIntent::State::Switching
            && entry.runId == intent.activeRunId) {
            if (!requestStop(entry, PendingStop::SwitchActive, Disposition::Keep))
                return;
            continue;
        }
        // Kept as after a technical stop: not held, a transfer keeps its archives.
        const bool stops = entry.lifecycle == Lifecycle::Preparing
            || (entry.lifecycle == Lifecycle::Posting && !letTransferFinish);
        if (stops && !requestStop(entry, PendingStop::Technical, Disposition::Keep))
            return; // the technical pause tells the close it failed
    }
}

void QueueScheduler::cancelClose()
{
    _closing = false;
    scheduleReevaluate();
}

void QueueScheduler::enterTechnicalPause(const QString &reason)
{
    if (_technicalPause)
        return;
    _technicalPause = true;
    _technicalReason = reason;
    emit technicalPauseChanged(true, reason);
}

void QueueScheduler::applyInOrder(std::function<bool()> write)
{
    if (_waitingWrites.isEmpty() && write())
        return;
    _waitingWrites << std::move(write);
    enterTechnicalPause(QStringLiteral("store_failed"));
}

void QueueScheduler::retryAfterTechnicalPause()
{
    while (!_waitingWrites.isEmpty()) {
        if (!_waitingWrites.first()())
            return; // still failing: the pause stays
        _waitingWrites.removeFirst();
    }
    if (!_technicalPause || !_stuck.isEmpty())
        return; // an unproven stop keeps the queue waiting
    _technicalPause = false;
    _technicalReason.clear();
    emit technicalPauseChanged(false, {});
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
        if (copy.slot != Slot::Transfer)
            break;
        if (!_queue->engineSetLifecycle(postId, Lifecycle::Quiescing, Slot::Transfer)) {
            enterTechnicalPause(QStringLiteral("store_failed"));
            break;
        }
        _executor->interrupt(postId, copy.runId, Disposition::Stop);
        break;
    case RuntimeRequest::RetryFinalization:
        // unblocked while it retries
        if (!_queue->engineBlock(postId, {}, Lifecycle::Finalizing)) {
            enterTechnicalPause(QStringLiteral("store_failed"));
            break;
        }
        _executor->retryFinalization(postId, copy.runId);
        break;
    case RuntimeRequest::AbandonPlanning:
        // Facts kept, no deliverable, nothing after: the runtime just goes.
        if (!_queue->engineFinishFinalization(postId,
                                              copy.runId,
                                              FinalizationStep::Kind::Abandon)) {
            enterTechnicalPause(QStringLiteral("store_failed"));
            break;
        }
        _executor->abandonPlanning(postId, copy.runId);
        break;
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
