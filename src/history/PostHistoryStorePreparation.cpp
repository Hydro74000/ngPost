// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Recoverable preparation (plan_queue.md §5.5, §6.3, lot C5): what a
// preparation records before it touches the disk. Where a set of archives
// is made, which tool runs and how it is proven stopped, which source is
// moved where, and the complete list of what was prepared. Journals outlive
// the queue entry: a post whose sources are not back in place keeps its
// owner (_releaseQueuedPost).
//
//========================================================================

#include "history/PostHistoryStore.h"
#include "history/PostHistorySql.h"

#include <QHash>
#include <QSet>

#include <algorithm>

using historysql::nullString;
using historysql::run;

namespace
{

void fail(QString *error, const QString &reason)
{
    if (error)
        *error = reason;
}

//! A tool at a time per set; a write repeated after a lost acknowledgement
//! changes nothing.
bool toolStepAllowed(const QString &from, const QString &to)
{
    static const QHash<QString, QStringList> next{
        { QStringLiteral("idle"), { QStringLiteral("starting") } },
        { QStringLiteral("starting"),
          { QStringLiteral("running"),
            QStringLiteral("stopped_verified"),
            QStringLiteral("unproven") } },
        { QStringLiteral("running"),
          { QStringLiteral("quiescing"),
            QStringLiteral("stopped_verified"),
            QStringLiteral("unproven"),
            QStringLiteral("exited") } },
        { QStringLiteral("quiescing"),
          { QStringLiteral("stopped_verified"), QStringLiteral("unproven") } },
        { QStringLiteral("stopped_verified"),
          { QStringLiteral("starting"), QStringLiteral("idle") } },
        // Capability "unavailable" only: ended on its own, nothing proven.
        { QStringLiteral("exited"), { QStringLiteral("starting"), QStringLiteral("idle") } },
        // Only a later proof settles a stop that could not be proven.
        { QStringLiteral("unproven"), { QStringLiteral("stopped_verified") } },
    };
    return from == to || next.value(from).contains(to);
}

bool moveStepAllowed(const QString &from, const QString &to)
{
    static const QHash<QString, QStringList> next{
        { QStringLiteral("planned"),
          { QStringLiteral("moved"), QStringLiteral("restored"), QStringLiteral("blocked") } },
        { QStringLiteral("moved"), { QStringLiteral("restored"), QStringLiteral("blocked") } },
        { QStringLiteral("blocked"), { QStringLiteral("restored") } },
    };
    return from == to || next.value(from).contains(to);
}

//! A manifest is complete or refused: sequences 0..n-1, unique paths, every
//! file identified, at least one file to post.
QString manifestError(const QList<PostHistoryStore::ArtifactRecord> &artifacts)
{
    static const QStringList roles{ QStringLiteral("archive"),
                                    QStringLiteral("par2"),
                                    QStringLiteral("nfo"),
                                    QStringLiteral("staged_source") };
    if (artifacts.isEmpty())
        return QStringLiteral("manifest_empty");
    QSet<int> sequences;
    QSet<QString> paths;
    bool publishes = false;
    for (const PostHistoryStore::ArtifactRecord &a : artifacts) {
        if (a.sequence < 0 || a.sequence >= artifacts.size() || sequences.contains(a.sequence))
            return QStringLiteral("manifest_sequence");
        if (a.path.isEmpty() || paths.contains(a.path))
            return QStringLiteral("manifest_path");
        if (a.size < 0 || a.mtime.isEmpty() || a.identityJson.isEmpty())
            return QStringLiteral("manifest_identity");
        if (!roles.contains(a.role))
            return QStringLiteral("manifest_role");
        sequences.insert(a.sequence);
        paths.insert(a.path);
        publishes = publishes || a.publish;
    }
    return publishes ? QString() : QStringLiteral("manifest_publishes_nothing");
}

QList<PostHistoryStore::ArtifactRecord> readArtifacts(QSqlDatabase &db, const QString &setId)
{
    QList<PostHistoryStore::ArtifactRecord> artifacts;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT sequence, path, size, mtime, native_identity_json, role, "
                             "publish, groups FROM post_run_artifacts WHERE artifact_set_id=? "
                             "ORDER BY sequence"));
    q.addBindValue(setId);
    if (!q.exec())
        return artifacts;
    while (q.next()) {
        PostHistoryStore::ArtifactRecord a;
        a.sequence = q.value(0).toInt();
        a.path = q.value(1).toString();
        a.size = q.value(2).toLongLong();
        a.mtime = q.value(3).toString();
        a.identityJson = q.value(4).toString();
        a.role = q.value(5).toString();
        a.publish = q.value(6).toInt() != 0;
        a.groups = q.value(7).toString();
        artifacts << a;
    }
    return artifacts;
}

//! The supervision a set records, or @p given when it says one.
QString supervisionOf(QSqlDatabase &db, const QString &setId, const QString &given)
{
    if (!given.isEmpty())
        return given;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT supervision FROM post_artifact_sets WHERE artifact_set_id=?"));
    q.addBindValue(setId);
    return q.exec() && q.next() ? q.value(0).toString() : QString();
}

//! The recipe revision the entry of @p postId has now.
bool entryRevision(QSqlDatabase &db, qint64 postId, qint64 *revision, QString *error)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT recipe_revision FROM post_queue WHERE post_id=?"));
    q.addBindValue(postId);
    if (!q.exec() || !q.next()) {
        fail(error, QStringLiteral("stale_run: post %1 is not queued").arg(postId));
        return false;
    }
    *revision = q.value(0).toLongLong();
    return true;
}

} // namespace

bool PostHistoryStore::_preparationWrite(const std::function<bool(QSqlDatabase &, QString *)> &work,
                                         QString *error)
{
    // A store that speaks for no session writes no preparation fact: those
    // rows decide what may be renamed back or deleted.
    if (_ownerId.isEmpty()) {
        fail(error, QStringLiteral("not_owner: no session"));
        return false;
    }
    if (!initialize(error))
        return false;
    QSqlDatabase db = _database(error);
    if (!db.isOpen())
        return false;
    if (!db.transaction()) {
        fail(error, db.lastError().text());
        return false;
    }
    QString err;
    if (!work(db, &err) || !db.commit()) {
        if (err.isEmpty())
            err = db.lastError().text();
        db.rollback();
        fail(error, err);
        return false;
    }
    return true;
}

bool PostHistoryStore::_checkOwnSet(QSqlDatabase &db,
                                    qint64 postId,
                                    const QString &runId,
                                    const QString &setId,
                                    QString *state,
                                    QString *toolState,
                                    QString *error)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT post_id, created_by_run, state, tool_state FROM "
                             "post_artifact_sets WHERE artifact_set_id=?"));
    q.addBindValue(setId);
    if (!q.exec() || !q.next()) {
        fail(error, QStringLiteral("artifact_set_unknown: %1").arg(setId));
        return false;
    }
    if (q.value(0).toLongLong() != postId || q.value(1).toString() != runId) {
        fail(error, QStringLiteral("artifact_set_mismatch: %1").arg(setId));
        return false;
    }
    if (state)
        *state = q.value(2).toString();
    if (toolState)
        *toolState = q.value(3).toString();
    return true;
}

bool PostHistoryStore::createArtifactSet(const ArtifactSetPlan &plan, QString *error)
{
    if (plan.setId.isEmpty() || plan.generation <= 0 || plan.root.isEmpty()
        || plan.archiveDir.isEmpty()) {
        fail(error, QStringLiteral("artifact_set_plan"));
        return false;
    }
    return _preparationWrite([this, &plan](QSqlDatabase &db, QString *err) {
        qint64 revision = 0;
        if (!_checkQueuedRun(db, plan.postId, plan.runId, err)
            || !entryRevision(db, plan.postId, &revision, err))
            return false;
        // Prepared for the recipe the entry has now, or not at all.
        if (static_cast<qint64>(plan.recipeRevision) != revision) {
            fail(err,
                 QStringLiteral("recipe_revision: %1, entry at %2")
                     .arg(plan.recipeRevision)
                     .arg(revision));
            return false;
        }
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT post_id, created_by_run, generation, state, root, "
                                 "archive_dir, staging_dir, recipe_revision FROM "
                                 "post_artifact_sets WHERE artifact_set_id=?"));
        q.addBindValue(plan.setId);
        if (!q.exec()) {
            fail(err, q.lastError().text());
            return false;
        }
        if (q.next()) {
            // Opened by the queue when the preparation began: its places complete it.
            const bool same = q.value(0).toLongLong() == plan.postId
                && q.value(1).toString() == plan.runId && q.value(2).toInt() == plan.generation
                && q.value(3).toString() == QStringLiteral("preparing")
                && q.value(4).toString() == plan.root
                && q.value(7).toLongLong() == static_cast<qint64>(plan.recipeRevision);
            const bool placesKept = (q.value(5).isNull()
                                     || q.value(5).toString() == plan.archiveDir)
                && (q.value(6).isNull() || q.value(6).toString() == plan.stagingDir);
            if (!same || !placesKept) {
                fail(err, QStringLiteral("artifact_set_mismatch: %1").arg(plan.setId));
                return false;
            }
            return run(db,
                       QStringLiteral("UPDATE post_artifact_sets SET archive_dir=?, staging_dir=? "
                                      "WHERE artifact_set_id=?"),
                       { plan.archiveDir,
                         plan.stagingDir.isEmpty() ? nullString() : QVariant(plan.stagingDir),
                         plan.setId },
                       err);
        }
        QSqlQuery last(db);
        last.prepare(QStringLiteral(
            "SELECT COALESCE(MAX(generation), 0) FROM post_artifact_sets WHERE post_id=?"));
        last.addBindValue(plan.postId);
        if (!last.exec() || !last.next() || last.value(0).toInt() >= plan.generation) {
            fail(err, QStringLiteral("artifact_set_generation: %1").arg(plan.generation));
            return false;
        }
        return run(db,
                   QStringLiteral("INSERT INTO post_artifact_sets(artifact_set_id, post_id, "
                                  "created_by_run, generation, recipe_revision, state, retained, "
                                  "root, archive_dir, staging_dir, bytes, created_at) VALUES(?, ?, "
                                  "?, ?, ?, 'preparing', 0, ?, ?, ?, 0, ?)"),
                   { plan.setId,
                     plan.postId,
                     plan.runId,
                     plan.generation,
                     static_cast<qint64>(plan.recipeRevision),
                     plan.root,
                     plan.archiveDir,
                     plan.stagingDir.isEmpty() ? nullString() : QVariant(plan.stagingDir),
                     _now() },
                   err);
    }, error);
}

bool PostHistoryStore::recordArtifactMarker(qint64 postId,
                                            const QString &runId,
                                            const QString &setId,
                                            const QString &markerIdentityJson,
                                            QString *error)
{
    return _preparationWrite([&](QSqlDatabase &db, QString *err) {
        QString state;
        if (!_checkQueuedRun(db, postId, runId, err)
            || !_checkOwnSet(db, postId, runId, setId, &state, nullptr, err))
            return false;
        if (state != QStringLiteral("preparing") || markerIdentityJson.isEmpty()) {
            fail(err, QStringLiteral("artifact_set_state: %1").arg(state));
            return false;
        }
        int rows = 0;
        if (!run(db,
                 QStringLiteral("UPDATE post_artifact_sets SET marker_identity_json=? WHERE "
                                "artifact_set_id=? AND (marker_identity_json IS NULL OR "
                                "marker_identity_json=?)"),
                 { markerIdentityJson, setId, markerIdentityJson },
                 err,
                 &rows))
            return false;
        if (rows != 1) // another marker: the folder is no longer the one recorded
            fail(err, QStringLiteral("artifact_marker_changed: %1").arg(setId));
        return rows == 1;
    }, error);
}

bool PostHistoryStore::recordToolSupervision(qint64 postId,
                                             const QString &runId,
                                             const QString &setId,
                                             const QString &capability,
                                             const QString &toolState,
                                             const QString &toolIdentityJson,
                                             QString *error)
{
    return _preparationWrite([&](QSqlDatabase &db, QString *err) {
        QString state;
        QString current;
        if (!_checkQueuedRun(db, postId, runId, err)
            || !_checkOwnSet(db, postId, runId, setId, &state, &current, err))
            return false;
        if (state != QStringLiteral("preparing") || !toolStepAllowed(current, toolState)) {
            fail(err, QStringLiteral("tool_state: %1 -> %2").arg(current, toolState));
            return false;
        }
        // Nothing starts before how it is stopped is known.
        if (toolState == QStringLiteral("starting") && capability.isEmpty()) {
            QSqlQuery known(db);
            known.prepare(QStringLiteral(
                "SELECT supervision FROM post_artifact_sets WHERE artifact_set_id=?"));
            known.addBindValue(setId);
            if (!known.exec() || !known.next() || known.value(0).toString().isEmpty()) {
                fail(err, QStringLiteral("supervision_missing"));
                return false;
            }
        }
        // "exited" says nothing of descendants: only where nothing can be proven.
        if (toolState == QStringLiteral("exited")
            && supervisionOf(db, setId, capability) != QStringLiteral("unavailable")) {
            fail(err, QStringLiteral("exited_needs_unavailable"));
            return false;
        }
        return run(db,
                   QStringLiteral("UPDATE post_artifact_sets SET supervision=COALESCE(?, "
                                  "supervision), tool_state=?, tool_identity_json=? WHERE "
                                  "artifact_set_id=?"),
                   { capability.isEmpty() ? nullString() : QVariant(capability),
                     toolState,
                     toolIdentityJson.isEmpty() ? nullString() : QVariant(toolIdentityJson),
                     setId },
                   err);
    }, error);
}

bool PostHistoryStore::planSourceMoves(qint64 postId,
                                       const QString &runId,
                                       const QString &setId,
                                       const QList<SourceMove> &moves,
                                       QString *error)
{
    return _preparationWrite([&](QSqlDatabase &db, QString *err) {
        if (!_checkQueuedRun(db, postId, runId, err)
            || (!setId.isEmpty() && !_checkOwnSet(db, postId, runId, setId, nullptr, nullptr, err)))
            return false;
        for (const SourceMove &move : moves) {
            if (move.sequence < 0 || move.sourcePath.isEmpty() || move.stagedPath.isEmpty()
                || move.identityJson.isEmpty()) {
                fail(err, QStringLiteral("source_move_incomplete: %1").arg(move.sequence));
                return false;
            }
            QSqlQuery q(db);
            q.prepare(QStringLiteral("SELECT source_path, staged_path, native_identity_json, "
                                     "COALESCE(artifact_set_id, '') FROM post_source_moves "
                                     "WHERE run_id=? AND sequence=?"));
            q.addBindValue(runId);
            q.addBindValue(move.sequence);
            if (!q.exec()) {
                fail(err, q.lastError().text());
                return false;
            }
            if (q.next()) {
                // Planned again after a lost acknowledgement: the same intent only.
                if (q.value(0).toString() != move.sourcePath
                    || q.value(1).toString() != move.stagedPath
                    || q.value(2).toString() != move.identityJson
                    || q.value(3).toString() != setId) {
                    fail(err, QStringLiteral("source_move_conflict: %1").arg(move.sequence));
                    return false;
                }
                continue;
            }
            if (!run(db,
                     QStringLiteral("INSERT INTO post_source_moves(post_id, run_id, "
                                    "artifact_set_id, sequence, source_path, staged_path, "
                                    "native_identity_json, state) VALUES(?, ?, ?, ?, ?, ?, ?, "
                                    "'planned')"),
                     { postId,
                       runId,
                       setId.isEmpty() ? nullString() : QVariant(setId),
                       move.sequence,
                       move.sourcePath,
                       move.stagedPath,
                       move.identityJson },
                     err))
                return false;
        }
        return true;
    }, error);
}

bool PostHistoryStore::resolveSourceMoves(qint64 postId,
                                          const QString &runId,
                                          const QList<int> &sequences,
                                          const QString &state,
                                          QString *error)
{
    return _preparationWrite([&](QSqlDatabase &db, QString *err) {
        // The entry may be gone; the post stays this session's until its
        // sources are back.
        QSqlQuery owner(db);
        owner.prepare(QStringLiteral("SELECT owner_id FROM posts WHERE id=?"));
        owner.addBindValue(postId);
        if (!owner.exec() || !owner.next() || owner.value(0).toString() != _ownerId) {
            fail(err, QStringLiteral("not_owner: post %1").arg(postId));
            return false;
        }
        for (const int sequence : sequences) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("SELECT state FROM post_source_moves WHERE post_id=? AND "
                                     "run_id=? AND sequence=?"));
            q.addBindValue(postId);
            q.addBindValue(runId);
            q.addBindValue(sequence);
            if (!q.exec() || !q.next()) {
                fail(err, QStringLiteral("source_move_unknown: %1").arg(sequence));
                return false;
            }
            const QString from = q.value(0).toString();
            if (!moveStepAllowed(from, state)) {
                fail(err, QStringLiteral("source_move_state: %1 -> %2").arg(from, state));
                return false;
            }
            if (!run(db,
                     QStringLiteral(
                         "UPDATE post_source_moves SET state=? WHERE run_id=? AND sequence=?"),
                     { state, runId, sequence },
                     err))
                return false;
        }
        return true;
    }, error);
}

QList<PostHistoryStore::SourceMove> PostHistoryStore::unsettledSourceMoves(qint64 postId,
                                                                           QString *error)
{
    QList<SourceMove> moves;
    if (!initialize(error))
        return moves;
    QSqlDatabase db = _database(error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT m.sequence, m.source_path, m.staged_path, "
                             "m.native_identity_json, m.state, m.run_id, m.artifact_set_id FROM "
                             "post_source_moves m JOIN post_runs r ON r.run_id = m.run_id WHERE "
                             "m.post_id=? AND m.state IN ('planned','moved','blocked') ORDER BY "
                             "r.run_no, m.sequence"));
    q.addBindValue(postId);
    if (!q.exec()) {
        fail(error, q.lastError().text());
        return moves;
    }
    while (q.next()) {
        SourceMove move;
        move.sequence = q.value(0).toInt();
        move.sourcePath = q.value(1).toString();
        move.stagedPath = q.value(2).toString();
        move.identityJson = q.value(3).toString();
        move.state = q.value(4).toString();
        move.runId = q.value(5).toString();
        move.setId = q.value(6).toString();
        moves << move;
    }
    return moves;
}

bool PostHistoryStore::recordPreparedArtifacts(qint64 postId,
                                               const QString &runId,
                                               const QString &setId,
                                               const QList<ArtifactRecord> &artifacts,
                                               QString *error)
{
    const QString incomplete = manifestError(artifacts);
    if (!incomplete.isEmpty()) {
        fail(error, incomplete);
        return false;
    }
    return _preparationWrite([&](QSqlDatabase &db, QString *err) {
        QString state;
        QString tool;
        qint64 revision = 0;
        if (!_checkQueuedRun(db, postId, runId, err)
            || !_checkOwnSet(db, postId, runId, setId, &state, &tool, err)
            || !entryRevision(db, postId, &revision, err))
            return false;
        QSqlQuery made(db);
        made.prepare(QStringLiteral(
            "SELECT recipe_revision FROM post_artifact_sets WHERE artifact_set_id=?"));
        made.addBindValue(setId);
        if (!made.exec() || !made.next() || made.value(0).toLongLong() != revision) {
            fail(err, QStringLiteral("recipe_revision: set %1 is stale").arg(setId));
            return false;
        }
        QList<ArtifactRecord> sorted = artifacts;
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
            return a.sequence < b.sequence;
        });
        if (state == QStringLiteral("verified")) {
            // Recorded already, the acknowledgement was lost: the same list only.
            if (readArtifacts(db, setId) == sorted)
                return true;
            fail(err, QStringLiteral("artifact_set_verified: %1").arg(setId));
            return false;
        }
        if (state != QStringLiteral("preparing")) {
            fail(err, QStringLiteral("artifact_set_state: %1").arg(state));
            return false;
        }
        // A proven stop, or (capability "unavailable") a tool that ended on
        // its own: its outputs may be used, nothing more is concluded.
        const bool exited = tool == QStringLiteral("exited")
            && supervisionOf(db, setId, {}) == QStringLiteral("unavailable");
        if (tool != QStringLiteral("idle") && tool != QStringLiteral("stopped_verified")
            && !exited) {
            fail(err, QStringLiteral("tool_not_stopped: %1").arg(tool));
            return false;
        }
        QSqlQuery pending(db);
        pending.prepare(QStringLiteral("SELECT 1 FROM post_source_moves WHERE run_id=? AND "
                                       "state='planned' LIMIT 1"));
        pending.addBindValue(runId);
        if (!pending.exec() || pending.next()) {
            fail(err, QStringLiteral("source_move_pending: %1").arg(runId));
            return false;
        }
        qint64 bytes = 0;
        for (const ArtifactRecord &a : sorted) {
            bytes += a.size;
            if (!run(db,
                     QStringLiteral("INSERT INTO post_run_artifacts(artifact_set_id, sequence, "
                                    "path, size, mtime, native_identity_json, role, publish, "
                                    "groups) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)"),
                     { setId,
                       a.sequence,
                       a.path,
                       a.size,
                       a.mtime,
                       a.identityJson,
                       a.role,
                       a.publish ? 1 : 0,
                       a.groups.isEmpty() ? nullString() : QVariant(a.groups) },
                     err))
                return false;
        }
        return run(db,
                   QStringLiteral("UPDATE post_artifact_sets SET state='verified', bytes=? WHERE "
                                  "artifact_set_id=?"),
                   { bytes, setId },
                   err)
            && run(db,
                   QStringLiteral(
                       "UPDATE post_queue SET current_artifact_set_id=? WHERE post_id=?"),
                   { setId, postId },
                   err);
    }, error);
}

std::optional<PostHistoryStore::ArtifactSetRecord> PostHistoryStore::artifactSet(
    const QString &setId,
    QString *error)
{
    if (!initialize(error))
        return std::nullopt;
    QSqlDatabase db = _database(error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT post_id, created_by_run, generation, recipe_revision, root, "
                             "archive_dir, staging_dir, state, retained, marker_identity_json, "
                             "supervision, tool_state, tool_identity_json, bytes FROM "
                             "post_artifact_sets WHERE artifact_set_id=?"));
    q.addBindValue(setId);
    if (!q.exec()) {
        fail(error, q.lastError().text());
        return std::nullopt;
    }
    if (!q.next())
        return std::nullopt;
    ArtifactSetRecord set;
    set.plan.setId = setId;
    set.plan.postId = q.value(0).toLongLong();
    set.plan.runId = q.value(1).toString();
    set.plan.generation = q.value(2).toInt();
    set.plan.recipeRevision = static_cast<quint64>(q.value(3).toLongLong());
    set.plan.root = q.value(4).toString();
    set.plan.archiveDir = q.value(5).toString();
    set.plan.stagingDir = q.value(6).toString();
    set.state = q.value(7).toString();
    set.retained = q.value(8).toInt() != 0;
    set.markerIdentityJson = q.value(9).toString();
    set.supervision = q.value(10).toString();
    set.toolState = q.value(11).toString();
    set.toolIdentityJson = q.value(12).toString();
    set.bytes = q.value(13).toLongLong();
    set.artifacts = readArtifacts(db, setId);
    return set;
}
