// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// One entry of the queue: a copyable value, no runtime pointer, no widget
// (plan_queue.md §3.2).
//
//========================================================================

#ifndef QUEUE_QUEUEENTRY_H
#define QUEUE_QUEUEENTRY_H

#include "PostingJobOptionsCodec.h"
#include "queue/QueueTypes.h"

#include <QDateTime>
#include <QList>
#include <QString>

namespace queue
{

//! Which scheduler resource an entry occupies.
enum class Slot {
    None,
    Transfer,
    Preparation
};

struct QueueEntry
{
    qint64 postId = 0;
    QString runId; //!< attempt tied to the entry; renewed after every interruption
    int runNo = 0; //!< 1-based number of that attempt for this post
    QString queueId;
    Domain domain = Domain::Session;

    //! Bumped by recipe edits, hold, block and domain changes; never by progress.
    quint64 recipeRevision = 0;

    Origin origin = Origin::Quick;
    int tabNumber = 0; //!< reserved Quick Post number, 0 when none
    Lifecycle lifecycle = Lifecycle::Queued;
    Slot slot = Slot::None;

    bool held = false;
    BlockReason blocked;

    //! A transfer manifest was committed: the entry is a resume, even back in
    //! Queued, and its recipe can no longer change.
    bool transferManifestEngaged = false;
    //! Sources renamed by an obfuscation are not all back in place yet.
    bool unsettledSourceJournal = false;

    ArtifactSetRef artifacts;           //!< set being prepared or verified, if any
    QList<ArtifactSetRef> retainedSets; //!< invalidated sets kept by keepRar
    int lastGeneration = 0;

    QString name; //!< display name: the nzb base name
    qint64 sourceBytes = 0;
    QDateTime enqueuedAt;

    PersistedQueueOptions options;

    bool needsPreparation() const
    {
        return !transferManifestEngaged && (options.job.doCompress || options.job.doPar2);
    }
    //! Can take the transfer slot as soon as it is its turn.
    bool isReady() const
    {
        return lifecycle == Lifecycle::Prepared
            || (lifecycle == Lifecycle::Queued && !needsPreparation());
    }
    bool hasRuntime() const { return slot != Slot::None; }
    bool isEligible() const
    {
        return domain == Domain::Session && !held && !blocked.isBlocked() && slot == Slot::None
            && (lifecycle == Lifecycle::Queued || lifecycle == Lifecycle::Prepared);
    }
    qint64 retainedBytes() const
    {
        qint64 total = artifacts.bytes;
        for (const ArtifactSetRef &set : retainedSets)
            total += set.bytes;
        return total;
    }
};

} // namespace queue

#endif // QUEUE_QUEUEENTRY_H
