// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>

#include "InMemoryQueueStore.h"

#include <QSet>
#include <QUuid>

namespace queue
{

QString InMemoryQueueStore::orderKey(const QString &queueId, Domain domain)
{
    return queueId + QLatin1Char('|') + QString::number(static_cast<int>(domain));
}

void InMemoryQueueStore::failNextCommits(int count, const QString &error)
{
    _failures = count;
    _failureError = error;
}

//! The invariants a SQL transaction would enforce with constraints.
QString InMemoryQueueStore::validate(const QueueTransaction &transaction) const
{
    QSet<qint64> known(_entries.keyBegin(), _entries.keyEnd());
    for (const QueueEntry &entry : transaction.upserts)
        known.insert(entry.postId);
    for (const QueueEntry &entry : transaction.existingAdmissions) {
        if (entry.postId <= 0 || _entries.contains(entry.postId))
            return QStringLiteral("resume_not_admissible");
        known.insert(entry.postId);
    }
    for (qint64 id : transaction.removals)
        known.remove(id);
    for (qint64 id : transaction.renewRuns)
        if (!_entries.contains(id))
            return QStringLiteral("renew_unknown_post");
    for (const QueueTransaction::Order &order : transaction.orders) {
        if (QSet<qint64>(order.ids.cbegin(), order.ids.cend()).size() != order.ids.size())
            return QStringLiteral("duplicate_in_order");
        for (qint64 id : order.ids)
            if (!known.contains(id))
                return QStringLiteral("order_unknown_post");
    }
    if (transaction.setIntent && !known.contains(transaction.setIntent->targetPostId))
        return QStringLiteral("intent_unknown_target");
    return {};
}

//! The rules the history database enforces on runs: a command for a replaced
//! run fails, F1 is recorded once, F3 needs F1.
QString InMemoryQueueStore::validateRuns(const QueueTransaction &transaction) const
{
    const auto current = [this](qint64 postId, const QString &runId) {
        return _entries.contains(postId) && _entries.value(postId).runId == runId;
    };
    QString shape = transaction.shapeError();
    if (!shape.isEmpty())
        return shape;
    for (qint64 id : transaction.renewRuns)
        if (_results.contains(_entries.value(id).runId))
            return QStringLiteral("finalization_engaged");
    for (auto it = transaction.expectedRuns.cbegin(); it != transaction.expectedRuns.cend(); ++it)
        if (!current(it.key(), it.value()))
            return QStringLiteral("stale_run");
    for (auto it = transaction.expectedRevisions.cbegin();
         it != transaction.expectedRevisions.cend();
         ++it)
        if (_entries.value(it.key()).recipeRevision != it.value())
            return QStringLiteral("stale_revision");
    QHash<QString, QString> batch; // run id -> result within this transaction
    for (const AttemptResult &attempt : transaction.results) {
        if (!current(attempt.postId, attempt.runId))
            return QStringLiteral("stale_run");
        const auto recorded = _results.constFind(attempt.runId);
        if ((recorded != _results.cend() && recorded->result != attempt.result)
            || batch.value(attempt.runId, attempt.result) != attempt.result)
            return QStringLiteral("contradictory_result");
        batch.insert(attempt.runId, attempt.result);
    }
    for (const FinalizationStep &step : transaction.finalizations)
        if (!current(step.postId, step.runId))
            return QStringLiteral("stale_run");
    for (const FinalizationStep &step : transaction.finalizations) {
        bool hasResult = _results.contains(step.runId);
        for (const AttemptResult &attempt : transaction.results)
            hasResult = hasResult || attempt.runId == step.runId;
        if (step.kind == FinalizationStep::Kind::Complete && !hasResult)
            return QStringLiteral("finalization_without_result");
    }
    return {};
}

QueueCommit InMemoryQueueStore::commit(const QueueTransaction &transaction)
{
    QueueCommit result;
    if (_failures > 0) {
        --_failures;
        result.error = _failureError;
        return result;
    }
    if (_refuse && _refuse(transaction)) {
        result.error = QStringLiteral("injected_failure");
        return result;
    }
    result.error = validate(transaction);
    if (result.ok())
        result.error = validateRuns(transaction);
    if (!result.ok())
        return result;

    for (QueueEntry entry : transaction.upserts) {
        if (entry.postId <= 0) {
            const qint64 placeholder = entry.postId;
            entry.postId = _nextPostId++;
            entry.runNo = 1;
            entry.runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            _runNumbers[entry.postId] = 1;
            result.assignedIds.insert(placeholder, entry.postId);
            result.runs.insert(entry.postId, { entry.runId, entry.runNo });
        }
        _entries.insert(entry.postId, entry);
    }
    for (QueueEntry entry : transaction.existingAdmissions) {
        entry.runNo = ++_runNumbers[entry.postId];
        entry.runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        result.runs.insert(entry.postId, { entry.runId, entry.runNo });
        _entries.insert(entry.postId, entry);
    }
    for (const AttemptResult &attempt : transaction.results)
        if (!_results.contains(attempt.runId)) // the first one stays: a repeat changes nothing
            _results.insert(attempt.runId, attempt);
    for (qint64 id : transaction.renewRuns) {
        QueueEntry &entry = _entries[id];
        ++_interruptions[entry.runId];
        _keptActiveMs[entry.runId] += transaction.interruptedActiveMs.value(id);
        entry.runNo = ++_runNumbers[id];
        entry.runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        result.runs.insert(id, { entry.runId, entry.runNo });
    }
    for (qint64 id : transaction.removals) {
        _entries.remove(id);
        if (transaction.outcomes.contains(id))
            _outcomes.insert(id, transaction.outcomes.value(id));
    }
    for (const FinalizationStep &step : transaction.finalizations) {
        _entries.remove(step.postId);
        _finalizations.insert(step.runId, step.kind);
    }
    for (qint64 id : transaction.releases)
        _released.insert(id);
    for (const QueueTransaction::Order &order : transaction.orders) {
        QList<qint64> ids;
        for (qint64 id : order.ids)
            ids << result.assignedIds.value(id, id);
        _orders.insert(orderKey(order.queueId, order.domain), ids);
    }
    if (!transaction.dropIntentId.isEmpty()) {
        for (auto it = _intents.begin(); it != _intents.end();) {
            it = it->intentId == transaction.dropIntentId ? _intents.erase(it) : std::next(it);
        }
    }
    if (transaction.setIntent)
        _intents.insert(transaction.setIntent->queueId, *transaction.setIntent);
    ++_commits;
    return result;
}

QList<QueueEntry> InMemoryQueueStore::loadEntries(const QString &queueId) const
{
    QList<QueueEntry> entries;
    for (const QueueEntry &entry : _entries)
        if (entry.queueId == queueId)
            entries << entry;
    return entries;
}

QList<qint64> InMemoryQueueStore::loadOrder(const QString &queueId, Domain domain) const
{
    return _orders.value(orderKey(queueId, domain));
}

std::optional<PostNowIntent> InMemoryQueueStore::loadIntent(const QString &queueId) const
{
    if (!_intents.contains(queueId))
        return std::nullopt;
    return _intents.value(queueId);
}

} // namespace queue
