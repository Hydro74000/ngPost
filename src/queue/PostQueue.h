// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// The queue manager's model (plan_queue.md §3.1, §3.5, §3.6).
//
// PostQueue owns the entries, their orders and the "post now" intent. Every
// change goes through the QueueStore first and is published in memory only
// once committed. User commands carry the QueueView they were issued from:
// a stale view is rejected, never applied to an order the user did not see.
//
// PostQueue makes no scheduling decision. QueueScheduler drives the runtimes
// and reports their progress through the engine* methods.
//
//========================================================================

#ifndef QUEUE_POSTQUEUE_H
#define QUEUE_POSTQUEUE_H

#include "queue/QueueCapabilities.h"
#include "queue/QueueEntry.h"
#include "queue/QueueStore.h"

#include <QHash>
#include <QObject>

#include <functional>

namespace queue
{

//! What a runtime must do for a command PostQueue cannot complete alone.
enum class RuntimeRequest {
    HoldInterrupt,     //!< hold an entry that is preparing or posting
    StopTransfer,      //!< remove the active entry: terminal stop
    CancelPreparation, //!< remove an entry that is preparing
    RetryFinalization,
    AbandonPlanning
};

class PostQueue : public QObject
{
    Q_OBJECT

public:
    PostQueue(QueueStore *store, const QString &sessionQueueId, QObject *parent = nullptr);

    //! Reads what the store holds for this session queue (and its deferred lot)
    //! into memory. Used once, at start, before any command.
    bool load();

    // --- reading ---------------------------------------------------------------
    QString sessionQueueId() const { return _sessionQueueId; }
    QList<qint64> order(Domain domain = Domain::Session, const QString &queueId = {}) const;
    QList<QueueEntry> snapshot(Domain domain = Domain::Session, const QString &queueId = {}) const;
    const QueueEntry *find(qint64 postId) const;
    QueueView view(Domain domain = Domain::Session, const QString &queueId = {}) const;
    //! Entry holding the transfer slot (posting, quiescing or finalizing), or null.
    const QueueEntry *activeEntry() const;
    const QueueEntry *entryInSlot(Slot slot) const;
    //! First session entry that may transfer next, preparing or not, or null.
    const QueueEntry *headEntry() const;
    const PostNowIntent &intent() const { return _intent; }
    bool isPaused() const { return _paused; }
    Capability capabilityOf(Action action, qint64 postId) const;

    //! Draws new random names and passwords for a duplicate (set by NgPost).
    void setRerollFunction(std::function<void(PersistedQueueOptions &)> reroll);
    //! Checks again a blocked entry; returns an empty reason when it can run.
    void setRevalidateFunction(std::function<BlockReason(const QueueEntry &)> revalidate);

    // --- user commands (plan_queue.md §3.5) ---------------------------------------
    QueueCommandResult submit(const PersistedQueueOptions &options,
                              Origin origin,
                              int tabNumber = 0);
    QueueCommandResult move(const QList<qint64> &ids, int targetIndex, const QueueView &seen);
    QueueCommandResult moveToEdge(const QList<qint64> &ids, Edge edge, const QueueView &seen);
    QueueCommandResult postNext(const QList<qint64> &ids, const QueueView &seen);
    QueueCommandResult postNow(qint64 postId, const QueueView &seen, bool replaceExisting = false);
    QueueCommandResult cancelPostNow(const QString &intentId);
    QueueCommandResult setHeld(const QList<qint64> &ids, bool held, const QueueView &seen);
    QueueCommandResult edit(qint64 postId,
                            const PersistedQueueOptions &options,
                            const QueueView &seen);
    QueueCommandResult duplicate(qint64 postId, const QueueView &seen);
    QueueCommandResult remove(const QList<qint64> &ids, RemovePolicy policy, const QueueView &seen);
    QueueCommandResult revalidate(qint64 postId, const QueueView &seen);
    QueueCommandResult retryFinalization(qint64 postId, const QueueView &seen);
    QueueCommandResult abandonPlanning(qint64 postId, const QueueView &seen);
    void setPaused(bool paused);

    // --- engine side, used by QueueScheduler ---------------------------------------
    bool engineBeginPreparation(qint64 postId, const ArtifactSetRef &set);
    bool engineRecordPrepared(qint64 postId, const ArtifactSetRef &set);
    bool engineActivate(qint64 postId);
    bool engineSetLifecycle(qint64 postId, Lifecycle lifecycle, Slot slot);
    //! Blocks an entry and frees its slot; its artifacts follow keepRar.
    bool engineBlock(qint64 postId, const BlockReason &reason, Lifecycle lifecycle);
    //! Interruption kept: interrupted run recorded, successor run attached.
    bool engineInterrupt(qint64 postId, bool manifestEngaged, bool hold, bool keepArtifacts);
    bool engineSetIntentSwitching();
    //! "Post now" switch: interrupts the old active and, when the target is
    //! still valid, puts it first, all in one transaction.
    bool engineCommitSwitch(bool manifestEngaged);
    bool engineDropIntent(const QString &reason);
    //! Renamed sources are not all back in place (journal not settled), or are.
    bool engineSetSourceJournal(qint64 postId, bool unsettled);
    //! The entry leaves the queue (F3 done, or a removal completed).
    bool engineRetire(qint64 postId);
    //! Whether the intent target can still be switched to.
    bool intentTargetValid() const;

signals:
    void entryChanged(qint64 postId);
    void entryRemoved(qint64 postId);
    void orderChanged(const QString &queueId, queue::Domain domain);
    void intentChanged();
    void intentDropped(qint64 targetPostId, const QString &reason);
    void pausedChanged(bool paused);
    void runtimeActionRequested(qint64 postId, queue::RuntimeRequest request);
    //! Something changed that may let the scheduler start work.
    void changed();

private:
    struct OrderRef
    {
        QString queueId;
        Domain domain = Domain::Session;
    };

    static QString orderKey(const QString &queueId, Domain domain);
    OrderRef resolve(Domain domain, const QString &queueId) const;
    QString checkView(const QList<qint64> &ids, const QueueView &seen, bool checkOrder) const;
    QString checkAction(Action action, const QList<qint64> &ids) const;
    QueueFacts factsFor(const QueueEntry &entry) const;
    int firstFreeIndex(const QList<qint64> &order, const QList<qint64> &moving) const;

    //! Commits, then applies to memory and emits; false leaves memory untouched.
    bool apply(const QueueTransaction &transaction, QueueCommit *result = nullptr);
    QueueTransaction::Order orderUpdate(const OrderRef &ref, const QList<qint64> &ids) const;
    QueueCommandResult commitOrder(const OrderRef &ref, const QList<qint64> &ids, bool dropsIntent);
    void releaseArtifacts(QueueEntry &entry, bool keepCurrent) const;
    QueueCommandResult holdOrRelease(const QList<qint64> &ids, bool held);
    QueueCommandResult removeData(const QList<qint64> &ids, RemovePolicy policy);
    QString duplicateNzbPath(const QString &path) const;
    QueueCommandResult admitEntry(QueueEntry entry);
    QueueEntry newEntry(const PersistedQueueOptions &options, Origin origin, int tabNumber) const;
    bool intentTouches(const QList<qint64> &ids) const;

    QueueStore *_store;
    QString _sessionQueueId;
    QHash<qint64, QueueEntry> _entries;
    QHash<QString, QList<qint64>> _orders;
    QHash<QString, quint64> _orderRevisions;
    PostNowIntent _intent;
    bool _paused = false;
    qint64 _nextPlaceholder = -1;

    std::function<void(PersistedQueueOptions &)> _reroll;
    std::function<BlockReason(const QueueEntry &)> _revalidate;
};

} // namespace queue

Q_DECLARE_METATYPE(queue::RuntimeRequest)

#endif // QUEUE_POSTQUEUE_H
