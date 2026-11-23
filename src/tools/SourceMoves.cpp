// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// The journaled moves of an obfuscation: see SourceMoves.h.
//
//========================================================================

#include "tools/SourceMoves.h"

#include "history/PostHistoryStore.h"
#include "utils/NativeFileIdentity.h"
#include "utils/NoReplaceRename.h"

#include <QFileInfo>

namespace sourcemoves
{

namespace
{

using Presence = NativeFileIdentity::Presence;

//! What a path holds against the identity recorded for it.
enum class Seen {
    Ours,    //!< the same file, the same kind, no link
    Absent,  //!< nothing there, the system says so
    Foreign, //!< something else
    Unknown  //!< could not be inspected: never an absence
};

Seen look(const QString &path, const NativeFileIdentity &recorded)
{
    NativeFileIdentity current;
    switch (NativeFileIdentity::probe(path, &current)) {
    case Presence::Absent: return Seen::Absent;
    case Presence::Unknown: return Seen::Unknown;
    case Presence::Present: break;
    }
    return current.sameFile(recorded) && current.directory == recorded.directory && !current.link
        ? Seen::Ours
        : Seen::Foreign;
}

//! Where the file of a move is, from the disk alone.
enum class Place {
    Source, //!< at its source only
    Staged, //!< at its staged path only
    Neither //!< both, none, another file or unknown: ambiguous
};

Place locate(const QString &source, const QString &staged, const NativeFileIdentity &recorded)
{
    const Seen atSource = look(source, recorded);
    const Seen atStaged = look(staged, recorded);
    if (atSource == Seen::Ours && atStaged == Seen::Absent)
        return Place::Source;
    if (atStaged == Seen::Ours && atSource == Seen::Absent)
        return Place::Staged;
    return Place::Neither;
}

Result recorded(PostHistoryStore &store,
                qint64 postId,
                const QString &runId,
                int sequence,
                const QString &state,
                Outcome outcome,
                Where where)
{
    Result result;
    result.planned = true;
    QString error;
    if (store.resolveSourceMoves(postId, runId, { sequence }, state, &error)) {
        result.outcome = outcome;
        return result;
    }
    result.outcome = Outcome::Unrecorded;
    result.where = where;
    result.error = error;
    return result;
}

} // namespace

Result moveOut(PostHistoryStore &store,
               const Run &run,
               int sequence,
               const QString &source,
               const QString &target)
{
    Result result;
    NativeFileIdentity identity;
    if (run.setId.isEmpty() || NativeFileIdentity::probe(source, &identity) != Presence::Present
        || identity.link) {
        result.error = QStringLiteral("source_unusable: %1").arg(source);
        return result; // Refused: nothing recorded, nothing moved
    }
    // A sequence is used once: what the journal holds is never moved again.
    QString known;
    if (store.ownedSourceMove(run.postId, run.runId, sequence, &known)
        || !known.startsWith(QLatin1String("source_move_unknown"))) {
        result.error = known.isEmpty() ? QStringLiteral("source_move_used: %1").arg(sequence)
                                       : known;
        return result; // Refused
    }
    PostHistoryStore::SourceMove move;
    move.sequence = sequence;
    move.sourcePath = source;
    move.stagedPath = target;
    move.identityJson = identity.toJson();
    // The intent first, with its owner, run and set checked: a refusal moves nothing.
    if (!store.planSourceMoves(run.postId, run.runId, run.setId, { move }, &result.error)) {
        result.outcome = Outcome::Unrecorded;
        result.where = Where::Source;
        return result;
    }
    int code = 0;
    const noreplace::Result renamed = noreplace::rename(source, target, &code);
    // Whatever the call said, the disk decides.
    switch (locate(source, target, identity)) {
    case Place::Staged:
        return recorded(store,
                        run.postId,
                        run.runId,
                        sequence,
                        QStringLiteral("moved"),
                        Outcome::Moved,
                        Where::Staged);
    case Place::Source: {
        Result kept = recorded(store,
                               run.postId,
                               run.runId,
                               sequence,
                               QStringLiteral("restored"),
                               Outcome::NotMoved,
                               Where::Source);
        if (kept.outcome == Outcome::NotMoved)
            kept.error =
                QStringLiteral("not_moved: %1 (%2)").arg(static_cast<int>(renamed)).arg(code);
        return kept;
    }
    case Place::Neither: break;
    }
    return recorded(store,
                    run.postId,
                    run.runId,
                    sequence,
                    QStringLiteral("blocked"),
                    Outcome::Blocked,
                    Where::Unknown);
}

Result moveBack(PostHistoryStore &store, qint64 postId, const QString &runId, int sequence)
{
    Result result;
    // Its owner, post and run, before anything on disk.
    const auto move = store.ownedSourceMove(postId, runId, sequence, &result.error);
    if (!move)
        return result; // Refused
    result.planned = true;
    if (move->state == QLatin1String("restored")) {
        result.outcome = Outcome::Restored; // settled already: a replay touches nothing
        return result;
    }
    // Only once nothing its tools started can still use the paths, as recorded.
    if (move->setId.isEmpty()) {
        return recorded(store,
                        postId,
                        runId,
                        sequence,
                        QStringLiteral("blocked"),
                        Outcome::Blocked,
                        Where::Unknown);
    }
    const auto set = store.artifactSet(move->setId, &result.error);
    if (!set || !PostHistoryStore::toolStopProven(*set)) {
        result.outcome = Outcome::NotProven;
        return result;
    }
    const auto identity = NativeFileIdentity::fromJson(move->identityJson);
    if (!identity) {
        return recorded(store,
                        postId,
                        runId,
                        sequence,
                        QStringLiteral("blocked"),
                        Outcome::Blocked,
                        Where::Unknown);
    }
    Place place = locate(move->sourcePath, move->stagedPath, *identity);
    if (place == Place::Staged) {
        noreplace::rename(move->stagedPath, move->sourcePath);
        place = locate(move->sourcePath, move->stagedPath, *identity);
    }
    // Back on disk for good before it is recorded so (a power cut too), also
    // when an earlier call put it back and could not record it.
    if (place == Place::Source
        && (!noreplace::syncDirectory(QFileInfo(move->sourcePath).absolutePath())
            || !noreplace::syncDirectory(QFileInfo(move->stagedPath).absolutePath()))) {
        result.outcome = Outcome::Unrecorded;
        result.where = Where::Source;
        result.error = QStringLiteral("not_synced: %1").arg(move->sourcePath);
        return result;
    }
    if (place == Place::Source)
        return recorded(store,
                        postId,
                        runId,
                        sequence,
                        QStringLiteral("restored"),
                        Outcome::Restored,
                        Where::Source);
    // Renaming it back was refused (still aside), or the disk is ambiguous.
    return recorded(store,
                    postId,
                    runId,
                    sequence,
                    QStringLiteral("blocked"),
                    Outcome::Blocked,
                    place == Place::Staged ? Where::Staged : Where::Unknown);
}

Settlement settle(PostHistoryStore &store, qint64 postId, QString *error)
{
    Settlement settled;
    QString err;
    const QList<PostHistoryStore::SourceMove> moves = store.unsettledSourceMoves(postId, &err);
    if (!err.isEmpty()) {
        if (error)
            *error = err;
        ++settled.unrecorded;
        return settled;
    }
    for (const PostHistoryStore::SourceMove &move : moves) {
        const Result result = moveBack(store, postId, move.runId, move.sequence);
        switch (result.outcome) {
        case Outcome::Restored: ++settled.restored; break;
        case Outcome::Blocked: ++settled.blocked; break;
        case Outcome::Unrecorded:
            ++settled.unrecorded;
            if (error)
                *error = result.error;
            break;
        default: ++settled.untouched; break;
        }
    }
    return settled;
}

} // namespace sourcemoves
