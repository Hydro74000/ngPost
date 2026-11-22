// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Queue manager tables of the history database (schema v6, plan_queue.md
// §5.1, §5.5). A queue::QueueTransaction is applied in one SQL transaction:
// an admission creates the posts row (status 'queued'), its first run and
// its queue entry together, so a post is never half admitted.
//
// Tables filled by later lots are created now (post_source_moves,
// post_path_refs, post_artifact_sets, post_run_artifacts) so that the v6
// migration is the only one the queue manager needs.
//
//========================================================================

#include "history/PostHistoryStore.h"

#include "PostingJobOptionsCodec.h"
#include "queue/QueueStore.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

using namespace queue;

namespace
{

//! Runs, queue orders, artifact sets, queue entries and the "post now" intent.
QStringList v6QueueStatements()
{
    return {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS post_runs ("
            "run_id TEXT PRIMARY KEY NOT NULL,"
            "post_id INTEGER NOT NULL REFERENCES posts(id) ON DELETE CASCADE,"
            "run_no INTEGER NOT NULL CHECK(run_no > 0),"
            "created_at TEXT NOT NULL, started_at TEXT, finished_at TEXT,"
            "state TEXT NOT NULL CHECK(state IN ('queued','preparing','posting','settling',"
            "'interrupted','terminal')),"
            "finalization_state TEXT NOT NULL DEFAULT 'none' CHECK(finalization_state IN "
            "('none','nzb_pending','complete','abandoned')),"
            "result TEXT CHECK(result IS NULL OR result IN ('success','partial','failed',"
            "'cancelled','interrupted')),"
            "result_recorded INTEGER NOT NULL DEFAULT 0 CHECK(result_recorded IN (0,1)),"
            "end_cause TEXT CHECK(end_cause IS NULL OR end_cause IN ('normal','user_stop','keep',"
            "'technical','abandon')),"
            "active_ms INTEGER NOT NULL DEFAULT 0 CHECK(active_ms >= 0),"
            "packing_root TEXT, archive_dir TEXT, staging_dir TEXT, scratch_nzb_path TEXT,"
            "artifact_identity_json TEXT, supervision_json TEXT,"
            "UNIQUE(post_id, run_no), UNIQUE(post_id, run_id))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_runs_post ON post_runs(post_id, run_no)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS post_queue_orders ("
                       "queue_id TEXT NOT NULL,"
                       "restore_state TEXT NOT NULL CHECK(restore_state IN ('session','deferred')),"
                       "order_revision INTEGER NOT NULL DEFAULT 0 CHECK(order_revision >= 0),"
                       "PRIMARY KEY(queue_id, restore_state))"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS post_artifact_sets ("
            "artifact_set_id TEXT PRIMARY KEY NOT NULL,"
            "post_id INTEGER NOT NULL REFERENCES posts(id) ON DELETE RESTRICT,"
            "created_by_run TEXT NOT NULL,"
            "generation INTEGER NOT NULL CHECK(generation > 0),"
            "recipe_revision INTEGER NOT NULL CHECK(recipe_revision >= 0),"
            "state TEXT NOT NULL CHECK(state IN "
            "('preparing','verified','invalidated','discarded')),"
            "retained INTEGER NOT NULL DEFAULT 0 CHECK(retained IN (0,1)),"
            "root TEXT NOT NULL, archive_dir TEXT, staging_dir TEXT, marker_identity_json TEXT,"
            "bytes INTEGER NOT NULL DEFAULT 0 CHECK(bytes >= 0),"
            "created_at TEXT NOT NULL,"
            "UNIQUE(post_id, generation), UNIQUE(post_id, artifact_set_id),"
            "FOREIGN KEY(post_id, created_by_run) REFERENCES post_runs(post_id, run_id) "
            "ON DELETE RESTRICT)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_artifact_sets_budget ON "
                       "post_artifact_sets(root, state)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS post_queue ("
            "post_id INTEGER PRIMARY KEY REFERENCES posts(id) ON DELETE CASCADE,"
            "queue_id TEXT NOT NULL,"
            "restore_state TEXT NOT NULL DEFAULT 'session' CHECK(restore_state IN "
            "('session','deferred')),"
            "run_id TEXT NOT NULL,"
            "position INTEGER NOT NULL CHECK(position >= 0),"
            "recipe_revision INTEGER NOT NULL DEFAULT 0 CHECK(recipe_revision >= 0),"
            "origin TEXT NOT NULL CHECK(origin IN ('quick','auto','cli','resume','drop')),"
            "tab_number INTEGER CHECK(tab_number IS NULL OR tab_number > 0),"
            "held INTEGER NOT NULL DEFAULT 0 CHECK(held IN (0,1)),"
            "blocked_reason TEXT, blocked_params_json TEXT,"
            "stage TEXT NOT NULL CHECK(stage IN ('queued','preparing','prepared','posting',"
            "'quiescing','finalizing','recovering')),"
            "current_artifact_set_id TEXT,"
            "source_bytes INTEGER NOT NULL DEFAULT 0 CHECK(source_bytes >= 0),"
            "restore_failures INTEGER NOT NULL DEFAULT 0 CHECK(restore_failures >= 0),"
            "options_json TEXT NOT NULL, enqueued_at TEXT NOT NULL, updated_at TEXT NOT NULL,"
            "FOREIGN KEY(queue_id, restore_state) REFERENCES post_queue_orders(queue_id, "
            "restore_state),"
            "FOREIGN KEY(post_id, run_id) REFERENCES post_runs(post_id, run_id),"
            "FOREIGN KEY(post_id, current_artifact_set_id) "
            "REFERENCES post_artifact_sets(post_id, artifact_set_id))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS post_queue_position "
                       "ON post_queue(queue_id, restore_state, position, post_id)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS post_queue_intents ("
            "intent_id TEXT PRIMARY KEY NOT NULL,"
            "queue_id TEXT NOT NULL UNIQUE,"
            "target_post_id INTEGER NOT NULL REFERENCES post_queue(post_id) ON DELETE CASCADE,"
            "target_recipe_revision INTEGER NOT NULL CHECK(target_recipe_revision >= 0),"
            "active_run_id TEXT NOT NULL REFERENCES post_runs(run_id),"
            "state TEXT NOT NULL CHECK(state IN ('pending','switching')),"
            "created_at TEXT NOT NULL)"),
    };
}

//! Artifact contents, staged source moves and path references.
QStringList v6ArtifactStatements()
{
    return {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS post_run_artifacts ("
            "artifact_set_id TEXT NOT NULL REFERENCES post_artifact_sets(artifact_set_id) "
            "ON DELETE CASCADE,"
            "sequence INTEGER NOT NULL CHECK(sequence >= 0),"
            "path TEXT NOT NULL, size INTEGER NOT NULL CHECK(size >= 0), mtime TEXT NOT NULL,"
            "native_identity_json TEXT NOT NULL,"
            "role TEXT NOT NULL CHECK(role IN ('archive','par2','nfo','staged_source')),"
            "publish INTEGER NOT NULL CHECK(publish IN (0,1)), groups TEXT,"
            "PRIMARY KEY(artifact_set_id, sequence))"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS post_source_moves ("
            "post_id INTEGER NOT NULL REFERENCES posts(id) ON DELETE RESTRICT,"
            "run_id TEXT NOT NULL, artifact_set_id TEXT,"
            "sequence INTEGER NOT NULL CHECK(sequence >= 0),"
            "source_path TEXT NOT NULL, staged_path TEXT NOT NULL,"
            "native_identity_json TEXT NOT NULL,"
            "state TEXT NOT NULL CHECK(state IN ('planned','moved','restored','blocked')),"
            "PRIMARY KEY(run_id, sequence),"
            "FOREIGN KEY(post_id, run_id) REFERENCES post_runs(post_id, run_id) ON DELETE RESTRICT,"
            "FOREIGN KEY(post_id, artifact_set_id) REFERENCES post_artifact_sets(post_id, "
            "artifact_set_id) "
            "ON DELETE RESTRICT)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_source_moves_recovery ON "
                       "post_source_moves(post_id, state)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS post_path_refs ("
                       "id INTEGER PRIMARY KEY,"
                       "post_id INTEGER NOT NULL REFERENCES posts(id) ON DELETE CASCADE,"
                       "run_id TEXT,"
                       "role TEXT NOT NULL CHECK(role IN ('source','nzb','workspace','metadata')),"
                       "path_key TEXT NOT NULL,"
                       "scope TEXT NOT NULL CHECK(scope IN ('exact','tree')),"
                       "access TEXT NOT NULL CHECK(access IN ('read','destructive','write')),"
                       "native_identity_json TEXT, released_at TEXT,"
                       "FOREIGN KEY(post_id, run_id) REFERENCES post_runs(post_id, run_id) ON "
                       "DELETE CASCADE)"),
        QStringLiteral(
            "CREATE UNIQUE INDEX IF NOT EXISTS post_path_refs_live_nzb ON post_path_refs(path_key) "
            "WHERE role='nzb' AND released_at IS NULL"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS post_path_refs_source ON post_path_refs(role, "
                       "path_key, released_at)"),
    };
}

const QStringList &v6Statements()
{
    // Schema retained with Codex (revue_queue/echanges/06_codex_arbitrage_c2c3.md).
    static const QStringList statements = v6QueueStatements() + v6ArtifactStatements();
    return statements;
}

bool hasColumn(QSqlDatabase &db, const QString &table, const QString &column, QString *error)
{
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    while (q.next())
        if (q.value(1).toString() == column)
            return true;
    return false;
}

QString restoreState(Domain domain)
{
    return domain == Domain::Deferred ? QStringLiteral("deferred") : QStringLiteral("session");
}

//! post_runs.state for @p entry. A preparation being stopped transferred
//! nothing: it is still a preparation, cancelled if it leaves.
QString runState(const QueueEntry &entry)
{
    switch (entry.lifecycle) {
    case Lifecycle::Queued: return QStringLiteral("queued");
    case Lifecycle::Preparing:
    case Lifecycle::Prepared: return QStringLiteral("preparing");
    case Lifecycle::Posting: return QStringLiteral("posting");
    case Lifecycle::Quiescing:
        return entry.slot == Slot::Preparation ? QStringLiteral("preparing")
                                               : QStringLiteral("settling");
    case Lifecycle::Finalizing:
    case Lifecycle::Recovering: return QStringLiteral("settling");
    }
    return QStringLiteral("queued");
}

QVariant nullString()
{
    return QVariant(QMetaType::fromType<QString>());
}

QString blockedParams(const BlockReason &reason)
{
    if (!reason.isBlocked() || reason.params.isEmpty())
        return {};
    return QString::fromUtf8(
        QJsonDocument(QJsonObject::fromVariantMap(reason.params)).toJson(QJsonDocument::Compact));
}

//! Runs one statement with positional values; false sets @p error.
bool run(QSqlDatabase &db,
         const QString &sql,
         const QVariantList &values,
         QString *error,
         int *rows = nullptr)
{
    QSqlQuery q(db);
    q.prepare(sql);
    for (const QVariant &value : values)
        q.addBindValue(value);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    if (rows)
        *rows = q.numRowsAffected();
    return true;
}

//! The parent row an entry's (queue, domain) refers to.
bool ensureOrderRow(QSqlDatabase &db, const QString &queueId, Domain domain, QString *error)
{
    return run(db,
               QStringLiteral("INSERT OR IGNORE INTO post_queue_orders(queue_id, restore_state, "
                              "order_revision) VALUES(?, ?, 0)"),
               { queueId, restoreState(domain) },
               error);
}

QString artifactState(const QueueEntry &entry)
{
    return entry.lifecycle == Lifecycle::Preparing ? QStringLiteral("preparing")
                                                   : QStringLiteral("verified");
}

} // namespace

// --- schema --------------------------------------------------------------------------------

bool PostHistoryStore::_migrateToV6(QSqlDatabase &db, QString *error)
{
    for (const QString &sql : v6Statements())
        if (!run(db, sql, {}, error))
            return false;
    struct Column
    {
        const char *table;
        const char *name;
        const char *ddl;
    };
    static const Column columns[] = {
        { "posts", "owner_id", "ALTER TABLE posts ADD COLUMN owner_id TEXT" },
        { "post_info", "current_run_id", "ALTER TABLE post_info ADD COLUMN current_run_id TEXT" },
        { "post_info",
          "transfer_manifest_complete",
          "ALTER TABLE post_info ADD COLUMN transfer_manifest_complete INTEGER NOT NULL DEFAULT 0 "
          "CHECK(transfer_manifest_complete IN (0, 1))" },
    };
    for (const Column &c : columns) {
        QString columnError;
        const bool present = hasColumn(db,
                                       QLatin1String(c.table),
                                       QLatin1String(c.name),
                                       &columnError);
        if (!columnError.isEmpty()) {
            if (error)
                *error = columnError;
            return false;
        }
        if (!present && !run(db, QLatin1String(c.ddl), {}, error))
            return false;
    }
    return run(db,
               QStringLiteral("CREATE INDEX IF NOT EXISTS idx_posts_owner ON posts(owner_id)"),
               {},
               error);
}

bool PostHistoryStore::_refuseFutureSchema(QSqlDatabase &db, QString *error)
{
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT value FROM schema_meta WHERE key='version'")))
        return true; // no schema_meta yet: a new or very old database
    if (!q.next() || q.value(0).toInt() <= kSchemaVersion)
        return true;
    if (error)
        *error = QStringLiteral(
                     "this history database was written by a newer ngPost (schema v%1, this "
                     "version understands v%2); update ngPost rather than risking your history")
                     .arg(q.value(0).toInt())
                     .arg(kSchemaVersion);
    return false;
}

// --- transactions --------------------------------------------------------------------------

QueueCommit PostHistoryStore::commitQueueTransaction(const QueueTransaction &transaction)
{
    QueueCommit result;
    if (!initialize(&result.error))
        return result;
    QSqlDatabase db = _database(&result.error);
    if (!db.isOpen())
        return result;
    if (!db.transaction()) {
        result.error = db.lastError().text();
        return result;
    }
    if (!_applyQueueTransaction(db, transaction, result) || !db.commit()) {
        if (result.error.isEmpty())
            result.error = db.lastError().text();
        db.rollback();
        result.assignedIds.clear();
        result.runs.clear();
    }
    return result;
}

bool PostHistoryStore::_applyQueueTransaction(QSqlDatabase &db,
                                              const QueueTransaction &transaction,
                                              QueueCommit &result)
{
    for (QueueEntry entry : transaction.upserts) {
        if (entry.postId <= 0) {
            const qint64 placeholder = entry.postId;
            if (!_admitQueueEntry(db, entry, &result.error))
                return false;
            result.assignedIds.insert(placeholder, entry.postId);
            result.runs.insert(entry.postId, { entry.runId, entry.runNo });
        } else if (!_writeQueueEntry(db, entry, false, &result.error)) {
            return false;
        }
    }
    for (QueueEntry entry : transaction.existingAdmissions) {
        if (!_admitExistingQueueEntry(db, entry, &result.error))
            return false;
        result.runs.insert(entry.postId, { entry.runId, entry.runNo });
    }
    for (qint64 postId : transaction.renewRuns) {
        QueueCommit::Run renewed;
        if (!_renewQueueRun(db, postId, &renewed.runId, &renewed.runNo, &result.error))
            return false;
        result.runs.insert(postId, renewed);
    }
    for (qint64 postId : transaction.removals) {
        const auto outcome = transaction.outcomes.constFind(postId);
        if (outcome != transaction.outcomes.cend()
            && !_recordRunOutcome(db, postId, *outcome, &result.error))
            return false;
        if (!_retireQueueEntry(db, postId, &result.error))
            return false;
    }
    for (const QueueTransaction::Order &order : transaction.orders) {
        QList<qint64> ids;
        for (qint64 id : order.ids)
            ids << result.assignedIds.value(id, id);
        if (!_writeQueueOrder(db, order.queueId, order.domain, ids, order.revision, &result.error))
            return false;
    }
    return _writeQueueIntent(db, transaction, &result.error);
}

bool PostHistoryStore::_admitQueueEntry(QSqlDatabase &db, QueueEntry &entry, QString *error)
{
    const PostingJobOptions &job = entry.options.job;
    const QString rarPass = job.rarPass.isEmpty() ? job.declaredPassword : job.rarPass;
    const bool stored = _storePasswords && !rarPass.isEmpty();
    if (!run(db,
             QStringLiteral(
                 "INSERT INTO posts(created_at, nzb_name, nzb_path, status, rar_name, rar_pass,"
                 "has_password, password_stored, password_origin, from_addr, do_compress, do_par2,"
                 "obfuscate_articles, obfuscate_file_name, resume_state, owner_id) "
                 "VALUES(?, ?, ?, 'queued', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, ?)"),
             { _now(),
               QFileInfo(job.nzbFilePath).completeBaseName(),
               job.nzbFilePath,
               job.rarName,
               stored ? rarPass : QString(),
               rarPass.isEmpty() ? 0 : 1,
               stored ? 1 : 0,
               passwordOriginName(entry.options.passwordOrigin),
               QString::fromStdString(job.from),
               job.doCompress ? 1 : 0,
               job.doPar2 ? 1 : 0,
               job.obfuscateArticles ? 1 : 0,
               job.obfuscateFileName ? 1 : 0,
               _ownerId.isEmpty() ? nullString() : QVariant(_ownerId) },
             error))
        return false;
    QSqlQuery last(db);
    if (!last.exec(QStringLiteral("SELECT last_insert_rowid()")) || !last.next()) {
        _setQueryError(error, last);
        return false;
    }
    entry.postId = last.value(0).toLongLong();
    for (const QString &group : job.grpList)
        if (!run(db,
                 QStringLiteral("INSERT INTO post_groups(post_id, group_name) VALUES(?, ?)"),
                 { entry.postId, group },
                 error))
            return false;
    PostInfo info;
    info.par2Pct = job.describedPar2Pct();
    info.articleSizeBytes = job.articleSizeBytes;
    info.sourcePath = job.inputPaths.isEmpty() ? QString() : job.inputPaths.first();
    info.originalName = info.sourcePath.isEmpty() ? QString()
                                                  : QFileInfo(info.sourcePath).fileName();
    info.appVersion = QStringLiteral(APP_VERSION);
    if (!_writePostInfo(db, entry.postId, info, error)
        || !_writePostMeta(db, entry.postId, job.meta, error))
        return false;
    entry.runNo = 1;
    entry.runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!run(db,
             QStringLiteral("INSERT INTO post_runs(run_id, post_id, run_no, created_at, state) "
                            "VALUES(?, ?, 1, ?, 'queued')"),
             { entry.runId, entry.postId, _now() },
             error)
        || !run(db,
                QStringLiteral("UPDATE post_info SET current_run_id=? WHERE post_id=?"),
                { entry.runId, entry.postId },
                error))
        return false;
    return _writeQueueEntry(db, entry, true, error);
}

//! A post of the history queued again: its next run, its owner, its queue row.
//! Its status and its earlier results stay as they are until it transfers.
bool PostHistoryStore::_admitExistingQueueEntry(QSqlDatabase &db, QueueEntry &entry, QString *error)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT (SELECT COALESCE(MAX(run_no), 0) + 1 FROM post_runs WHERE "
                             "post_id=p.id) FROM posts p WHERE p.id=? AND (p.owner_id IS NULL "
                             "OR p.owner_id=?)"));
    q.addBindValue(entry.postId);
    q.addBindValue(_ownerId);
    if (!q.exec() || !q.next()) {
        if (error)
            *error = q.lastError().text().isEmpty()
                ? QStringLiteral("post %1 is unknown or owned by another session").arg(entry.postId)
                : q.lastError().text();
        return false;
    }
    entry.runNo = q.value(0).toInt();
    entry.runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return run(db,
               QStringLiteral("INSERT INTO post_runs(run_id, post_id, run_no, created_at, state) "
                              "VALUES(?, ?, ?, ?, 'queued')"),
               { entry.runId, entry.postId, entry.runNo, _now() },
               error)
        && run(db,
               QStringLiteral("INSERT OR IGNORE INTO post_info(post_id) VALUES(?)"),
               { entry.postId },
               error)
        && run(db,
               QStringLiteral("UPDATE post_info SET current_run_id=? WHERE post_id=?"),
               { entry.runId, entry.postId },
               error)
        && run(db,
               QStringLiteral("UPDATE posts SET owner_id=? WHERE id=?"),
               { _ownerId.isEmpty() ? nullString() : QVariant(_ownerId), entry.postId },
               error)
        && _writeQueueEntry(db, entry, true, error);
}

//! Current and retained sets are recorded; a set the entry let go of is
//! invalidated (its files are cleaned up by the recovery lot, never here).
bool PostHistoryStore::_writeArtifactSets(QSqlDatabase &db, const QueueEntry &entry, QString *error)
{
    QList<std::pair<ArtifactSetRef, QString>> sets;
    if (!entry.artifacts.isNull())
        sets.append({ entry.artifacts, artifactState(entry) });
    for (const ArtifactSetRef &kept : entry.retainedSets)
        sets.append({ kept, QStringLiteral("invalidated") });
    QStringList keep;
    for (const auto &[set, state] : sets) {
        keep << set.id;
        const bool retained = state == QStringLiteral("invalidated");
        if (!run(db,
                 QStringLiteral(
                     "INSERT INTO post_artifact_sets(artifact_set_id, post_id, created_by_run, "
                     "generation,"
                     "recipe_revision, state, retained, root, bytes, created_at) "
                     "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT(artifact_set_id) DO UPDATE "
                     "SET "
                     "state=excluded.state, retained=excluded.retained, bytes=excluded.bytes"),
                 { set.id,
                   entry.postId,
                   entry.runId,
                   set.generation,
                   static_cast<qint64>(set.recipeRevision),
                   state,
                   retained ? 1 : 0,
                   set.root,
                   set.bytes,
                   _now() },
                 error))
            return false;
    }
    QStringList marks;
    QVariantList values{ entry.postId };
    for (const QString &id : keep) {
        marks << QStringLiteral("?");
        values << id;
    }
    const QString others = keep.isEmpty()
        ? QString()
        : QStringLiteral(" AND artifact_set_id NOT IN (%1)").arg(marks.join(QLatin1Char(',')));
    return run(db,
               QStringLiteral(
                   "UPDATE post_artifact_sets SET state='invalidated', retained=0 WHERE post_id=? "
                   "AND state IN ('preparing','verified')%1")
                   .arg(others),
               values,
               error);
}

bool PostHistoryStore::_writeQueueEntry(QSqlDatabase &db,
                                        const QueueEntry &entry,
                                        bool insert,
                                        QString *error)
{
    const auto encoded = optionscodec::encode(entry.options);
    if (!encoded.ok()) {
        if (error)
            *error = QStringLiteral("queue options: %1").arg(encoded.error);
        return false;
    }
    if (!ensureOrderRow(db, entry.queueId, entry.domain, error)
        || !_writeArtifactSets(db, entry, error))
        return false;
    const QVariantList common{
        entry.queueId,
        restoreState(entry.domain),
        entry.runId,
        static_cast<qint64>(entry.recipeRevision),
        originName(entry.origin),
        entry.tabNumber > 0 ? QVariant(entry.tabNumber) : QVariant(QMetaType::fromType<int>()),
        entry.held ? 1 : 0,
        entry.blocked.isBlocked() ? QVariant(entry.blocked.code) : nullString(),
        blockedParams(entry.blocked),
        lifecycleName(entry.lifecycle),
        entry.artifacts.isNull() ? nullString() : QVariant(entry.artifacts.id),
        entry.sourceBytes,
        QString::fromUtf8(QJsonDocument(encoded.json).toJson(QJsonDocument::Compact)),
        _now()
    };
    int rows = 1;
    if (insert) {
        QVariantList values{ entry.postId };
        values << common << entry.enqueuedAt.toUTC().toString(Qt::ISODate);
        if (!run(db,
                 QStringLiteral(
                     "INSERT INTO post_queue(post_id, queue_id, restore_state, run_id, "
                     "recipe_revision,"
                     "origin, tab_number, held, blocked_reason, blocked_params_json, stage,"
                     "current_artifact_set_id, source_bytes, options_json, updated_at, enqueued_at,"
                     "position) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0)"),
                 values,
                 error))
            return false;
    } else {
        QVariantList values = common;
        values << entry.postId;
        if (!run(db,
                 QStringLiteral(
                     "UPDATE post_queue SET queue_id=?, restore_state=?, run_id=?, "
                     "recipe_revision=?,"
                     "origin=?, tab_number=?, held=?, blocked_reason=?, blocked_params_json=?, "
                     "stage=?,"
                     "current_artifact_set_id=?, source_bytes=?, options_json=?, updated_at=? "
                     "WHERE post_id=?"),
                 values,
                 error,
                 &rows))
            return false;
    }
    if (rows != 1) {
        if (error)
            *error = QStringLiteral("queue entry %1 is missing").arg(entry.postId);
        return false;
    }
    if (entry.transferManifestEngaged
        && !run(db,
                QStringLiteral("UPDATE post_info SET transfer_manifest_complete=1 WHERE post_id=?"),
                { entry.postId },
                error))
        return false;
    // The run follows its entry until it has a result.
    const QString state = runState(entry);
    if (!run(
            db,
            QStringLiteral(
                "UPDATE post_runs SET state=?, started_at=CASE WHEN ?='posting' THEN "
                "COALESCE(started_at, ?) ELSE started_at END WHERE run_id=? AND result_recorded=0"),
            { state, state, _now(), entry.runId },
            error))
        return false;
    return insert || _mirrorQueuedPost(db, entry, error);
}

//! Before its transfer, an edited recipe is also what History must show.
bool PostHistoryStore::_mirrorQueuedPost(QSqlDatabase &db, const QueueEntry &entry, QString *error)
{
    const PostingJobOptions &job = entry.options.job;
    int rows = 0;
    if (!run(db,
             QStringLiteral(
                 "UPDATE posts SET nzb_name=?, nzb_path=?, rar_name=?, from_addr=?, do_compress=?,"
                 "do_par2=?, obfuscate_articles=?, obfuscate_file_name=? "
                 "WHERE id=? AND status='queued'"),
             { QFileInfo(job.nzbFilePath).completeBaseName(),
               job.nzbFilePath,
               job.rarName,
               QString::fromStdString(job.from),
               job.doCompress ? 1 : 0,
               job.doPar2 ? 1 : 0,
               job.obfuscateArticles ? 1 : 0,
               job.obfuscateFileName ? 1 : 0,
               entry.postId },
             error,
             &rows))
        return false;
    if (rows == 0)
        return true; // already started: the transfer owns the history row now
    if (!run(db,
             QStringLiteral("DELETE FROM post_groups WHERE post_id=?"),
             { entry.postId },
             error))
        return false;
    for (const QString &group : job.grpList)
        if (!run(db,
                 QStringLiteral("INSERT INTO post_groups(post_id, group_name) VALUES(?, ?)"),
                 { entry.postId, group },
                 error))
            return false;
    return true;
}

bool PostHistoryStore::_renewQueueRun(QSqlDatabase &db,
                                      qint64 postId,
                                      QString *runId,
                                      int *runNo,
                                      QString *error)
{
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT q.run_id, (SELECT COALESCE(MAX(run_no), 0) + 1 FROM post_runs r "
                       "WHERE r.post_id = q.post_id) FROM post_queue q WHERE q.post_id=?"));
    q.addBindValue(postId);
    if (!q.exec() || !q.next()) {
        if (error)
            *error = q.lastError().text().isEmpty()
                ? QStringLiteral("queue entry %1 is missing").arg(postId)
                : q.lastError().text();
        return false;
    }
    const QString previous = q.value(0).toString();
    *runNo = q.value(1).toInt();
    *runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // The interruption is recorded once: a run that already has a result keeps it.
    return run(db,
               QStringLiteral("UPDATE post_runs SET state='interrupted', result='interrupted', "
                              "result_recorded=1, end_cause='keep', finished_at=? "
                              "WHERE run_id=? AND result_recorded=0"),
               { _now(), previous },
               error)
        && run(db,
               QStringLiteral("INSERT INTO post_runs(run_id, post_id, run_no, created_at, state) "
                              "VALUES(?, ?, ?, ?, 'queued')"),
               { *runId, postId, *runNo, _now() },
               error)
        && run(db,
               QStringLiteral("UPDATE post_queue SET run_id=? WHERE post_id=?"),
               { *runId, postId },
               error)
        && run(db,
               QStringLiteral("UPDATE post_info SET current_run_id=? WHERE post_id=?"),
               { *runId, postId },
               error);
}

//! Ends the current run of @p postId with a known outcome, before its retirement.
//! A post that never transferred takes the outcome as its status; a resume keeps
//! its own, and with it what it can still resume.
bool PostHistoryStore::_recordRunOutcome(QSqlDatabase &db,
                                         qint64 postId,
                                         const RunOutcome &outcome,
                                         QString *error)
{
    return run(db,
               QStringLiteral(
                   "UPDATE post_runs SET state='terminal', result=?, result_recorded=1, "
                   "end_cause=?, finished_at=?, supervision_json=COALESCE(?, supervision_json) "
                   "WHERE run_id=(SELECT run_id FROM post_queue WHERE post_id=?) AND "
                   "result_recorded=0"),
               { outcome.result,
                 outcome.endCause,
                 _now(),
                 outcome.diagnosticJson.isEmpty() ? nullString() : QVariant(outcome.diagnosticJson),
                 postId },
               error)
        && run(db,
               QStringLiteral("UPDATE posts SET status=?, finished_at=? WHERE id=? AND "
                              "status='queued'"),
               { outcome.result, _now(), postId },
               error);
}

//! The entry leaves the queue. A post that never started becomes 'cancelled'
//! and is released; a started one keeps its history and its resumability.
bool PostHistoryStore::_retireQueueEntry(QSqlDatabase &db, qint64 postId, QString *error)
{
    // A run that transferred ends with the outcome its post recorded.
    return run(db,
               QStringLiteral(
                   "UPDATE post_runs SET state='terminal', result=(SELECT CASE status WHEN "
                   "'success' THEN 'success' WHEN 'partial' THEN 'partial' WHEN 'cancelled' THEN "
                   "'cancelled' ELSE 'failed' END FROM posts WHERE id=?), result_recorded=1, "
                   "end_cause=(SELECT CASE status WHEN 'cancelled' THEN 'user_stop' ELSE 'normal' "
                   "END FROM posts WHERE id=?), finished_at=? WHERE run_id=(SELECT run_id FROM "
                   "post_queue WHERE post_id=?) AND result_recorded=0 AND state IN "
                   "('posting','settling')"),
               { postId, postId, _now(), postId },
               error)
        && run(db,
               QStringLiteral(
                   "UPDATE post_runs SET state='terminal', result='cancelled', result_recorded=1, "
                   "end_cause='abandon', finished_at=? WHERE run_id=(SELECT run_id FROM post_queue "
                   "WHERE post_id=?) AND result_recorded=0 AND state IN ('queued','preparing')"),
               { _now(), postId },
               error)
        && run(db,
               QStringLiteral("UPDATE posts SET status='cancelled', finished_at=?, owner_id=NULL "
                              "WHERE id=? AND status='queued'"),
               { _now(), postId },
               error)
        // Out of the queue, a post belongs to no session any more.
        && run(db, QStringLiteral("UPDATE posts SET owner_id=NULL WHERE id=?"), { postId }, error)
        && run(db,
               QStringLiteral("DELETE FROM post_queue_intents WHERE target_post_id=?"),
               { postId },
               error)
        && run(db, QStringLiteral("DELETE FROM post_queue WHERE post_id=?"), { postId }, error);
}

bool PostHistoryStore::_writeQueueOrder(QSqlDatabase &db,
                                        const QString &queueId,
                                        Domain domain,
                                        const QList<qint64> &ids,
                                        quint64 revision,
                                        QString *error)
{
    if (!ensureOrderRow(db, queueId, domain, error))
        return false;
    for (int i = 0; i < ids.size(); ++i)
        if (!run(db,
                 QStringLiteral("UPDATE post_queue SET position=?, queue_id=?, restore_state=? "
                                "WHERE post_id=?"),
                 { i, queueId, restoreState(domain), ids.at(i) },
                 error))
            return false;
    return run(
        db,
        QStringLiteral(
            "UPDATE post_queue_orders SET order_revision=? WHERE queue_id=? AND restore_state=?"),
        { static_cast<qint64>(revision), queueId, restoreState(domain) },
        error);
}

bool PostHistoryStore::_writeQueueIntent(QSqlDatabase &db,
                                         const QueueTransaction &transaction,
                                         QString *error)
{
    if (!transaction.dropIntentId.isEmpty()
        && !run(db,
                QStringLiteral("DELETE FROM post_queue_intents WHERE intent_id=?"),
                { transaction.dropIntentId },
                error))
        return false;
    if (!transaction.setIntent)
        return true;
    const PostNowIntent &intent = *transaction.setIntent;
    return run(db,
               QStringLiteral(
                   "INSERT OR REPLACE INTO post_queue_intents(intent_id, queue_id, target_post_id,"
                   "target_recipe_revision, active_run_id, state, created_at) "
                   "VALUES(?, ?, ?, ?, ?, ?, ?)"),
               { intent.intentId,
                 intent.queueId,
                 intent.targetPostId,
                 static_cast<qint64>(intent.targetRecipeRevision),
                 intent.activeRunId,
                 intent.state == PostNowIntent::State::Switching ? QStringLiteral("switching")
                                                                 : QStringLiteral("pending"),
                 _now() },
               error);
}

// --- reading ---------------------------------------------------------------------------------

namespace
{

void readArtifacts(QSqlDatabase &db, QueueEntry &e, const QString &currentId)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT artifact_set_id, generation, recipe_revision, bytes, root, state, retained "
        "FROM post_artifact_sets WHERE post_id=? ORDER BY generation"));
    q.addBindValue(e.postId);
    if (!q.exec())
        return;
    while (q.next()) {
        const ArtifactSetRef set{ q.value(0).toString(),
                                  q.value(1).toInt(),
                                  static_cast<quint64>(q.value(2).toLongLong()),
                                  q.value(3).toLongLong(),
                                  q.value(4).toString() };
        e.lastGeneration = std::max(e.lastGeneration, set.generation);
        if (set.id == currentId)
            e.artifacts = set;
        else if (q.value(6).toInt() != 0 && q.value(5).toString() == QStringLiteral("invalidated"))
            e.retainedSets << set;
    }
}

QueueEntry entryFromRow(const QSqlQuery &q, const QString &queueId)
{
    QueueEntry e;
    e.postId = q.value(0).toLongLong();
    e.runId = q.value(1).toString();
    e.runNo = q.value(2).toInt();
    e.queueId = queueId;
    e.domain = q.value(3).toString() == QStringLiteral("deferred") ? Domain::Deferred
                                                                   : Domain::Session;
    parseOrigin(q.value(4).toString(), e.origin);
    e.tabNumber = q.value(5).toInt();
    e.held = q.value(6).toInt() != 0;
    if (!q.value(7).isNull())
        e.blocked = {
            q.value(7).toString(),
            QJsonDocument::fromJson(q.value(8).toString().toUtf8()).object().toVariantMap()
        };
    parseLifecycle(q.value(9).toString(), e.lifecycle);
    e.recipeRevision = static_cast<quint64>(q.value(10).toLongLong());
    e.transferManifestEngaged = q.value(11).toInt() != 0;
    e.unsettledSourceJournal = q.value(12).toInt() != 0;
    e.sourceBytes = q.value(14).toLongLong();
    const auto decoded = optionscodec::decode(
        QJsonDocument::fromJson(q.value(15).toString().toUtf8()).object());
    if (decoded.ok())
        e.options = decoded.options;
    else
        e.blocked = { QStringLiteral("options_unreadable"),
                      { { QStringLiteral("error"), decoded.error } } };
    e.enqueuedAt = QDateTime::fromString(q.value(16).toString(), Qt::ISODate);
    e.options.job.rarPass = q.value(17).toString();
    e.name = QFileInfo(e.options.job.nzbFilePath).completeBaseName();
    return e;
}

} // namespace

QList<QueueEntry> PostHistoryStore::loadQueueEntries(const QString &queueId, QString *error)
{
    QList<QueueEntry> entries;
    if (!initialize(error))
        return entries;
    QSqlDatabase db = _database(error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT q.post_id, q.run_id, r.run_no, q.restore_state, q.origin, q.tab_number, q.held,"
        "q.blocked_reason, q.blocked_params_json, q.stage, q.recipe_revision,"
        "COALESCE(i.transfer_manifest_complete, 0),"
        "EXISTS(SELECT 1 FROM post_source_moves m WHERE m.post_id=q.post_id AND m.state <> "
        "'restored'),"
        "q.current_artifact_set_id, q.source_bytes, q.options_json, q.enqueued_at, p.rar_pass "
        "FROM post_queue q JOIN posts p ON p.id = q.post_id LEFT JOIN post_info i ON i.post_id = "
        "q.post_id "
        "LEFT JOIN post_runs r ON r.run_id = q.run_id WHERE q.queue_id=? "
        "ORDER BY q.restore_state DESC, q.position, q.post_id"));
    q.addBindValue(queueId);
    if (!q.exec()) {
        _setQueryError(error, q);
        return entries;
    }
    while (q.next()) {
        QueueEntry e = entryFromRow(q, queueId);
        readArtifacts(db, e, q.value(13).toString());
        entries << e;
    }
    return entries;
}

QList<qint64> PostHistoryStore::loadQueueOrder(const QString &queueId,
                                               Domain domain,
                                               QString *error)
{
    QList<qint64> ids;
    if (!initialize(error))
        return ids;
    QSqlDatabase db = _database(error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT post_id FROM post_queue WHERE queue_id=? AND restore_state=? "
                             "ORDER BY position, post_id"));
    q.addBindValue(queueId);
    q.addBindValue(restoreState(domain));
    if (!q.exec())
        _setQueryError(error, q);
    while (q.next())
        ids << q.value(0).toLongLong();
    return ids;
}

std::optional<PostNowIntent> PostHistoryStore::loadQueueIntent(const QString &queueId,
                                                               QString *error)
{
    if (!initialize(error))
        return std::nullopt;
    QSqlDatabase db = _database(error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT intent_id, target_post_id, target_recipe_revision, active_run_id, state "
        "FROM post_queue_intents WHERE queue_id=?"));
    q.addBindValue(queueId);
    if (!q.exec() || !q.next()) {
        if (q.lastError().isValid())
            _setQueryError(error, q);
        return std::nullopt;
    }
    PostNowIntent intent;
    intent.intentId = q.value(0).toString();
    intent.queueId = queueId;
    intent.targetPostId = q.value(1).toLongLong();
    intent.targetRecipeRevision = static_cast<quint64>(q.value(2).toLongLong());
    intent.activeRunId = q.value(3).toString();
    intent.state = q.value(4).toString() == QStringLiteral("switching")
        ? PostNowIntent::State::Switching
        : PostNowIntent::State::Pending;
    return intent;
}

QHash<QString, int> PostHistoryStore::queueIds(QString *error)
{
    QHash<QString, int> ids;
    if (!initialize(error))
        return ids;
    QSqlDatabase db = _database(error);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT queue_id, COUNT(*) FROM post_queue GROUP BY queue_id"))) {
        _setQueryError(error, q);
        return ids;
    }
    while (q.next())
        ids.insert(q.value(0).toString(), q.value(1).toInt());
    return ids;
}

// --- ownership ---------------------------------------------------------------------------------

namespace
{

//! "owner_id IS NULL OR owner_id NOT IN (...)": the posts no live owner holds.
QString orphanFilter(const QStringList &liveOwners, const QString &column)
{
    if (liveOwners.isEmpty())
        return QStringLiteral("1=1");
    QStringList marks;
    for (int i = 0; i < liveOwners.size(); ++i)
        marks << QStringLiteral("?");
    return QStringLiteral("(%1 IS NULL OR %1 NOT IN (%2))")
        .arg(column, marks.join(QLatin1Char(',')));
}

QVariantList asValues(const QStringList &list)
{
    QVariantList values;
    for (const QString &s : list)
        values << s;
    return values;
}

} // namespace

bool PostHistoryStore::normalizeInterruptedPosts(const QStringList &liveOwners, QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = _database(error);
    const QString posts = orphanFilter(liveOwners, QStringLiteral("p.owner_id"));
    const QVariantList owners = asValues(liveOwners);
    const QString files =
        QStringLiteral("SELECT f.id FROM post_files f JOIN posts p ON p.id = f.post_id WHERE ")
        + posts;
    const QString stopped = QStringLiteral("application stopped before confirmation");
    return run(db,
               QStringLiteral(
                   "UPDATE post_articles SET status='unknown', error=?, updated_at=datetime('now') "
                   "WHERE status='posting' AND file_id IN (%1)")
                   .arg(files),
               QVariantList{ stopped } + owners,
               error)
        && run(db,
               QStringLiteral(
                   "UPDATE post_article_attempts SET status='unknown', error=?, "
                   "finished_at=datetime('now') WHERE status='posting' AND file_id IN (%1)")
                   .arg(files),
               QVariantList{ stopped } + owners,
               error)
        && run(db,
               QStringLiteral(
                   "UPDATE posts SET status='cancelled', resume_state=NULL,"
                   "resume_reason='posting never started; nothing to resume',"
                   "finished_at=COALESCE(finished_at, datetime('now')) WHERE status='posting' "
                   "AND NOT EXISTS (SELECT 1 FROM post_files WHERE post_id=posts.id) "
                   "AND NOT EXISTS (SELECT 1 FROM post_queue WHERE post_id=posts.id) "
                   "AND id IN (SELECT p.id FROM posts p WHERE %1)")
                   .arg(posts),
               owners,
               error)
        && run(db,
               QStringLiteral(
                   "UPDATE posts SET status='resumable', resume_state='resumable',"
                   "resume_reason='application stopped while posting' WHERE status='posting' "
                   "AND EXISTS (SELECT 1 FROM post_files WHERE post_id=posts.id) "
                   "AND id IN (SELECT p.id FROM posts p WHERE %1)")
                   .arg(posts),
               owners,
               error);
}

bool PostHistoryStore::cleanupInvalidResumePosts(const QStringList &liveOwners, QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = _database(error);
    return run(db,
               QStringLiteral(
                   "UPDATE posts SET status='cancelled', resume_state=NULL,"
                   "resume_reason='posting never started; nothing to resume',"
                   "finished_at=COALESCE(finished_at, datetime('now'))"
                   " WHERE (status IN ('posting','resumable') OR resume_state='resumable')"
                   " AND NOT EXISTS (SELECT 1 FROM post_files WHERE post_id=posts.id)"
                   " AND NOT EXISTS (SELECT 1 FROM post_queue WHERE post_id=posts.id)"
                   " AND id IN (SELECT p.id FROM posts p WHERE %1)")
                   .arg(orphanFilter(liveOwners, QStringLiteral("p.owner_id"))),
               asValues(liveOwners),
               error);
}

int PostHistoryStore::claimQueue(const QString &fromQueueId,
                                 const QString &fromOwnerId,
                                 const QString &toQueueId,
                                 const QString &toOwnerId,
                                 Domain domain,
                                 QString *error)
{
    if (!initialize(error))
        return -1;
    QSqlDatabase db = _database(error);
    if (!db.transaction())
        return -1;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT post_id FROM post_queue WHERE queue_id=? "
                             "ORDER BY restore_state DESC, position, post_id"));
    q.addBindValue(fromQueueId);
    QList<qint64> ids;
    if (q.exec())
        while (q.next())
            ids << q.value(0).toLongLong();
    QSqlQuery base(db);
    base.prepare(
        QStringLiteral("SELECT COALESCE(MAX(position) + 1, 0) FROM post_queue WHERE queue_id=? "
                       "AND restore_state=?"));
    base.addBindValue(toQueueId);
    base.addBindValue(restoreState(domain));
    const int first = base.exec() && base.next() ? base.value(0).toInt() : 0;
    bool ok = ensureOrderRow(db, toQueueId, domain, error);
    for (int i = 0; ok && i < ids.size(); ++i)
        ok = run(
            db,
            QStringLiteral(
                "UPDATE post_queue SET queue_id=?, restore_state=?, position=? WHERE post_id=?"),
            { toQueueId, restoreState(domain), first + i, ids.at(i) },
            error);
    ok = ok
        && run(db,
               QStringLiteral("UPDATE posts SET owner_id=? WHERE owner_id=? AND id IN (SELECT "
                              "post_id FROM post_queue)"),
               { toOwnerId, fromOwnerId },
               error)
        && run(db,
               QStringLiteral("UPDATE posts SET owner_id=NULL WHERE owner_id=?"),
               { fromQueueId },
               error)
        && run(db,
               QStringLiteral("DELETE FROM post_queue_intents WHERE queue_id=?"),
               { fromQueueId },
               error)
        && run(db,
               QStringLiteral("DELETE FROM post_queue_orders WHERE queue_id=?"),
               { fromQueueId },
               error);
    if (!ok || !db.commit()) {
        db.rollback();
        return -1;
    }
    return static_cast<int>(ids.size());
}
