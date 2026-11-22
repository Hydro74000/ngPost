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
    // The job's own destructor stops a tool that still runs.
    for (const Runtime &runtime : std::as_const(_runtimes))
        delete runtime.job.data();
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
    // only map back to their names: it stays until they are restored.
    const bool over = !ok || step == PrepStep::Collect;
    if (over && !job->hasUnrestoredSources())
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
                emit transferFinished(postId, runId, false);
                emit finalized(postId, runId, false, QStringLiteral("busy"));
            });
            return;
        }
        job = it->job; // kept by its preparation
    } else {
        job = createJob(postId, runId, setId);
        if (!job) {
            later([this, postId, runId] {
                emit transferFinished(postId, runId, false);
                emit finalized(postId, runId, false, QStringLiteral("no_runtime"));
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
    // queue did: a stop asked in between ends it before any byte is sent.
    QMetaObject::invokeMethod(job, [job, manifest = _manifests.take(key(postId, runId, setId))] {
        job->activateTransfer(manifest);
    }, Qt::QueuedConnection);
}

void PostingJobExecutor::onTransferEnded(qint64 postId, PostingJob *job, const QString &runId)
{
    if (!isCurrent(postId, job, runId, Phase::Transferring))
        return;
    Runtime &runtime = _runtimes[postId];
    if (runtime.ended)
        return;
    runtime.ended = true;
    const bool success = job->hasPostFinishedSuccessfully();
    const bool localEndOk = !job->historyDataUnreliable();
    job->allowArtifactCleanup(success); // C3: as the historical job decided it
    emit runtimeEnded(postId, job);
    releaseRuntime(postId);
    emit transferFinished(postId, runId, success);
    emit finalized(postId,
                   runId,
                   localEndOk,
                   localEndOk ? QString() : QStringLiteral("history_unreliable"));
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
                emit transferFinished(postId, runId, false);
                emit finalized(postId, runId, true, {});
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
        // Keeping or abandoning a transfer needs the barriers of lot C4.
        later([this, postId, runId] { emit quiesced(postId, runId, false, false); });
        return;
    }
    it->phase = Phase::Quiescing;
    const QString setId = it->setId;
    const bool stopped = it->job && it->job->quiescePreparation();
    if (stopped) {
        _manifests.remove(key(postId, runId, setId));
        releaseRuntime(postId);
    } // otherwise the runtime and its slot stay: the stop is not proven
    later([this, postId, runId, stopped] { emit quiesced(postId, runId, stopped, false); });
}

void PostingJobExecutor::retryFinalization(qint64 postId, const QString &runId)
{
    later([this, postId, runId] {
        emit finalized(postId, runId, false, QStringLiteral("unsupported"));
    });
}

void PostingJobExecutor::abandonPlanning(qint64 postId, const QString &runId)
{
    later([this, postId, runId] {
        emit finalized(postId, runId, false, QStringLiteral("unsupported"));
    });
}
