// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// NgPost and the queue engine (plan_queue.md lot C3), used only while
// queueEngineEnabled(). The chain is built once, in this order, and torn down
// the other way round:
//
//   owner lock -> history service -> queue store -> PostQueue
//              -> PostingJobExecutor -> QueueScheduler
//
// Every post enters through admitPost() or admitResume(): the admission is
// durable before any runtime exists, and the scheduler alone starts work.
// _activeJob stays a view on the transferring runtime, never its owner.
//
//========================================================================

#include "NgPost.h"

#include "PostCmdRunner.h"
#include "PostingJob.h"
#include "PostingJobExecutor.h"
#include "PostingJobOptionsCodec.h"
#include "history/HistoryQueueStore.h"
#include "history/PostHistoryService.h"
#include "history/ResumePlanner.h"
#include "queue/OwnerLock.h"
#include "queue/PostQueue.h"
#include "queue/QueueScheduler.h"
#include "tools/ExternalToolResolver.h"
#include "vpn/VpnManager.h"
#ifdef __USE_HMI__
#include "hmi/AutoPostWidget.h"
#include "hmi/MainWindow.h"
#include "hmi/PostingWidget.h"
#endif

#include <QDateTime>
#include <QDir>
#include <QScopedValueRollback>
#include <QUuid>

using namespace queue;

namespace
{

QList<PersistedSource> capturedSources(const QFileInfoList &files)
{
    QList<PersistedSource> sources;
    for (const QFileInfo &file : files) {
        PersistedSource source;
        source.path = file.absoluteFilePath();
        source.isDir = file.isDir();
        source.sizeBytes = file.isDir() ? NgPost::recursiveSize(file) : file.size();
        source.mtimeEpoch = file.lastModified().toSecsSinceEpoch();
        sources << source;
    }
    return sources;
}

} // namespace

bool NgPost::_ensureQueueEngine()
{
    if (_postQueue)
        return true;
#ifdef NGPOST_TESTING
    if (_queueEngineUnavailableForTest)
        return false;
#endif
    if (!_ensureHistoryStore())
        return false;
    auto lock = std::make_unique<OwnerLock>(_postDbFile);
    QString error;
    if (!lock->acquire(&error)) {
        _error(tr("History database error: %1").arg(error), ERROR_CODE::ERR_HISTORY);
        return false;
    }
    _historyService->setOwnerId(lock->ownerId());
    _queueStore = new ServiceQueueStore(_historyService);
    // A new session queue at every start: restoring an earlier one is lot C6.
    _postQueue = new PostQueue(_queueStore,
                               QUuid::createUuid().toString(QUuid::WithoutBraces),
                               this);
    _postQueue->load();
    _queueExecutor = new PostingJobExecutor(_postQueue,
                                            [this](const QueueEntry &entry,
                                                   const JobExecutionContext &context) {
        return _createQueueRuntime(entry, context);
    },
                                            lock->ownerId(),
                                            this);
    connect(_queueExecutor,
            &PostingJobExecutor::runtimeActivated,
            this,
            &NgPost::_onQueueRuntimeActivated);
    connect(_queueExecutor,
            &PostingJobExecutor::transferEnded,
            this,
            &NgPost::_onQueueTransferEnded);
    connect(_queueExecutor, &PostingJobExecutor::runtimeEnded, this, &NgPost::_onQueueRuntimeEnded);
    connect(_queueExecutor, &PostingJobExecutor::runtimeKept, this, &NgPost::_onQueueRuntimeKept);
    connect(_queueExecutor,
            &PostingJobExecutor::localWorkQueued,
            this,
            [this](qint64 postId, const QString &, bool postActionsQueued) {
        if (postActionsQueued)
            _ownersAwaitingTasks.insert(postId);
        else
            _releaseQueuedPost(postId);
    });
    // Queued: a task may report its end while it is being handed over, before
    // its post is known to wait for it.
    if (_postCmdRunner)
        connect(_postCmdRunner,
                &PostCmdRunner::taskFinished,
                this,
                [this](qint64 postId, const QString &, quint64, bool) {
            if (_ownersAwaitingTasks.remove(postId))
                _releaseQueuedPost(postId);
        },
                Qt::QueuedConnection);

    QueueScheduler::Budget budget;
    budget.preparedMax = _preparePacking ? 1 : 0; // lot C3: PREPARE_PACKING only
    // Where a preparation writes. The free space budget itself is lot C6: until
    // then a post that does not fit fails as it always did.
    budget.rootFor = [](const QueueEntry &entry) {
        const QString tmp = entry.options.job.tmpPath;
        return QDir(tmp.isEmpty() ? QDir::tempPath() : tmp).absolutePath();
    };
    _queueScheduler = new QueueScheduler(_postQueue, _queueExecutor, budget, this);
    // Until the queue view can show a blocked entry (C7a), a failed preparation
    // ends its post as failed, as it always did.
    _queueScheduler->setPreparationFailurePolicy(QueueScheduler::PreparationFailure::Retire);
    _queueScheduler->setAdmission([this](const QueueEntry &) {
        if (!_vpnManager)
            return QueueScheduler::Admission::Proceed;
        switch (_vpnManager->admitJob(_nntpServers, _vpnManager->jobNeedsVpn(_nntpServers))) {
        case VpnManager::Admission::Proceed: return QueueScheduler::Admission::Proceed;
        case VpnManager::Admission::Wait: return QueueScheduler::Admission::Wait;
        case VpnManager::Admission::Blocked: break;
        }
        return QueueScheduler::Admission::Blocked;
    });
    connect(_queueScheduler,
            &QueueScheduler::technicalPauseChanged,
            this,
            [this](bool paused, const QString &reason) {
        if (paused) {
            _error(tr("History database error: %1").arg(reason), ERROR_CODE::ERR_HISTORY);
            // A stop that could not be written: Cancel and Resume must stay
            // available to ask again.
            _cancelingAll = false;
        }
        emit postingStateChanged();
        maybeFinishApplication();
    });
    // The end of the last entry is what lets the application finish.
    connect(_postQueue, &PostQueue::entryRemoved, this, [this](qint64 postId) {
#ifdef __USE_HMI__
        // Still followed by a tab: it left the queue without transferring.
        if (PostingWidget *view = _queueViews.take(postId))
            view->onQueuedPostRemoved();
        // An Auto post that ended before any transfer closes its line too.
        const QString autoRow = _autoRows.take(postId);
        if (!autoRow.isEmpty() && _hmi)
            _hmi->autoWidget()->updateFinishedJob(autoRow, 0, 0, 0);
#else
        Q_UNUSED(postId)
#endif
        if (!hasPostingJobs()) {
            _queuePaused = false;
            _cancelingAll = false;
            if (_postQueue->isPaused())
                _postQueue->setPaused(false); // paused by a global cancel
        }
        emit postingStateChanged();
        QMetaObject::invokeMethod(this, &NgPost::maybeFinishApplication, Qt::QueuedConnection);
    });
    connect(_postQueue, &PostQueue::entryChanged, this, [this](qint64 postId) {
        const QueueEntry *entry = _postQueue->find(postId);
        if (entry && entry->blocked.isBlocked())
            QMetaObject::invokeMethod(this, &NgPost::maybeFinishApplication, Qt::QueuedConnection);
    });
    _ownerLock = lock.release();
    return true;
}

void NgPost::_stopQueueRuntimes()
{
    // Decisions first, then the runtimes, whose jobs still write history.
    delete _queueScheduler;
    _queueScheduler = nullptr;
    delete _queueExecutor;
    _queueExecutor = nullptr;
    _activeJob = nullptr; // it was the executor's
    delete _postQueue;
    _postQueue = nullptr;
    delete _queueStore;
    _queueStore = nullptr;
}

void NgPost::_releaseQueueOwner()
{
    // Once the history is flushed and closed: then only may another session
    // claim what this one held.
    delete _ownerLock;
    _ownerLock = nullptr;
}

PersistedQueueOptions NgPost::_queueRecipe(const PostingJobOptions &options) const
{
    PersistedQueueOptions recipe;
    recipe.job = options;
    if (options.rarPass.isEmpty())
        recipe.passwordOrigin = options.declaredPassword.isEmpty() ? PasswordOrigin::Absent
                                                                   : PasswordOrigin::Declared;
    else if (options.rarPass == _rarPassFixed)
        recipe.passwordOrigin = PasswordOrigin::Fixed;
    else if (options.passwordGenerated)
        recipe.passwordOrigin = PasswordOrigin::Generated;
    else
        recipe.passwordOrigin = PasswordOrigin::Provided;
    recipe.passwordLength = static_cast<int>(options.rarPass.size());
    if (options.doCompress)
        recipe.archiverFamily = (options.rarTool == QLatin1String("7zip")
                                 || (options.rarTool.isEmpty()
                                     && externaltool::archiverForFile(options.rarPath)
                                         == QLatin1String("7zip")))
            ? QStringLiteral("7zip")
            : QStringLiteral("rar");
    if (options.doPar2) {
        const par2::Tool tool = options.par2Tool != par2::Tool::Auto ? options.par2Tool
            : useParPar()                                            ? par2::Tool::ParPar
            : useMultiPar()                                          ? par2::Tool::MultiPar
                                                                     : par2::Tool::Par2cmdline;
        recipe.par2Family = par2::toolName(tool);
    }
    recipe.sources = capturedSources(options.files);
    return recipe;
}

NgPost::QueueAdmission NgPost::admitPost(const PostingJobOptions &options,
                                         Origin origin,
                                         int tabNumber,
                                         PostingWidget *view)
{
    // A confirmation may run a nested event loop: no shutdown in that gap.
    const auto hold = holdShutdown();
    QueueAdmission admission;
    if (!_queueAdmissionConfirmed(&admission))
        return admission;
    const QueueCommandResult result = _postQueue->submit(_queueRecipe(options), origin, tabNumber);
    admission = _queueAdmitted(result, view);
#ifdef __USE_HMI__
    // The Auto Posting tab lists a monitored file by its path: the line to
    // close if the post leaves without a transfer.
    if (admission.accepted && origin == Origin::Auto && !options.files.isEmpty())
        _autoRows.insert(admission.postId, options.files.first().absoluteFilePath());
#endif
    return admission;
}

NgPost::QueueAdmission NgPost::admitResume(qint64 postId,
                                           const PostingJobOptions &options,
                                           Origin origin,
                                           PostingWidget *view)
{
    const auto hold = holdShutdown();
    QueueAdmission admission;
    if (!_queueAdmissionConfirmed(&admission))
        return admission;
    const QueueCommandResult result = _postQueue->enqueueExisting(postId,
                                                                  _queueRecipe(options),
                                                                  origin);
    return _queueAdmitted(result, view);
}

void NgPost::startQueuedWork()
{
    if (!_queueScheduler)
        return;
    // The user is on the tab that just started: no tab switch, as before.
    const QScopedValueRollback<bool> clicked(_startingFromView, true);
    _queueScheduler->reevaluate();
}

bool NgPost::_queueAdmissionConfirmed(QueueAdmission *admission)
{
    _lastPostingStartCanceled = false;
    if (!_ensureQueueEngine()) {
        admission->error = tr("history store is not available");
        return false;
    }
    // Before anything is written: a refusal leaves no trace to clean up.
    if (!_confirmPostingAdmission()) {
        _lastPostingStartCanceled = true;
        admission->error = tr("Posting canceled by user.");
        return false;
    }
    return true;
}

NgPost::QueueAdmission NgPost::_queueAdmitted(const QueueCommandResult &result, PostingWidget *view)
{
    QueueAdmission admission;
    if (!result.ok()) {
        admission.error = result.message.isEmpty() ? result.errorCode : result.message;
        _error(tr("History database error: %1").arg(admission.error), ERROR_CODE::ERR_HISTORY);
        return admission;
    }
    admission.accepted = true;
    admission.postId = result.postIds.value(0);
#ifdef __USE_HMI__
    if (view)
        _queueViews.insert(admission.postId, view);
#else
    Q_UNUSED(view)
#endif
    emit postingStateChanged();
    return admission;
}

PostingJob *NgPost::_createQueueRuntime(const QueueEntry &entry, const JobExecutionContext &context)
{
#ifdef __USE_HMI__
    PostingWidget *view = _queueViews.value(entry.postId);
#else
    PostingWidget *view = nullptr;
#endif
    // The frozen options of the admission: no new name, password or poster.
    PostingJobOptions options = entry.options.job;
    if (entry.transferManifestEngaged) {
        // Its transfer manifest is engaged: what is left of it is resumed from
        // the history, never prepared nor sent again.
        QString error;
        if (!_queuedResumeOptions(entry.postId, entry.options.job, &options, &error)) {
            _error(tr("Post %1 cannot be resumed: %2").arg(entry.postId).arg(error),
                   ERROR_CODE::ERR_HISTORY);
            return nullptr;
        }
    }
    return new PostingJob(this, options, context, view);
}

bool NgPost::_queuedResumeOptions(qint64 postId,
                                  const PostingJobOptions &original,
                                  PostingJobOptions *options,
                                  QString *error)
{
    PostHistoryStore::PostDetails details;
    QString err;
    if (!_historyService || !_historyService->loadPostDetails(postId, &details, &err)) {
        *error = err.isEmpty() ? tr("history store is not available") : err;
        return false;
    }
    const ResumePlanner::JobPlan plan = ResumePlanner::buildJobPlan(details);
    if (!plan.unavailableSources.isEmpty()) {
        *error = tr("source files are missing or changed");
        return false;
    }
    if (plan.files.isEmpty()) {
        *error = tr("no source file remains available to resume");
        return false;
    }
    QString nzbPath = details.nzbPath;
    if (nzbPath.isEmpty()) {
        nzbPath = _nzbPath + QDir::separator() + details.post.nzbName;
        if (!nzbPath.endsWith(QStringLiteral(".nzb")))
            nzbPath += QStringLiteral(".nzb");
    }
    QList<QString> groups = details.post.groups.split(',', Qt::SkipEmptyParts);
    if (groups.isEmpty())
        groups = getPostingGroups();
    *options = ResumePlanner::jobOptions(_baseJobOptions(), details, nzbPath, groups, plan, from());
    // A password the history does not keep is still known to this session.
    if (options->rarPass.isEmpty()) {
        const QString known = original.rarPass.isEmpty() ? original.declaredPassword
                                                         : original.rarPass;
        if (!known.isEmpty()) {
            options->rarPass = known;
            options->resumePasswordUnavailable = false;
        }
    }
    return true;
}

void NgPost::_onQueueRuntimeActivated(qint64 postId, PostingJob *job)
{
    Q_UNUSED(postId)
    _activeJob = job; // a view on the transfer: the executor owns it
    job->_vpnRequired = _vpnManager && _vpnManager->jobNeedsVpn(_nntpServers);
    _retainVpnForJob(job);
#ifdef __USE_HMI__
    if (_hmi) {
        connect(job,
                &PostingJob::articlesNumber,
                _hmi,
                &MainWindow::onSetProgressBarRange,
                Qt::QueuedConnection);
        if (!job->widget())
            connect(job,
                    &PostingJob::postingStarted,
                    _hmi->autoWidget(),
                    &AutoPostWidget::onMonitorJobStart);
        else
            job->widget()->bindQueueRuntime(job);
        if (!_startingFromView)
            _hmi->setTab(job->widget()); // the queue moved on to this post
    }
#endif
    connect(job,
            &PostingJob::postingStarted,
            this,
            &NgPost::onPostingJobStarted,
            Qt::QueuedConnection);
    for (auto ended : { &PostingJob::postingFinished, &PostingJob::noMoreConnection })
        connect(job, ended, this, [this, job] { _onPostingJobEnded(job); }, Qt::DirectConnection);
    emit postingStateChanged();
}

void NgPost::_onQueueTransferEnded(qint64 postId, PostingJob *job)
{
    Q_UNUSED(postId)
    // The transfer is over: the next post may become the active one while this
    // one finishes its NZB and what follows.
    if (_activeJob == job)
        _activeJob = nullptr;
    _releaseVpnForJob(job);
    emit postingStateChanged();
}

void NgPost::_onQueueRuntimeKept(qint64 postId, PostingJob *job)
{
    Q_UNUSED(postId)
    if (_activeJob == job)
        _activeJob = nullptr;
    _releaseVpnForJob(job);
#ifdef __USE_HMI__
    // Its post is waiting again: its tab says so, and still follows it.
    if (PostingWidget *view = job->widget(); view && view->followsRuntime(job))
        view->onQueueRuntimeKept();
#endif
    emit postingStateChanged();
}

void NgPost::_releaseQueuedPost(qint64 postId, int attempt)
{
    if (!_postQueue || _postQueue->engineReleaseOwnership(postId)) {
        _pendingReleases.remove(postId);
        QMetaObject::invokeMethod(this, &NgPost::maybeFinishApplication, Qt::QueuedConnection);
        return;
    }
    // Not written: the post keeps its owner until it is, and the end waits.
    _pendingReleases.insert(postId);
    if (attempt >= kMaxReleaseAttempts) {
        _pendingReleases.remove(postId); // the lock goes at exit: an orphan, recovered later
        _error(tr("History database error: %1").arg(QStringLiteral("owner_release")),
               ERROR_CODE::ERR_HISTORY);
        QMetaObject::invokeMethod(this, &NgPost::maybeFinishApplication, Qt::QueuedConnection);
        return;
    }
    QTimer::singleShot(500 << attempt, this, [this, postId, attempt] {
        if (_pendingReleases.contains(postId))
            _releaseQueuedPost(postId, attempt + 1);
    });
}

void NgPost::_onQueueRuntimeEnded(qint64 postId, PostingJob *job)
{
#ifdef __USE_HMI__
    // Its tab is over only now, and only if it still follows this post.
    if (PostingWidget *view = job->widget(); view && view->followsRuntime(job))
        view->onPostingJobDone();
#endif
    _recordFinishedJob(job);
    if (_activeJob == job)
        _activeJob = nullptr;
    _releaseVpnForJob(job);
#ifdef __USE_HMI__
    _queueViews.remove(postId);
    _autoRows.remove(postId); // _recordFinishedJob() closed its line
#else
    Q_UNUSED(postId)
#endif
    emit postingStateChanged();
}

void NgPost::_retryMonitoredAdmission(const QFileInfo &fileInfo, const QString &monitorFolder)
{
    const QString path = fileInfo.absoluteFilePath();
    const int attempt = ++_admissionRetries[path];
    if (attempt > kMaxAdmissionRetries) {
        _admissionRetries.remove(path);
        QMetaObject::invokeMethod(this, &NgPost::maybeFinishApplication, Qt::QueuedConnection);
        return; // each refusal was already reported
    }
    const int delayMs = std::min(_admissionRetryBaseMs << (attempt - 1), 30000);
    const quint64 generation = _monitorGeneration;
    QTimer::singleShot(delayMs, this, [this, fileInfo, monitorFolder, generation] {
        if (generation != _monitorGeneration || !QFileInfo::exists(fileInfo.absoluteFilePath())) {
            // Monitoring stopped, or the file left.
            _admissionRetries.remove(fileInfo.absoluteFilePath());
            maybeFinishApplication();
            return;
        }
        _post(fileInfo, monitorFolder);
    });
}

bool NgPost::_removeQueuedPosts(bool monitoredOnly)
{
    QList<qint64> ids;
    for (const QueueEntry &entry : _postQueue->snapshot(Domain::Session)) {
        if (monitoredOnly && entry.origin != Origin::Auto)
            continue;
        // An entry already stopping or finalizing ends on its own.
        if (_postQueue->capabilityOf(Action::Remove, entry.postId).allowed)
            ids << entry.postId;
    }
    if (ids.isEmpty())
        return true;
    const QueueCommandResult removed = _postQueue->remove(ids,
                                                          RemovePolicy::Default,
                                                          _postQueue->view());
    if (!removed.ok())
        _error(tr("History database error: %1")
                   .arg(removed.message.isEmpty() ? removed.errorCode : removed.message),
               ERROR_CODE::ERR_HISTORY);
    // A running entry is only asked to stop once its Quiescing is written:
    // a technical pause here means that request did not leave.
    return removed.ok() && !(_queueScheduler && _queueScheduler->isTechnicallyPaused());
}

bool NgPost::_queueHasWork() const
{
    return _postQueue && !_postQueue->order(Domain::Session).isEmpty();
}

bool NgPost::_queueCanProgress() const
{
    if (!_postQueue || (_queueScheduler && _queueScheduler->isTechnicallyPaused()))
        return false;
    for (const QueueEntry &entry : _postQueue->snapshot(Domain::Session))
        if (entry.hasRuntime() || (!entry.blocked.isBlocked() && !entry.held))
            return true;
    return false;
}
