// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// See PostingJobExecutor.h.
//
//========================================================================

#include "PostingJobExecutor.h"

#include "PostingJob.h"
#include "queue/PostQueue.h"

using namespace queue;

PostingJobExecutor::PostingJobExecutor(const PostQueue *queue,
                                       Factory factory,
                                       QString ownerId,
                                       QObject *parent)
    : QueueExecutor(parent)
    , _queue(queue)
    , _factory(std::move(factory))
    , _ownerId(std::move(ownerId))
{
    // A prepared entry removed before its transfer leaves nothing behind here.
    connect(_queue, &PostQueue::entryRemoved, this, [this](qint64 postId) {
        const QString prefix = QStringLiteral("%1|").arg(postId);
        for (auto it = _manifests.begin(); it != _manifests.end();)
            it = it.key().startsWith(prefix) ? _manifests.erase(it) : std::next(it);
    });
}

PostingJobExecutor::~PostingJobExecutor()
{
    // Torn down with work still running (an exit whose close could not be
    // written): a transfer stops the way a Keep does, producers joined and
    // confirmed articles flushed, before its job is deleted. No result is
    // recorded: the run is found unfinished at the next start. The job's own
    // destructor stops a tool that still runs.
    for (const Runtime &runtime : std::as_const(_runtimes)) {
        PostingJob *job = runtime.job;
        if (!job)
            continue;
        disconnect(job, nullptr, this, nullptr);
        if (runtime.phase != Phase::Preparing)
            job->finishTransfer();
        delete job;
    }
}

QString PostingJobExecutor::key(qint64 postId, const QString &runId, const QString &setId)
{
    return QStringLiteral("%1|%2|%3").arg(postId).arg(runId, setId);
}

PostingJob *PostingJobExecutor::runtime(qint64 postId) const
{
    return _runtimes.value(postId).job;
}

PreparedManifest PostingJobExecutor::manifest(qint64 postId,
                                              const QString &runId,
                                              const QString &setId) const
{
    return _manifests.value(key(postId, runId, setId));
}

void PostingJobExecutor::later(std::function<void()> emitter)
{
    QMetaObject::invokeMethod(this, std::move(emitter), Qt::QueuedConnection);
}

PostingJob *PostingJobExecutor::createJob(qint64 postId, const QString &runId, const QString &setId)
{
    const QueueEntry *entry = _queue->find(postId);
    if (!entry || !_factory)
        return nullptr;
    JobExecutionContext context;
    context.ownerId = _ownerId;
    context.postId = postId;
    context.runId = runId;
    context.origin = entry->origin;
    context.tabNumber = entry->tabNumber;
    context.setId = setId;
    if (!setId.isEmpty() && entry->artifacts.id == setId) {
        context.setRoot = entry->artifacts.root;
        context.setGeneration = entry->artifacts.generation;
        context.setRecipeRevision = entry->artifacts.recipeRevision;
    }
    PostingJob *job = _factory(*entry, context);
    if (job && !job->parent())
        job->setParent(this);
    return job;
}

bool PostingJobExecutor::isCurrent(qint64 postId,
                                   const PostingJob *job,
                                   const QString &runId,
                                   Phase phase) const
{
    const auto it = _runtimes.constFind(postId);
    return it != _runtimes.cend() && it->job == job && it->runId == runId && it->phase == phase;
}

void PostingJobExecutor::releaseRuntime(qint64 postId)
{
    const auto it = _runtimes.find(postId);
    if (it == _runtimes.end())
        return;
    if (PostingJob *job = it->job) {
        disconnect(job, nullptr, this, nullptr);
        job->deleteLater();
    }
    _runtimes.erase(it);
}

// --- preparation -----------------------------------------------------------------

void PostingJobExecutor::prepareStep(qint64 postId,
                                     const QString &runId,
                                     const QString &setId,
                                     PrepStep step)
{
    PostingJob *job = nullptr;
    const auto it = _runtimes.constFind(postId);
    if (it != _runtimes.cend()) {
        if (it->runId != runId || it->setId != setId || it->phase != Phase::Preparing) {
            // Another run still holds a runtime: never two jobs for one post.
            later([this, postId, runId, setId, step] {
                emit stepFinished(postId, runId, setId, step, false, QStringLiteral("busy"), 0);
            });
            return;
        }
        job = it->job;
    }
    if (!job) {
        job = createJob(postId, runId, setId);
        if (!job) {
            later([this, postId, runId, setId, step] {
                emit stepFinished(postId,
                                  runId,
                                  setId,
                                  step,
                                  false,
                                  QStringLiteral("no_runtime"),
                                  0);
            });
            return;
        }
        connect(job,
                &PostingJob::preparationStepFinished,
                this,
                [this, postId, job, runId, setId](PrepStep done,
                                                  bool ok,
                                                  const QString &error,
                                                  qint64 bytes) {
            onStepEnded(postId, job, runId, setId, done, ok, error, bytes);
        });
        Runtime runtime;
        runtime.job = job;
        runtime.runId = runId;
        runtime.setId = setId;
        _runtimes.insert(postId, runtime);
    }
    job->runPreparationStep(step); // a step that cannot start reports it too
}

void PostingJobExecutor::onStepEnded(qint64 postId,
                                     PostingJob *job,
                                     const QString &runId,
                                     const QString &setId,
                                     PrepStep step,
                                     bool ok,
                                     const QString &error,
                                     qint64 bytes)
{
    if (!isCurrent(postId, job, runId, Phase::Preparing) || _runtimes[postId].setId != setId)
        return; // a replaced runtime, or one being stopped
    if (ok && step == PrepStep::Collect)
        _manifests.insert(key(postId, runId, setId), job->preparedManifest());
    // Nothing else runs now. A job still holding renamed sources keeps the
    // only map back to their names: it stays until they are restored. One
    // whose tool ended unproven stays with its slot: the tool may still write.
    const bool over = !ok || step == PrepStep::Collect;
    if (over && !job->hasUnrestoredSources() && !job->holdsUnprovenTool())
        releaseRuntime(postId);
    emit stepFinished(postId, runId, setId, step, ok, error, bytes);
}

// --- transfer -------------------------------------------------------------------

void PostingJobExecutor::activate(qint64 postId, const QString &runId, const QString &setId)
{
    PostingJob *job = nullptr;
    const auto it = _runtimes.constFind(postId);
    if (it != _runtimes.cend()) {
        if (it->runId != runId || it->setId != setId || it->phase != Phase::Preparing) {
            later([this, postId, runId] {
                emit transferFinished(postId, runId, notSent(QStringLiteral("failed")));
            });
            return;
        }
        job = it->job; // kept by its preparation
    } else {
        job = createJob(postId, runId, setId);
        if (!job) {
            later([this, postId, runId] {
                emit transferFinished(postId, runId, notSent(QStringLiteral("failed")));
            });
            return;
        }
        Runtime runtime;
        runtime.job = job;
        runtime.runId = runId;
        runtime.setId = setId;
        _runtimes.insert(postId, runtime);
    }
    _runtimes[postId].phase = Phase::Transferring;
    connect(job, &PostingJob::postingStarted, this, [this, postId, job, runId] {
        if (isCurrent(postId, job, runId, Phase::Transferring))
            emit transferStarted(postId, runId);
    }, Qt::QueuedConnection);
    // PostingJob may end through both signals: the run ends once.
    for (auto ended : { &PostingJob::postingFinished, &PostingJob::noMoreConnection })
        connect(job, ended, this, [this, postId, job, runId] {
            onTransferEnded(postId, job, runId);
        }, Qt::QueuedConnection);
    emit runtimeActivated(postId, job);
    // Bound now, started on the next turn of the event loop, as the historical
    // queue did: a stop asked in between ends it before any byte is sent. A
    // Keep in between took the runtime away: then it never starts at all.
    QMetaObject::invokeMethod(this,
                              [this,
                               postId,
                               runId,
                               job = QPointer<PostingJob>(job),
                               manifest = _manifests.take(key(postId, runId, setId))] {
        if (job && isCurrent(postId, job, runId, Phase::Transferring))
            job->activateTransfer(manifest);
    },
                              Qt::QueuedConnection);
}

AttemptResult PostingJobExecutor::notSent(const QString &result)
{
    AttemptResult attempt;
    attempt.result = result;
    attempt.endCause = result == QLatin1String("cancelled") ? QStringLiteral("user_stop")
                                                            : QStringLiteral("technical");
    attempt.transferred = false;
    return attempt;
}

void PostingJobExecutor::onTransferEnded(qint64 postId, PostingJob *job, const QString &runId)
{
    if (!isCurrent(postId, job, runId, Phase::Transferring))
        return;
    Runtime &runtime = _runtimes[postId];
    if (runtime.ended)
        return;
    runtime.ended = true;
    // The runtime stays for F2 and for what follows F3: only the transfer ends.
    runtime.phase = Phase::Finalizing;
    const PostingJob::TransferOutcome outcome = job->finishTransfer();
    AttemptResult attempt;
    attempt.result = outcome.result;
    attempt.endCause = outcome.endCause;
    attempt.avgSpeed = outcome.avgSpeed;
    attempt.activeMs = outcome.activeMs;
    attempt.transferred = outcome.transferred;
    emit transferEnded(postId, job);
    emit transferFinished(postId, runId, attempt);
}

void PostingJobExecutor::finalize(qint64 postId, const QString &runId)
{
    const auto it = _runtimes.constFind(postId);
    if (it == _runtimes.cend() || it->runId != runId || it->phase != Phase::Finalizing
        || !it->job) {
        // No runtime ever transferred for this run: nothing to publish.
        later([this, postId, runId] { emit finalized(postId, runId, true, {}); });
        return;
    }
    QString error;
    const bool published = it->job->publishDeliverable(&error);
    later([this, postId, runId, published, error] {
        emit finalized(postId, runId, published, error);
    });
}

void PostingJobExecutor::completeLocalWork(qint64 postId, const QString &runId)
{
    const auto it = _runtimes.constFind(postId);
    if (it == _runtimes.cend() || it->runId != runId || !it->job) {
        emit localWorkQueued(postId, runId, false);
        return;
    }
    PostingJob *job = it->job;
    // Only archives whose tool is proven stopped may go (an "exited" one is not).
    job->allowArtifactCleanup(job->hasPostFinishedSuccessfully() && job->preparedSetStopProven());
    // F2 published something only if articles were sent: no sheet, script or
    // upload for a post that produced no NZB.
    job->runPostFinalizationWork(job->hasTransferStarted());
    const bool queued = job->postActionsQueued();
    emit runtimeEnded(postId, job);
    // Its sources not settled, the runtime stays: it holds the way back, and
    // the journal its moves (lot C5.7).
    if (job->settleSources())
        releaseRuntime(postId);
    emit localWorkQueued(postId, runId, queued);
}

void PostingJobExecutor::pauseTransfer(qint64 postId)
{
    const Runtime runtime = _runtimes.value(postId);
    if (runtime.job && runtime.phase == Phase::Transferring)
        runtime.job->pause();
}

void PostingJobExecutor::resumeTransfer(qint64 postId)
{
    const Runtime runtime = _runtimes.value(postId);
    if (runtime.job && runtime.phase == Phase::Transferring)
        runtime.job->resume();
}

// --- stops ----------------------------------------------------------------------

void PostingJobExecutor::interrupt(qint64 postId, const QString &runId, Disposition disposition)
{
    const auto it = _runtimes.find(postId);
    if (it == _runtimes.end() || it->runId != runId) {
        // Nothing runs for this run: the stop holds at once.
        if (disposition == Disposition::Stop)
            later([this, postId, runId] {
                emit transferFinished(postId, runId, notSent(QStringLiteral("cancelled")));
            });
        else
            later([this, postId, runId] { emit quiesced(postId, runId, true, false); });
        return;
    }
    if (it->phase == Phase::Transferring) {
        if (disposition == Disposition::Stop && it->job) {
            emit it->job->stopPosting(); // terminal: ends through onTransferEnded()
            return;
        }
        keepTransfer(postId, runId);
        return;
    }
    quiescePreparation(postId, runId);
}

//! A preparation stops asynchronously: its tool's supervisor answers. Asked
//! again while it stops, the same answer comes; a later ask is answered again.
void PostingJobExecutor::quiescePreparation(qint64 postId, const QString &runId)
{
    Runtime &runtime = _runtimes[postId];
    PostingJob *job = runtime.job;
    if (!job) {
        later([this, postId, runId] { emit quiesced(postId, runId, false, false); });
        return;
    }
    if (runtime.phase == Phase::Quiescing && runtime.quiesceAsked)
        return; // under way: its answer covers this ask too
    runtime.phase = Phase::Quiescing;
    runtime.quiesceAsked = true;
    const QString setId = runtime.setId;
    // Connected before the ask: the answer may come at once.
    connect(job,
            &PostingJob::preparationQuiesced,
            this,
            [this, postId, job, runId, setId](bool proven) {
        if (!isCurrent(postId, job, runId, Phase::Quiescing))
            return; // a replaced runtime
        _runtimes[postId].quiesceAsked = false;
        if (proven) {
            _manifests.remove(key(postId, runId, setId));
            releaseRuntime(postId);
        } // otherwise the runtime and its slot stay: the stop is not proven
        emit quiesced(postId, runId, proven, false);
    },
            Qt::SingleShotConnection);
    job->quiescePreparation();
}

//! An interruption kept (Hold, "post now", close and keep): producers stopped
//! and joined, the history flushed, no result recorded. Its successor resumes
//! the transfer manifest; in-flight articles were classified by their closing.
void PostingJobExecutor::keepTransfer(qint64 postId, const QString &runId)
{
    Runtime &runtime = _runtimes[postId];
    PostingJob *job = runtime.job;
    if (!job) {
        later([this, postId, runId] { emit quiesced(postId, runId, false, false); });
        return;
    }
    runtime.phase = Phase::Quiescing;
    const PostingJob::TransferOutcome outcome = job->finishTransfer();
    const bool manifest = job->transferManifestEngaged();
    emit runtimeKept(postId, job);
    // A source whose move is not settled keeps its runtime, and the stop is
    // not proven: no new run while the journal says it is aside (lot C5.7).
    const bool settled = job->settleSources();
    if (settled)
        releaseRuntime(postId); // its archives stay: no cleanup was allowed
    const qint64 activeMs = outcome.activeMs;
    later([this, postId, runId, settled, manifest, activeMs] {
        emit quiesced(postId, runId, settled, manifest, activeMs);
    });
}

void PostingJobExecutor::retryFinalization(qint64 postId, const QString &runId)
{
    const auto it = _runtimes.constFind(postId);
    if (it == _runtimes.cend() || it->runId != runId || it->phase != Phase::Finalizing) {
        // Repairing it without its runtime is the restoration's (lot C6).
        later([this, postId, runId] {
            emit finalized(postId, runId, false, QStringLiteral("no_runtime"));
        });
        return;
    }
    finalize(postId, runId);
}

void PostingJobExecutor::abandonPlanning(qint64 postId, const QString &runId)
{
    const auto it = _runtimes.constFind(postId);
    if (it != _runtimes.cend() && it->runId == runId && it->job) {
        // No deliverable, no post action, archives kept for a resume. Never
        // reached with a source aside (Collect refuses), checked all the same.
        emit runtimeEnded(postId, it->job);
        if (it->job->settleSources())
            releaseRuntime(postId);
    }
    emit localWorkQueued(postId, runId, false);
}
