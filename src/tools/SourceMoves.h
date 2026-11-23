// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// The journaled moves of an obfuscation (plan_queue.md lot C5.7): a source
// moved aside under a random name, then put back. Before each rename its
// native identity and the intent are recorded (nothing moves otherwise),
// then the rename (atomic, never a copy nor a replacement), then its outcome.
// Putting back happens only once the tools of its artifact set are proven
// stopped in the history, and decides from the disk, never from a return
// code alone:
//   original alone and the same file  -> settled (restored)
//   staged copy alone and the same file -> renamed back, then restored
//   both there, neither, another file, or a path that cannot be inspected
//                                      -> blocked, nothing overwritten
// A sequence is used once; a return is made durable (fsync) before it is
// recorded. Used for the queue's runtimes, and directly by tests after
// reopening a store.
//
//========================================================================

#ifndef TOOLS_SOURCEMOVES_H
#define TOOLS_SOURCEMOVES_H

#include <QString>
#include <QtGlobal>

class PostHistoryStore;

namespace sourcemoves
{

struct Run
{
    qint64 postId = 0;
    QString runId;
    QString setId; //!< required: without it no stop can ever be proven
};

enum class Outcome {
    Moved,     //!< moved aside, recorded
    NotMoved,  //!< still at its place, recorded: another target may be tried
    Restored,  //!< back under its name (or never left), recorded
    Blocked,   //!< the disk is ambiguous: nothing touched, recorded as blocked
    NotProven, //!< its tools are not proven stopped: nothing touched nor recorded
    Refused,   //!< not this session's, or unusable: nothing touched nor recorded
    Unrecorded //!< the history refused a write: see where the file is
};

enum class Where {
    Source,
    Staged,
    Unknown
};

struct Result
{
    Outcome outcome = Outcome::Refused;
    Where where = Where::Unknown; //!< where the file is, when that is not recorded
    bool planned = false;         //!< its intent is in the journal: it must be settled
    QString error;
};

//! Moves @p source to @p target as move @p sequence of @p run.
Result moveOut(PostHistoryStore &store,
               const Run &run,
               int sequence,
               const QString &source,
               const QString &target);

//! Puts move @p sequence of @p runId back, see the table above. A move
//! already restored is not touched again.
Result moveBack(PostHistoryStore &store, qint64 postId, const QString &runId, int sequence);

struct Settlement
{
    int restored = 0;
    int blocked = 0;
    int untouched = 0;  //!< not proven, or refused
    int unrecorded = 0; //!< the history refused: still to settle
};

//! moveBack() for every move of @p postId not settled yet.
Settlement settle(PostHistoryStore &store, qint64 postId, QString *error = nullptr);

} // namespace sourcemoves

#endif // TOOLS_SOURCEMOVES_H
