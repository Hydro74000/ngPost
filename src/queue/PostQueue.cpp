// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// See PostQueue.h. Pattern of every mutation: check the view and the
// capabilities, build one QueueTransaction, commit it, and only then update
// memory and emit (apply()).
//
//========================================================================

#include "PostQueue.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace queue
{

namespace
{

QString codeStale()
{
    return QStringLiteral("stale_view");
}

//! @p ids in the order they have in @p order (relative order of a selection).
QList<qint64> inOrder(const QList<qint64> &order, const QList<qint64> &ids)
{
    QList<qint64> sorted;
    for (qint64 id : order)
        if (ids.contains(id))
            sorted << id;
    return sorted;
}

} // namespace

PostQueue::PostQueue(QueueStore *store, const QString &sessionQueueId, QObject *parent)
    : QObject(parent)
    , _store(store)
    , _sessionQueueId(sessionQueueId)
{
}

void PostQueue::setRerollFunction(std::function<void(PersistedQueueOptions &)> reroll)
{
    _reroll = std::move(reroll);
}

void PostQueue::setRevalidateFunction(std::function<BlockReason(const QueueEntry &)> revalidate)
{
    _revalidate = std::move(revalidate);
}

bool PostQueue::load()
{
    _entries.clear();
    _orders.clear();
    _orderRevisions.clear();
    for (const QueueEntry &entry : _store->loadEntries(_sessionQueueId))
        _entries.insert(entry.postId, entry);
    for (Domain domain : { Domain::Session, Domain::Deferred }) {
        const QList<qint64> ids = _store->loadOrder(_sessionQueueId, domain);
        if (!ids.isEmpty())
            _orders.insert(orderKey(_sessionQueueId, domain), ids);
    }
    _intent = _store->loadIntent(_sessionQueueId).value_or(PostNowIntent{});
    emit changed();
    return true;
}

// --- reading ------------------------------------------------------------------------

QString PostQueue::orderKey(const QString &queueId, Domain domain)
{
    return queueId + QLatin1Char('|') + QString::number(static_cast<int>(domain));
}

PostQueue::OrderRef PostQueue::resolve(Domain domain, const QString &queueId) const
{
    return { queueId.isEmpty() ? _sessionQueueId : queueId, domain };
}

QList<qint64> PostQueue::order(Domain domain, const QString &queueId) const
{
    const OrderRef ref = resolve(domain, queueId);
    return _orders.value(orderKey(ref.queueId, ref.domain));
}

QList<QueueEntry> PostQueue::snapshot(Domain domain, const QString &queueId) const
{
    QList<QueueEntry> entries;
    for (qint64 id : order(domain, queueId))
        entries << _entries.value(id);
    return entries;
}

const QueueEntry *PostQueue::find(qint64 postId) const
{
    auto it = _entries.constFind(postId);
    return it == _entries.cend() ? nullptr : &it.value();
}

QueueView PostQueue::view(Domain domain, const QString &queueId) const
{
    const OrderRef ref = resolve(domain, queueId);
    QueueView v;
    v.queueId = ref.queueId;
    v.domain = ref.domain;
    v.orderRevision = _orderRevisions.value(orderKey(ref.queueId, ref.domain));
    for (qint64 id : order(domain, queueId))
        v.recipeRevisions.insert(id, _entries.value(id).recipeRevision);
    return v;
}

const QueueEntry *PostQueue::entryInSlot(Slot slot) const
{
    for (const QueueEntry &entry : _entries)
        if (entry.slot == slot)
            return &entry;
    return nullptr;
}

const QueueEntry *PostQueue::activeEntry() const
{
    return entryInSlot(Slot::Transfer);
}

const QueueEntry *PostQueue::headEntry() const
{
    // Strict order: the head is the first entry that may transfer next, even
    // while it is still preparing. A ready entry behind it waits.
    for (qint64 id : order(Domain::Session)) {
        const QueueEntry *entry = find(id);
        if (!entry || entry->held || entry->blocked.isBlocked() || entry->slot == Slot::Transfer)
            continue;
        if (entry->lifecycle == Lifecycle::Queued || entry->lifecycle == Lifecycle::Preparing
            || entry->lifecycle == Lifecycle::Prepared)
            return entry;
    }
    return nullptr;
}

QueueFacts PostQueue::factsFor(const QueueEntry &entry) const
{
    QueueFacts facts;
    facts.ownedLocally = entry.domain != Domain::Foreign;
    facts.intentSwitching = !_intent.isNull() && _intent.state == PostNowIntent::State::Switching
        && _intent.targetPostId == entry.postId;
    return facts;
}

Capability PostQueue::capabilityOf(Action action, qint64 postId) const
{
    const QueueEntry *entry = find(postId);
    if (!entry)
        return Capability::no(QStringLiteral("unknown_post"));
    return capability(action, *entry, factsFor(*entry));
}

bool PostQueue::intentTargetValid() const
{
    if (_intent.isNull())
        return false;
    const QueueEntry *target = find(_intent.targetPostId);
    // Preparing is the normal state of a target while the active keeps posting.
    return target && target->domain == Domain::Session && !target->held
        && !target->blocked.isBlocked() && target->recipeRevision == _intent.targetRecipeRevision
        && (target->lifecycle == Lifecycle::Queued || target->lifecycle == Lifecycle::Preparing
            || target->lifecycle == Lifecycle::Prepared);
}

// --- checks ---------------------------------------------------------------------------

QString PostQueue::checkView(const QList<qint64> &ids, const QueueView &seen, bool checkOrder) const
{
    if (ids.isEmpty())
        return QStringLiteral("empty_selection");
    const OrderRef ref = resolve(seen.domain, seen.queueId);
    for (qint64 id : ids) {
        const QueueEntry *entry = find(id);
        if (!entry)
            return QStringLiteral("unknown_post");
        if (entry->queueId != ref.queueId || entry->domain != ref.domain)
            return QStringLiteral("other_domain");
        if (seen.recipeRevisions.value(id, ~quint64(0)) != entry->recipeRevision)
            return codeStale();
    }
    if (checkOrder
        && seen.orderRevision != _orderRevisions.value(orderKey(ref.queueId, ref.domain)))
        return codeStale();
    return {};
}

QString PostQueue::checkAction(Action action, const QList<qint64> &ids) const
{
    for (qint64 id : ids) {
        const Capability cap = capabilityOf(action, id);
        if (!cap.allowed)
            return cap.reason;
    }
    return {};
}

bool PostQueue::intentTouches(const QList<qint64> &ids) const
{
    // Only a pending intent is invalidated. Once switching, the old active is
    // being interrupted and must stay recoverable: the commit checks the target.
    return !_intent.isNull() && _intent.state == PostNowIntent::State::Pending
        && ids.contains(_intent.targetPostId);
}

int PostQueue::firstFreeIndex(const QList<qint64> &rest, const QList<qint64> &moving) const
{
    int index = 0;
    const QueueEntry *active = activeEntry();
    if (active && !rest.isEmpty() && rest.first() == active->postId)
        index = 1;
    if (!_intent.isNull() && !moving.contains(_intent.targetPostId)) {
        const int target = static_cast<int>(rest.indexOf(_intent.targetPostId));
        if (target >= 0)
            index = std::max(index, target + 1);
    }
    return index;
}

// --- commit -----------------------------------------------------------------------------

QueueTransaction::Order PostQueue::orderUpdate(const OrderRef &ref, const QList<qint64> &ids) const
{
    return { ref.queueId,
             ref.domain,
             ids,
             _orderRevisions.value(orderKey(ref.queueId, ref.domain)) + 1 };
}

bool PostQueue::apply(const QueueTransaction &transaction, QueueCommit *result)
{
    const QueueCommit commit = _store->commit(transaction);
    if (result)
        *result = commit;
    if (!commit.ok())
        return false;

    QList<qint64> changedIds;
    for (QueueEntry entry : transaction.upserts) {
        if (entry.postId <= 0)
            entry.postId = commit.assignedIds.value(entry.postId);
        _entries.insert(entry.postId, entry);
        changedIds << entry.postId;
    }
    for (const QueueEntry &entry : transaction.existingAdmissions) {
        _entries.insert(entry.postId, entry);
        changedIds << entry.postId;
    }
    for (auto it = commit.runs.cbegin(); it != commit.runs.cend(); ++it) {
        QueueEntry &entry = _entries[it.key()];
        entry.runId = it.value().runId;
        entry.runNo = it.value().runNo;
        if (!changedIds.contains(it.key()))
            changedIds << it.key();
    }
    QList<qint64> removedIds = transaction.removals;
    for (const FinalizationStep &step : transaction.finalizations)
        removedIds << step.postId;
    for (qint64 id : removedIds)
        _entries.remove(id);
    for (const QueueTransaction::Order &o : transaction.orders) {
        QList<qint64> ids;
        for (qint64 id : o.ids)
            ids << commit.assignedIds.value(id, id);
        _orders.insert(orderKey(o.queueId, o.domain), ids);
        _orderRevisions.insert(orderKey(o.queueId, o.domain), o.revision);
    }
    const bool intentChanges = !transaction.dropIntentId.isEmpty() || transaction.setIntent;
    if (!transaction.dropIntentId.isEmpty() && _intent.intentId == transaction.dropIntentId)
        _intent = {};
    if (transaction.setIntent)
        _intent = *transaction.setIntent;

    for (qint64 id : changedIds)
        emit entryChanged(id);
    for (qint64 id : removedIds)
        emit entryRemoved(id);
    for (const QueueTransaction::Order &o : transaction.orders)
        emit orderChanged(o.queueId, o.domain);
    if (intentChanges)
        emit intentChanged();
    emit changed();
    return true;
}

QueueCommandResult PostQueue::commitOrder(const OrderRef &ref,
                                          const QList<qint64> &ids,
                                          bool dropsIntent)
{
    QueueTransaction tx;
    tx.orders << orderUpdate(ref, ids);
    const qint64 target = _intent.targetPostId;
    if (dropsIntent)
        tx.dropIntentId = _intent.intentId;
    QueueCommit commit;
    if (!apply(tx, &commit))
        return QueueCommandResult::rejected(QStringLiteral("store_failed"), commit.error);
    if (dropsIntent)
        emit intentDropped(target, QStringLiteral("target_moved"));
    return QueueCommandResult::done(ids);
}

void PostQueue::releaseArtifacts(QueueEntry &entry, bool keepCurrent) const
{
    if (entry.artifacts.isNull())
        return;
    if (keepCurrent)
        entry.retainedSets << entry.artifacts;
    entry.artifacts = {};
}

// --- admission ---------------------------------------------------------------------------

QueueEntry PostQueue::newEntry(const PersistedQueueOptions &options,
                               Origin origin,
                               int tabNumber) const
{
    QueueEntry entry;
    entry.queueId = _sessionQueueId;
    entry.domain = Domain::Session;
    entry.origin = origin;
    entry.tabNumber = tabNumber;
    entry.options = options;
    entry.name = QFileInfo(options.job.nzbFilePath).completeBaseName();
    entry.enqueuedAt = QDateTime::currentDateTimeUtc();
    for (const PersistedSource &source : options.sources)
        entry.sourceBytes += source.sizeBytes;
    return entry;
}

QueueCommandResult PostQueue::admitEntry(QueueEntry entry)
{
    entry.postId = _nextPlaceholder--;
    const OrderRef ref = resolve(Domain::Session, {});
    QList<qint64> newOrder = order(Domain::Session);
    newOrder << entry.postId;
    QueueTransaction tx;
    tx.upserts << entry;
    tx.orders << orderUpdate(ref, newOrder);
    QueueCommit commit;
    if (!apply(tx, &commit))
        return QueueCommandResult::rejected(QStringLiteral("store_failed"), commit.error);
    return QueueCommandResult::done({ commit.assignedIds.value(entry.postId) });
}

QueueCommandResult PostQueue::submit(const PersistedQueueOptions &options,
                                     Origin origin,
                                     int tabNumber)
{
    if (options.sources.isEmpty())
        return QueueCommandResult::rejected(QStringLiteral("no_source"));
    return admitEntry(newEntry(options, origin, tabNumber));
}

QueueCommandResult PostQueue::enqueueExisting(qint64 postId,
                                              const PersistedQueueOptions &options,
                                              Origin origin,
                                              int tabNumber)
{
    if (postId <= 0)
        return QueueCommandResult::rejected(QStringLiteral("unknown_post"));
    if (_entries.contains(postId))
        return QueueCommandResult::rejected(QStringLiteral("already_queued"));
    QueueEntry entry = newEntry(options, origin, tabNumber);
    entry.postId = postId;
    // What is left to send was decided by the transfer manifest: no packing.
    entry.transferManifestEngaged = true;
    QList<qint64> newOrder = order(Domain::Session);
    newOrder << postId;
    QueueTransaction tx;
    tx.existingAdmissions << entry;
    tx.orders << orderUpdate(resolve(Domain::Session, {}), newOrder);
    QueueCommit commit;
    if (!apply(tx, &commit))
        return QueueCommandResult::rejected(QStringLiteral("store_failed"), commit.error);
    return QueueCommandResult::done({ postId });
}

QString PostQueue::duplicateNzbPath(const QString &path) const
{
    const QFileInfo info(path);
    QSet<QString> used;
    for (const QueueEntry &entry : _entries)
        used.insert(entry.options.job.nzbFilePath);
    for (int n = 2;; ++n) {
        const QString candidate = info.dir().filePath(
            QStringLiteral("%1 (%2).%3").arg(info.completeBaseName()).arg(n).arg(info.suffix()));
        if (!used.contains(candidate) && !QFileInfo::exists(candidate))
            return candidate;
    }
}

QueueCommandResult PostQueue::duplicate(qint64 postId, const QueueView &seen)
{
    QString error = checkView({ postId }, seen, false);
    if (error.isEmpty())
        error = checkAction(Action::Duplicate, { postId });
    if (!error.isEmpty())
        return QueueCommandResult::rejected(error);
    const QueueEntry &source = _entries[postId];
    PersistedQueueOptions options = source.options;
    options.job.nzbFilePath = duplicateNzbPath(options.job.nzbFilePath);
    if (_reroll)
        _reroll(options);
    QueueEntry copy = newEntry(options, source.origin, 0);
    if (source.unsettledSourceJournal)
        copy.blocked = { QStringLiteral("sources_pending_recovery"), {} };
    return admitEntry(copy);
}

// --- ordering ----------------------------------------------------------------------------

QueueCommandResult PostQueue::move(const QList<qint64> &ids, int targetIndex, const QueueView &seen)
{
    QString error = checkView(ids, seen, true);
    if (error.isEmpty())
        error = checkAction(Action::Move, ids);
    if (!error.isEmpty())
        return QueueCommandResult::rejected(error);

    const OrderRef ref = resolve(seen.domain, seen.queueId);
    const QList<qint64> current = order(ref.domain, ref.queueId);
    const QList<qint64> moving = inOrder(current, ids);
    QList<qint64> result = current;
    for (qint64 id : moving)
        result.removeOne(id);
    const int minIndex = ref.domain == Domain::Session ? firstFreeIndex(result, moving) : 0;
    if (targetIndex < minIndex)
        return QueueCommandResult::rejected(!_intent.isNull() && minIndex > 1
                                                ? QStringLiteral("intent_pending")
                                                : QStringLiteral("before_active"));
    const int at = std::min(targetIndex, static_cast<int>(result.size()));
    for (int i = 0; i < moving.size(); ++i)
        result.insert(at + i, moving.at(i));
    if (result == current)
        return QueueCommandResult::done(moving);
    return commitOrder(ref, result, intentTouches(moving));
}

QueueCommandResult PostQueue::moveToEdge(const QList<qint64> &ids, Edge edge, const QueueView &seen)
{
    const OrderRef ref = resolve(seen.domain, seen.queueId);
    QList<qint64> rest = order(ref.domain, ref.queueId);
    const QList<qint64> m = inOrder(rest, ids);
    for (qint64 id : m)
        rest.removeOne(id);
    const int index = edge == Edge::Bottom ? static_cast<int>(rest.size())
        : ref.domain == Domain::Session    ? firstFreeIndex(rest, m)
                                           : 0;
    return move(ids, index, seen);
}

QueueCommandResult PostQueue::postNext(const QList<qint64> &ids, const QueueView &seen)
{
    if (seen.domain != Domain::Session)
        return QueueCommandResult::rejected(QStringLiteral("other_domain"));
    QString error = checkAction(Action::PostNext, ids);
    if (!error.isEmpty())
        return QueueCommandResult::rejected(error);
    return moveToEdge(ids, Edge::Top, seen);
}

QueueCommandResult PostQueue::postNow(qint64 postId, const QueueView &seen, bool replaceExisting)
{
    QString error = checkView({ postId }, seen, true);
    if (error.isEmpty())
        error = checkAction(Action::PostNow, { postId });
    if (error.isEmpty() && seen.domain != Domain::Session)
        error = QStringLiteral("other_domain");
    if (!error.isEmpty())
        return QueueCommandResult::rejected(error);

    const QueueEntry *active = activeEntry();
    if (!active || active->lifecycle == Lifecycle::Finalizing)
        return postNext({ postId }, seen); // nothing to preempt
    if (!_intent.isNull()) {
        if (_intent.targetPostId == postId)
            return QueueCommandResult::inProgress({ postId });
        if (_intent.state == PostNowIntent::State::Switching)
            return QueueCommandResult::rejected(QStringLiteral("intent_switching"));
        if (!replaceExisting)
            return QueueCommandResult::rejected(QStringLiteral("intent_exists"));
    }

    const OrderRef ref = resolve(Domain::Session, {});
    QList<qint64> newOrder = order(Domain::Session);
    newOrder.removeOne(postId);
    newOrder.insert(std::min(1, static_cast<int>(newOrder.size())), postId);
    PostNowIntent intent;
    intent.intentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    intent.queueId = ref.queueId;
    intent.targetPostId = postId;
    intent.targetRecipeRevision = _entries.value(postId).recipeRevision;
    intent.activeRunId = active->runId;

    QueueTransaction tx;
    tx.orders << orderUpdate(ref, newOrder);
    tx.setIntent = intent;
    tx.dropIntentId = _intent.intentId;
    QueueCommit commit;
    if (!apply(tx, &commit))
        return QueueCommandResult::rejected(QStringLiteral("store_failed"), commit.error);
    return QueueCommandResult::inProgress({ postId });
}

QueueCommandResult PostQueue::cancelPostNow(const QString &intentId)
{
    if (_intent.isNull() || _intent.intentId != intentId)
        return QueueCommandResult::rejected(codeStale());
    if (_intent.state == PostNowIntent::State::Switching)
        return QueueCommandResult::rejected(QStringLiteral("intent_switching"));
    const qint64 target = _intent.targetPostId;
    QueueTransaction tx;
    tx.dropIntentId = intentId;
    QueueCommit commit;
    if (!apply(tx, &commit))
        return QueueCommandResult::rejected(QStringLiteral("store_failed"), commit.error);
    emit intentDropped(target, QStringLiteral("cancelled"));
    return QueueCommandResult::done({ target });
}

// --- hold, edit, remove --------------------------------------------------------------------

QueueCommandResult PostQueue::holdOrRelease(const QList<qint64> &ids, bool held)
{
    QueueTransaction tx;
    QList<qint64> runtime;
    for (qint64 id : ids) {
        QueueEntry entry = _entries.value(id);
        if (held && entry.hasRuntime()) {
            runtime << id;
            continue;
        }
        entry.held = held;
        ++entry.recipeRevision;
        tx.upserts << entry;
    }
    const bool dropsIntent = held && intentTouches(ids);
    const qint64 target = _intent.targetPostId;
    if (dropsIntent)
        tx.dropIntentId = _intent.intentId;
    QueueCommit commit;
    if (!tx.isEmpty() && !apply(tx, &commit))
        return QueueCommandResult::rejected(QStringLiteral("store_failed"), commit.error);
    if (dropsIntent && !runtime.contains(target))
        emit intentDropped(target, QStringLiteral("target_held"));
    for (qint64 id : runtime)
        emit runtimeActionRequested(id, RuntimeRequest::HoldInterrupt);
    return runtime.isEmpty() ? QueueCommandResult::done(ids) : QueueCommandResult::inProgress(ids);
}

QueueCommandResult PostQueue::setHeld(const QList<qint64> &ids, bool held, const QueueView &seen)
{
    QString error = checkView(ids, seen, false);
    if (error.isEmpty())
        error = checkAction(held ? Action::Hold : Action::Release, ids);
    if (!error.isEmpty())
        return QueueCommandResult::rejected(error);
    return holdOrRelease(ids, held);
}

QueueCommandResult PostQueue::edit(qint64 postId,
                                   const PersistedQueueOptions &options,
                                   const QueueView &seen)
{
    QString error = checkView({ postId }, seen, false);
    if (error.isEmpty())
        error = checkAction(Action::Edit, { postId });
    if (!error.isEmpty())
        return QueueCommandResult::rejected(error);

    QueueEntry entry = _entries.value(postId);
    const QSet<FieldClass> changedClass = optionscodec::changedClasses(entry.options, options);
    if (changedClass.isEmpty())
        return QueueCommandResult::done({ postId });
    if (entry.transferManifestEngaged) {
        // Only keepRar: ResumePlanner forces the other operation options.
        if (changedClass != QSet<FieldClass>{ FieldClass::Operation }
            || entry.options.job.delFilesAfterPost != options.job.delFilesAfterPost
            || entry.options.job.overwriteNzb != options.job.overwriteNzb)
            return QueueCommandResult::rejected(QStringLiteral("manifest_engaged"));
    }
    if (entry.lifecycle == Lifecycle::Prepared
        && changedClass.contains(FieldClass::ArtifactRecipe)) {
        releaseArtifacts(entry, entry.options.job.keepRar);
        entry.lifecycle = Lifecycle::Queued;
    }
    entry.options = options;
    entry.name = QFileInfo(options.job.nzbFilePath).completeBaseName();
    ++entry.recipeRevision;

    QueueTransaction tx;
    tx.upserts << entry;
    const bool dropsIntent = intentTouches({ postId });
    if (dropsIntent)
        tx.dropIntentId = _intent.intentId;
    QueueCommit commit;
    if (!apply(tx, &commit))
        return QueueCommandResult::rejected(QStringLiteral("store_failed"), commit.error);
    if (dropsIntent)
        emit intentDropped(postId, QStringLiteral("target_edited"));
    return QueueCommandResult::done({ postId });
}

QueueCommandResult PostQueue::removeData(const QList<qint64> &ids,
                                         RemovePolicy policy,
                                         const QHash<qint64, RunOutcome> &outcomes)
{
    Q_UNUSED(policy) // physical archive cleanup belongs to the recovery lot (C5)
    QueueTransaction tx;
    tx.removals = ids;
    tx.outcomes = outcomes;
    const bool dropsIntent = addLeaving(tx, ids);
    const qint64 target = _intent.targetPostId;
    QueueCommit commit;
    if (!apply(tx, &commit))
        return QueueCommandResult::rejected(QStringLiteral("store_failed"), commit.error);
    if (dropsIntent)
        emit intentDropped(target, QStringLiteral("target_removed"));
    return QueueCommandResult::done(ids);
}

bool PostQueue::addLeaving(QueueTransaction &tx, const QList<qint64> &ids) const
{
    QHash<QString, OrderRef> touched;
    for (qint64 id : ids) {
        const QueueEntry &entry = _entries[id];
        touched.insert(orderKey(entry.queueId, entry.domain), { entry.queueId, entry.domain });
    }
    for (const OrderRef &ref : touched) {
        QList<qint64> rest = order(ref.domain, ref.queueId);
        for (qint64 id : ids)
            rest.removeOne(id);
        tx.orders << orderUpdate(ref, rest);
    }
    const bool dropsIntent = intentTouches(ids);
    if (dropsIntent)
        tx.dropIntentId = _intent.intentId;
    return dropsIntent;
}

QueueCommandResult PostQueue::remove(const QList<qint64> &ids,
                                     RemovePolicy policy,
                                     const QueueView &seen)
{
    QString error = checkView(ids, seen, false);
    if (error.isEmpty())
        error = checkAction(Action::Remove, ids);
    if (!error.isEmpty())
        return QueueCommandResult::rejected(error);

    QList<qint64> data;
    QList<QPair<qint64, RuntimeRequest>> runtime;
    for (qint64 id : ids) {
        const QueueEntry &entry = _entries[id];
        if (entry.lifecycle == Lifecycle::Posting)
            runtime.append({ id, RuntimeRequest::StopTransfer });
        else if (entry.lifecycle == Lifecycle::Preparing)
            runtime.append({ id, RuntimeRequest::CancelPreparation });
        else
            data << id;
    }
    if (!data.isEmpty()) {
        const QueueCommandResult removed = removeData(data, policy);
        if (!removed.ok())
            return removed;
    }
    for (const auto &request : runtime)
        emit runtimeActionRequested(request.first, request.second);
    return runtime.isEmpty() ? QueueCommandResult::done(ids) : QueueCommandResult::inProgress(ids);
}

QueueCommandResult PostQueue::revalidate(qint64 postId, const QueueView &seen)
{
    QString error = checkView({ postId }, seen, false);
    if (error.isEmpty())
        error = checkAction(Action::Revalidate, { postId });
    if (!error.isEmpty())
        return QueueCommandResult::rejected(error);
    QueueEntry entry = _entries.value(postId);
    entry.blocked = _revalidate ? _revalidate(entry) : BlockReason{};
    ++entry.recipeRevision;
    QueueTransaction tx;
    tx.upserts << entry;
    QueueCommit commit;
    if (!apply(tx, &commit))
        return QueueCommandResult::rejected(QStringLiteral("store_failed"), commit.error);
    if (entry.blocked.isBlocked())
        return QueueCommandResult::rejected(QStringLiteral("still_blocked"), entry.blocked.code);
    return QueueCommandResult::done({ postId });
}

QueueCommandResult PostQueue::retryFinalization(qint64 postId, const QueueView &seen)
{
    QString error = checkView({ postId }, seen, false);
    if (error.isEmpty())
        error = checkAction(Action::RetryFinalization, { postId });
    if (!error.isEmpty())
        return QueueCommandResult::rejected(error);
    emit runtimeActionRequested(postId, RuntimeRequest::RetryFinalization);
    return QueueCommandResult::inProgress({ postId });
}

QueueCommandResult PostQueue::abandonPlanning(qint64 postId, const QueueView &seen)
{
    QString error = checkView({ postId }, seen, false);
    if (error.isEmpty())
        error = checkAction(Action::AbandonPlanning, { postId });
    if (!error.isEmpty())
        return QueueCommandResult::rejected(error);
    emit runtimeActionRequested(postId, RuntimeRequest::AbandonPlanning);
    return QueueCommandResult::inProgress({ postId });
}

void PostQueue::setPaused(bool paused)
{
    if (_paused == paused)
        return;
    _paused = paused;
    emit pausedChanged(paused);
    emit changed();
}

// --- engine side ------------------------------------------------------------------------------

bool PostQueue::engineBeginPreparation(qint64 postId, const ArtifactSetRef &set)
{
    if (!_entries.contains(postId))
        return false;
    QueueEntry entry = _entries.value(postId);
    entry.lifecycle = Lifecycle::Preparing;
    entry.slot = Slot::Preparation;
    entry.artifacts = set;
    entry.lastGeneration = std::max(entry.lastGeneration, set.generation);
    QueueTransaction tx;
    tx.upserts << entry;
    return apply(tx);
}

bool PostQueue::engineRecordPrepared(qint64 postId, const ArtifactSetRef &set)
{
    if (!_entries.contains(postId))
        return false;
    QueueEntry entry = _entries.value(postId);
    entry.lifecycle = Lifecycle::Prepared;
    entry.slot = Slot::None;
    entry.artifacts = set;
    QueueTransaction tx;
    tx.upserts << entry;
    return apply(tx);
}

bool PostQueue::engineActivate(qint64 postId)
{
    if (!_entries.contains(postId))
        return false;
    QueueEntry entry = _entries.value(postId);
    entry.lifecycle = Lifecycle::Posting;
    entry.slot = Slot::Transfer;
    const OrderRef ref = resolve(Domain::Session, {});
    QList<qint64> newOrder = order(Domain::Session);
    newOrder.removeOne(postId);
    newOrder.prepend(postId); // the active entry is always first
    QueueTransaction tx;
    tx.upserts << entry;
    tx.orders << orderUpdate(ref, newOrder);
    return apply(tx);
}

bool PostQueue::engineSetLifecycle(qint64 postId, Lifecycle lifecycle, Slot slot)
{
    if (!_entries.contains(postId))
        return false;
    QueueEntry entry = _entries.value(postId);
    entry.lifecycle = lifecycle;
    entry.slot = slot;
    QueueTransaction tx;
    tx.upserts << entry;
    return apply(tx);
}

bool PostQueue::engineBlock(qint64 postId, const BlockReason &reason, Lifecycle lifecycle)
{
    if (!_entries.contains(postId))
        return false;
    QueueEntry entry = _entries.value(postId);
    entry.blocked = reason;
    entry.slot = Slot::None;
    if (lifecycle == Lifecycle::Queued && entry.lifecycle != Lifecycle::Prepared)
        releaseArtifacts(entry, entry.options.job.keepRar);
    entry.lifecycle = lifecycle;
    ++entry.recipeRevision;
    QueueTransaction tx;
    tx.upserts << entry;
    const bool dropsIntent = intentTouches({ postId });
    if (dropsIntent)
        tx.dropIntentId = _intent.intentId;
    if (!apply(tx))
        return false;
    if (dropsIntent)
        emit intentDropped(postId, QStringLiteral("target_blocked"));
    return true;
}

bool PostQueue::engineMarkStuck(qint64 postId, const BlockReason &reason)
{
    if (!_entries.contains(postId))
        return false;
    QueueEntry entry = _entries.value(postId);
    entry.blocked = reason; // lifecycle and slot stay: the runtime still holds them
    ++entry.recipeRevision;
    QueueTransaction tx;
    tx.upserts << entry;
    return apply(tx);
}

namespace
{
//! An interruption acknowledged at last proves the stop that was not.
void clearUnprovenStop(QueueEntry &entry)
{
    if (entry.blocked.code == QLatin1String("quiesce_failed"))
        entry.blocked = {};
}
} // namespace

bool PostQueue::engineInterrupt(qint64 postId, bool manifestEngaged, bool hold, bool keepArtifacts)
{
    if (!_entries.contains(postId))
        return false;
    QueueEntry entry = _entries.value(postId);
    clearUnprovenStop(entry);
    entry.lifecycle = Lifecycle::Queued;
    entry.slot = Slot::None;
    entry.transferManifestEngaged = entry.transferManifestEngaged || manifestEngaged;
    if (hold && !entry.held) {
        entry.held = true;
        ++entry.recipeRevision;
    }
    if (!keepArtifacts)
        releaseArtifacts(entry, entry.options.job.keepRar);
    QueueTransaction tx;
    tx.expectedRuns.insert(postId, entry.runId);
    tx.expectedRevisions.insert(postId, _entries.value(postId).recipeRevision);
    tx.upserts << entry;
    tx.renewRuns << postId;
    const bool dropsIntent = hold && intentTouches({ postId });
    if (dropsIntent)
        tx.dropIntentId = _intent.intentId;
    if (!apply(tx))
        return false;
    if (dropsIntent)
        emit intentDropped(postId, QStringLiteral("target_held"));
    return true;
}

bool PostQueue::engineSetIntentSwitching()
{
    if (_intent.isNull())
        return false;
    PostNowIntent switching = _intent;
    switching.state = PostNowIntent::State::Switching;
    QueueTransaction tx;
    tx.setIntent = switching;
    return apply(tx);
}

bool PostQueue::engineCommitSwitch(bool manifestEngaged)
{
    const QueueEntry *active = activeEntry();
    if (_intent.isNull() || !active)
        return false;
    QueueEntry old = *active;
    clearUnprovenStop(old);
    old.lifecycle = Lifecycle::Queued;
    old.slot = Slot::None;
    old.transferManifestEngaged = old.transferManifestEngaged || manifestEngaged;

    QueueTransaction tx;
    tx.expectedRuns.insert(old.postId, old.runId);
    tx.expectedRevisions.insert(old.postId, old.recipeRevision);
    tx.upserts << old;
    tx.renewRuns << old.postId;
    tx.dropIntentId = _intent.intentId;
    const QueueEntry *target = find(_intent.targetPostId);
    if (intentTargetValid() && target->isReady()) {
        QList<qint64> newOrder = order(Domain::Session);
        newOrder.removeOne(_intent.targetPostId);
        newOrder.removeOne(old.postId);
        newOrder.prepend(old.postId);
        newOrder.prepend(_intent.targetPostId);
        tx.orders << orderUpdate(resolve(Domain::Session, {}), newOrder);
    }
    return apply(tx);
}

bool PostQueue::engineDropIntent(const QString &reason)
{
    if (_intent.isNull())
        return false;
    const qint64 target = _intent.targetPostId;
    QueueTransaction tx;
    tx.dropIntentId = _intent.intentId;
    if (!apply(tx))
        return false;
    emit intentDropped(target, reason);
    return true;
}

bool PostQueue::engineSetSourceJournal(qint64 postId, bool unsettled)
{
    if (!_entries.contains(postId))
        return false;
    QueueEntry entry = _entries.value(postId);
    entry.unsettledSourceJournal = unsettled;
    QueueTransaction tx;
    tx.upserts << entry;
    return apply(tx);
}

bool PostQueue::engineRetire(qint64 postId)
{
    if (!_entries.contains(postId))
        return false;
    return removeData({ postId }, RemovePolicy::Default).ok();
}

bool PostQueue::engineRecordResult(const AttemptResult &attempt)
{
    if (!_entries.contains(attempt.postId))
        return false;
    QueueEntry entry = _entries.value(attempt.postId);
    entry.lifecycle = Lifecycle::Finalizing; // F1 and the move to finalization together
    entry.slot = Slot::Transfer;
    QueueTransaction tx;
    tx.expectedRuns.insert(attempt.postId, attempt.runId);
    tx.expectedRevisions.insert(attempt.postId, entry.recipeRevision);
    tx.upserts << entry;
    tx.results << attempt;
    return apply(tx);
}

bool PostQueue::engineFinishFinalization(qint64 postId,
                                         const QString &runId,
                                         FinalizationStep::Kind kind)
{
    if (!_entries.contains(postId))
        return false;
    QueueTransaction tx;
    tx.expectedRuns.insert(postId, runId);
    tx.expectedRevisions.insert(postId, _entries.value(postId).recipeRevision);
    tx.finalizations << FinalizationStep{ postId, runId, kind };
    const bool dropsIntent = addLeaving(tx, { postId });
    if (!apply(tx))
        return false;
    if (dropsIntent)
        emit intentDropped(postId, QStringLiteral("target_removed"));
    return true;
}

bool PostQueue::engineKeepForRestart(qint64 postId,
                                     const QString &runId,
                                     qint64 activeMs,
                                     bool manifestEngaged,
                                     bool hold,
                                     bool keepArtifacts)
{
    if (!_entries.contains(postId))
        return false;
    QueueEntry entry = _entries.value(postId);
    QueueTransaction tx;
    tx.expectedRuns.insert(postId, runId);
    tx.expectedRevisions.insert(postId, entry.recipeRevision);
    clearUnprovenStop(entry);
    entry.lifecycle = Lifecycle::Queued;
    entry.slot = Slot::None;
    entry.transferManifestEngaged = entry.transferManifestEngaged || manifestEngaged;
    if (hold && !entry.held) {
        entry.held = true;
        ++entry.recipeRevision;
    }
    if (!keepArtifacts)
        releaseArtifacts(entry, entry.options.job.keepRar);
    tx.upserts << entry;
    tx.renewRuns << postId;
    tx.interruptedActiveMs.insert(postId, activeMs);
    const bool dropsIntent = hold && intentTouches({ postId });
    if (dropsIntent)
        tx.dropIntentId = _intent.intentId;
    if (!apply(tx))
        return false;
    if (dropsIntent)
        emit intentDropped(postId, QStringLiteral("target_held"));
    return true;
}

bool PostQueue::engineReleaseOwnership(qint64 postId)
{
    if (_entries.contains(postId))
        return false; // still queued: its owner is still needed
    QueueTransaction tx;
    tx.releases << postId;
    return apply(tx);
}

bool PostQueue::engineFailPreparation(qint64 postId, PrepStep step, const QString &error)
{
    if (!_entries.contains(postId))
        return false;
    RunOutcome outcome;
    outcome.result = QStringLiteral("failed");
    outcome.endCause = QStringLiteral("technical");
    outcome.diagnosticJson = QString::fromUtf8(
        QJsonDocument(
            QJsonObject{ { QStringLiteral("preparation_failed"),
                           QJsonObject{ { QStringLiteral("step"), static_cast<int>(step) },
                                        { QStringLiteral("error"), error } } } })
            .toJson(QJsonDocument::Compact));
    return removeData({ postId }, RemovePolicy::Default, { { postId, outcome } }).ok();
}

} // namespace queue
