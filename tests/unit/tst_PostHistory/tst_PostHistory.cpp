// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_PostHistory.cpp -- SQLite history, resume classification and NZB
// regeneration checks for the LevelUp posting history.
//
//========================================================================

#include <QtTest>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QXmlStreamReader>

#include "history/NzbHistoryRegenerator.h"
#include "history/PostHistoryService.h"
#include "history/PostHistoryStore.h"
#include "history/ResumePlanner.h"
#include "NgPost.h"

class TestPostHistory : public QObject
{
    Q_OBJECT

private slots:
    //! Building the service must not create a database. It is constructed
    //! before the config is read, so the path it holds is still the default
    //! one; touching it there dropped an empty history in every folder ngPost
    //! ever resolved, even when POST_DB pointed elsewhere.
    void service_construction_creates_no_database();

    //! And when the path is reconfigured, only the configured database is
    //! ever created — never the one the service was built with.
    void reconfigured_service_only_creates_the_configured_database();

    //! GUI startup queues cleanup before its first snapshot. Both operations
    //! must use POST_DB, in FIFO order, without touching the constructor's
    //! provisional default path.
    void prepared_service_uses_configured_database_before_snapshot();

    //! The one that matters: a database that already holds posts must come
    //! back with every row intact, whatever the service does at startup.
    void existing_database_with_entries_is_never_reset();

    void sqlite_lifecycle_and_resume_states();
    void empty_resume_post_is_cancelled_by_cleanup();
    void terminal_failure_without_files_stays_failed_and_not_resumable();
    void missing_source_is_not_resumable();
    void mark_article_status_keeps_payload_when_row_is_missing();
    void apply_article_events_batches_ordered_status_changes();
    void terminal_status_purges_attempt_audit_rows();
    void list_posts_paginates_with_stable_order();
    void list_posts_applies_filters_before_pagination();
    void service_flushes_batches_and_returns_snapshots();
    //! A resume decision must never be made from the database when a queued
    //! article event could not be persisted first.
    void service_resume_queries_fail_closed_when_flush_fails();
    void service_snapshot_paginates_history_but_stats_are_complete();
    //! An existing v1 database gains the new tables on open, keeps its rows,
    //! and records the new schema version.
    void schema_migrates_v1_database_without_losing_posts();

    //! v2 had post_info but not the frozen article boundary. Opening it adds
    //! the column and old posts are resumed only when their persisted segments
    //! prove one exact value.
    void schema_migrates_v2_and_derives_article_size_safely();

    //! A database written by a newer ngPost is refused rather than silently
    //! used with a schema this version does not understand.
    void schema_refuses_a_newer_database();

    //! Corrupt/non-numeric versions are not silently interpreted as v1.
    void schema_refuses_an_invalid_version();

    //! started_at stays NULL until the transfer begins, and a resume never
    //! rewrites the date of the first attempt.
    void started_at_is_set_by_the_transfer_and_never_rewritten();

    //! Merely beginning a resume must not discard the outcome of the previous
    //! attempt; a source check can still refuse the retry before any transfer.
    void resume_marker_preserves_previous_terminal_facts();

    //! The size of the whole post is written once: a resume only sees the
    //! leftovers and must not shrink it. The transfer time, on the contrary,
    //! accumulates over attempts.
    void post_size_is_written_once_and_active_seconds_accumulate();

    //! Posted means the persisted article status is exactly `posted`; pending
    //! rows must not be inferred as successes by subtracting only failures.
    void post_info_counts_only_explicitly_posted_articles();

    //! Metadata round trip, scope included, and "password" refused: it is a
    //! secret, not a metadata.
    void post_meta_keeps_scope_and_refuses_the_password_key();

    //! Only the metadata the user chose to publish reaches the delivered nzb.
    void regenerated_nzb_publishes_only_nzb_scoped_meta();

    //! A resume must not rebuild the archive: doCompress and doPar2 are orders,
    //! and the rar/par2 volumes are already on disk. What the original post did
    //! is carried as facts instead.
    void resume_options_never_replay_compression_or_par2();

    //! The obfuscation of the original post is replayed from the history, not
    //! taken from the globals, which may have changed since.
    void resume_options_take_obfuscation_from_history();

    //! A resumed post still describes the par2 redundancy of the ORIGINAL
    //! post: it is a fact about the archive, not an order to redo anything.
    void resume_describes_the_original_par2_percentage();

    //! A source is checked again right before being read, not only when the
    //! resume plan was built: a job can wait a long time in the queue.
    void resume_file_state_detects_a_source_that_changed();

    //! A resume must not carry the metadata or the password of the run that
    //! happens to be going on: they belong to another post.
    void resume_options_drop_the_current_metadata_and_password();

    //! When password storage is disabled, the delivered NZB is the only
    //! durable place from which a resume may recover the archive password.
    void resume_recovers_unstored_password_from_existing_nzb();

    //! A protected post must not resume with an unrelated current password or
    //! silently publish a consolidated NZB without its original password.
    void resume_fails_closed_when_unstored_password_is_absent_from_nzb();

    void nzb_regeneration_keeps_prior_files_after_resume();
    void nzb_regeneration_repairs_missing_article_bytes();
    void nzb_regeneration_masks_password_by_default();
    void nzb_regeneration_uses_live_password_without_storing_it();
    void nzb_regeneration_refuses_incomplete_success_and_empty_history();
    void nzb_regeneration_refuses_a_corrupt_stored_article_size();
    //! A failed regeneration must leave an existing NZB byte-for-byte intact.
    void nzb_regeneration_to_file_is_atomic_on_failure();
    void import_legacy_csv_is_explicit_history_only();
};

namespace
{

int countNzbSegments(const QString &nzb)
{
    QXmlStreamReader reader(nzb);
    int segments = 0;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QLatin1String("segment"))
            ++segments;
    }
    return segments;
}

QList<qint64> nzbSegmentBytes(const QString &nzb)
{
    QXmlStreamReader reader(nzb);
    QList<qint64> bytes;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QLatin1String("segment"))
            bytes << reader.attributes().value(QStringLiteral("bytes")).toLongLong();
    }
    return bytes;
}

// Runs raw SQL on the database file, to assert (or forge) what the store API
// does not expose. Returns the first column of the first row, if any.
QVariant rawSql(const QString &dbPath, const QString &sql, const QVariantList &binds = {})
{
    static int sConnectionCounter = 0;
    const QString conn = QStringLiteral("tst_raw_sql_%1").arg(++sConnectionCounter);
    QVariant result;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(sql);
            for (const QVariant &bind : binds)
                q.addBindValue(bind);
            if (q.exec() && q.next())
                result = q.value(0);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return result;
}

bool execRawSql(const QString &dbPath,
                const QString &sql,
                const QVariantList &binds = {},
                QString *error = nullptr)
{
    static int sConnectionCounter = 0;
    const QString conn = QStringLiteral("tst_raw_exec_%1").arg(++sConnectionCounter);
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(sql);
            for (const QVariant &bind : binds)
                q.addBindValue(bind);
            ok = q.exec();
            if (!ok && error)
                *error = q.lastError().text();
            db.close();
        } else if (error) {
            *error = db.lastError().text();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

// Counts the per-attempt audit rows of a post by reading the SQLite file
// directly, since the store exposes no API for them (they are write-only).
int countAttemptRows(const QString &dbPath, qint64 postId)
{
    int n = -1;
    const QString conn = QStringLiteral("tst_count_attempts");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("SELECT COUNT(*) FROM post_article_attempts "
                                     "WHERE file_id IN (SELECT id FROM post_files WHERE post_id=?)"));
            q.addBindValue(postId);
            if (q.exec() && q.next())
                n = q.value(0).toInt();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return n;
}

qint64 createStoredPost(PostHistoryStore &store,
                        const QString &name,
                        const QString &status,
                        const QStringList &groups,
                        QString *error)
{
    PostHistoryStore::PostRecord post;
    post.nzbName = name;
    post.nzbPath = QStringLiteral("/tmp/%1").arg(name);
    post.rarName = name + QStringLiteral(".rar");
    post.from = QStringLiteral("poster@example.invalid");
    post.groups = groups;
    const qint64 postId = store.createPost(post, error);
    if (postId <= 0)
        return 0;
    if (status != QStringLiteral("posting")
        && !store.updatePostStatus(postId, status, 0, 0, 0, 0, QString(), error))
        return 0;
    return postId;
}

bool updateCreatedAt(const QString &dbPath,
                     const QList<qint64> &postIds,
                     const QString &createdAt,
                     QString *error)
{
    if (postIds.isEmpty())
        return true;

    static int sConnectionCounter = 0;
    const QString conn = QStringLiteral("tst_update_created_at_%1").arg(++sConnectionCounter);
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (!db.open()) {
            if (error)
                *error = db.lastError().text();
            ok = false;
        } else if (!db.transaction()) {
            if (error)
                *error = db.lastError().text();
            ok = false;
        } else {
            QSqlQuery q(db);
            if (!q.prepare(QStringLiteral("UPDATE posts SET created_at=? WHERE id=?"))) {
                if (error)
                    *error = q.lastError().text();
                ok = false;
            }
            for (const qint64 postId : postIds) {
                if (!ok)
                    break;
                q.bindValue(0, createdAt);
                q.bindValue(1, postId);
                if (!q.exec()) {
                    if (error)
                        *error = q.lastError().text();
                    ok = false;
                    break;
                }
            }
            if (ok && !db.commit()) {
                if (error)
                    *error = db.lastError().text();
                ok = false;
            }
            if (!ok)
                db.rollback();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

} // namespace

void TestPostHistory::sqlite_lifecycle_and_resume_states()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString err;
    QVERIFY2(store.initialize(&err), qPrintable(err));

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("demo.nzb");
    post.nzbPath = dir.filePath(QStringLiteral("demo.nzb"));
    post.from = QStringLiteral("poster@example.invalid");
    post.groups = { QStringLiteral("alt.binaries.test") };
    const qint64 postId = store.createPost(post, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    const QString sourcePath = dir.filePath(QStringLiteral("payload.bin"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write(QByteArray(12, 'x'));
    source.close();

    QFileInfo sourceInfo(sourcePath);
    PostHistoryStore::FileRecord file;
    file.postId = postId;
    file.ordinal = 1;
    file.originalPath = sourceInfo.absoluteFilePath();
    file.postedName = sourceInfo.fileName();
    file.sizeBytes = sourceInfo.size();
    file.mtimeEpoch = sourceInfo.lastModified().toSecsSinceEpoch();
    file.totalArticles = 3;
    file.groups = post.groups;
    const qint64 fileId = store.upsertFile(file, &err);
    QVERIFY2(fileId > 0, qPrintable(err));

    PostHistoryStore::ArticleRecord a1;
    a1.fileId = fileId;
    a1.part = 1;
    a1.bytes = 4;
    QVERIFY2(store.upsertArticle(a1, &err), qPrintable(err));
    QVERIFY2(store.markArticlePosting(fileId, 1, QStringLiteral("old@ngpost"), 1, &err), qPrintable(err));
    QVERIFY2(store.markArticleUnknown(fileId, 1, QStringLiteral("old@ngpost"),
                                      QStringLiteral("network lost"), &err),
             qPrintable(err));

    PostHistoryStore::ArticleRecord a2 = a1;
    a2.part = 2;
    QVERIFY2(store.upsertArticle(a2, &err), qPrintable(err));
    QVERIFY2(store.markArticlePosting(fileId, 2, QStringLiteral("ok@ngpost"), 1, &err), qPrintable(err));
    QVERIFY2(store.markArticlePosted(fileId, 2, QStringLiteral("ok@ngpost"), &err), qPrintable(err));
    QVERIFY2(store.updateArticlePayload(fileId, 2, 4, 4, &err), qPrintable(err));

    PostHistoryStore::ArticleRecord a3 = a1;
    a3.part = 3;
    QVERIFY2(store.upsertArticle(a3, &err), qPrintable(err));
    QVERIFY2(store.markArticlePosting(fileId, 3, QStringLiteral("ko@ngpost"), 1, &err), qPrintable(err));
    QVERIFY2(store.markArticleFailed(fileId, 3, QStringLiteral("ko@ngpost"),
                                     QStringLiteral("server rejected"), &err),
             qPrintable(err));

    ResumePlanner planner(&store);
    const ResumePlanner::Decision d = planner.check(postId, &err);
    QCOMPARE(d.state, ResumePlanner::ResumeState::PartiallyResumable);
    QCOMPARE(d.postedArticles, 1);
    QCOMPARE(d.failedArticles, 1);
    QCOMPARE(d.unknownArticles, 1);

    PostHistoryStore::PostDetails details;
    QVERIFY2(store.loadPostDetails(postId, &details, &err), qPrintable(err));
    QCOMPARE(details.articlesByFile.value(fileId).at(1).status, QStringLiteral("posted"));
    QCOMPARE(details.articlesByFile.value(fileId).at(1).msgId, QStringLiteral("ok@ngpost"));
}

void TestPostHistory::empty_resume_post_is_cancelled_by_cleanup()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString err;
    QVERIFY2(store.initialize(&err), qPrintable(err));

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("never-started.nzb");
    post.nzbPath = dir.filePath(QStringLiteral("never-started.nzb"));
    post.from = QStringLiteral("poster@example.invalid");
    post.groups = { QStringLiteral("alt.binaries.test") };
    const qint64 postId = store.createPost(post, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    QVERIFY2(store.cleanupInvalidResumePosts(&err), qPrintable(err));
    QVERIFY2(store.cleanupInvalidResumePosts(&err), qPrintable(err));

    PostHistoryStore::PostDetails details;
    QVERIFY2(store.loadPostDetails(postId, &details, &err), qPrintable(err));
    QCOMPARE(details.post.status, QStringLiteral("cancelled"));
    QVERIFY(!details.post.resumable);
    QCOMPARE(details.post.resumeReason,
             QStringLiteral("posting never started; nothing to resume"));
    QCOMPARE(details.files.size(), 0);

    ResumePlanner planner(&store);
    const ResumePlanner::Decision d = planner.check(postId, &err);
    QCOMPARE(d.state, ResumePlanner::ResumeState::NotResumable);
    QCOMPARE(d.reason, QStringLiteral("posting never started; nothing to resume"));
    QCOMPARE(store.resumeCandidates(&err).size(), 0);
}

void TestPostHistory::terminal_failure_without_files_stays_failed_and_not_resumable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString error;

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("failed-before-transfer.nzb");
    const qint64 postId = store.createPost(post, &error);
    QVERIFY2(postId > 0, qPrintable(error));
    QVERIFY2(store.updatePostStatus(postId,
                                    QStringLiteral("failed"),
                                    0,
                                    0,
                                    0,
                                    0,
                                    QString(),
                                    &error),
             qPrintable(error));

    QVERIFY2(store.cleanupInvalidResumePosts(&error), qPrintable(error));
    PostHistoryStore::PostDetails details;
    QVERIFY2(store.loadPostDetails(postId, &details, &error), qPrintable(error));
    QCOMPARE(details.post.status, QStringLiteral("failed"));
    QVERIFY(!details.post.resumable);
    QCOMPARE(details.post.resumeReason,
             QStringLiteral("posting never started; nothing to resume"));
}

void TestPostHistory::missing_source_is_not_resumable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString err;
    QVERIFY2(store.initialize(&err), qPrintable(err));

    const QString sourcePath = dir.filePath(QStringLiteral("gone.bin"));
    {
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write(QByteArray(8, 'x'));
    }
    const QFileInfo sourceInfo(sourcePath);

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("missing-source.nzb");
    post.nzbPath = dir.filePath(QStringLiteral("missing-source.nzb"));
    post.from = QStringLiteral("poster@example.invalid");
    post.groups = { QStringLiteral("alt.binaries.test") };
    const qint64 postId = store.createPost(post, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    PostHistoryStore::FileRecord file;
    file.postId = postId;
    file.ordinal = 1;
    file.originalPath = sourceInfo.absoluteFilePath();
    file.postedName = sourceInfo.fileName();
    file.sizeBytes = sourceInfo.size();
    file.mtimeEpoch = sourceInfo.lastModified().toSecsSinceEpoch();
    file.totalArticles = 2;
    file.groups = post.groups;
    const qint64 fileId = store.upsertFile(file, &err);
    QVERIFY2(fileId > 0, qPrintable(err));

    PostHistoryStore::ArticleRecord article;
    article.fileId = fileId;
    article.part = 1;
    article.bytes = 4;
    QVERIFY2(store.upsertArticle(article, &err), qPrintable(err));
    QVERIFY2(store.markArticlePosted(fileId, 1, QStringLiteral("ok@ngpost"), &err), qPrintable(err));
    QVERIFY(QFile::remove(sourcePath));

    ResumePlanner planner(&store);
    const ResumePlanner::Decision d = planner.check(postId, &err);
    QCOMPARE(d.state, ResumePlanner::ResumeState::NotResumable);
    QCOMPARE(d.reason, QStringLiteral("source files are missing or changed"));
}

void TestPostHistory::mark_article_status_keeps_payload_when_row_is_missing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString err;
    QVERIFY2(store.initialize(&err), qPrintable(err));

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("payload-status.nzb");
    post.nzbPath = dir.filePath(QStringLiteral("payload-status.nzb"));
    post.from = QStringLiteral("poster@example.invalid");
    post.groups = { QStringLiteral("alt.binaries.test") };
    const qint64 postId = store.createPost(post, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    PostHistoryStore::FileRecord file;
    file.postId = postId;
    file.ordinal = 1;
    file.postedName = QStringLiteral("payload.bin");
    file.sizeBytes = 42;
    file.totalArticles = 1;
    file.groups = post.groups;
    const qint64 fileId = store.upsertFile(file, &err);
    QVERIFY2(fileId > 0, qPrintable(err));

    QVERIFY2(store.markArticlePosted(fileId, 1, QStringLiteral("msg@ngpost"),
                                     0, 42, &err),
             qPrintable(err));

    PostHistoryStore::PostDetails details;
    QVERIFY2(store.loadPostDetails(postId, &details, &err), qPrintable(err));
    const QList<PostHistoryStore::ArticleSummary> articles =
        details.articlesByFile.value(fileId);
    QCOMPARE(articles.size(), 1);
    QCOMPARE(articles.first().status, QStringLiteral("posted"));
    QCOMPARE(articles.first().msgId, QStringLiteral("msg@ngpost"));
    QCOMPARE(articles.first().bytes, 42);
}

void TestPostHistory::apply_article_events_batches_ordered_status_changes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString err;
    QVERIFY2(store.initialize(&err), qPrintable(err));

    const QString sourcePath = dir.filePath(QStringLiteral("payload.bin"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write(QByteArray(8, 'x'));
    source.close();

    const QFileInfo sourceInfo(sourcePath);
    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("batch.nzb");
    post.nzbPath = dir.filePath(QStringLiteral("batch.nzb"));
    post.from = QStringLiteral("poster@example.invalid");
    post.groups = { QStringLiteral("alt.binaries.test") };
    const qint64 postId = store.createPost(post, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    PostHistoryStore::FileRecord file;
    file.postId = postId;
    file.ordinal = 1;
    file.originalPath = sourceInfo.absoluteFilePath();
    file.postedName = sourceInfo.fileName();
    file.sizeBytes = sourceInfo.size();
    file.mtimeEpoch = sourceInfo.lastModified().toSecsSinceEpoch();
    file.totalArticles = 2;
    file.groups = post.groups;
    const qint64 fileId = store.upsertFile(file, &err);
    QVERIFY2(fileId > 0, qPrintable(err));

    QList<PostHistoryStore::ArticleEvent> events;
    PostHistoryStore::ArticleEvent e;
    e.kind = PostHistoryStore::ArticleEvent::Kind::Posting;
    e.fileId = fileId;
    e.part = 1;
    e.pos = 0;
    e.bytes = 4;
    e.attemptNo = 1;
    e.msgId = QStringLiteral("one@ngpost");
    events << e;
    e.kind = PostHistoryStore::ArticleEvent::Kind::Posted;
    events << e;

    e.kind = PostHistoryStore::ArticleEvent::Kind::Posting;
    e.part = 2;
    e.pos = 4;
    e.msgId = QStringLiteral("two@ngpost");
    events << e;
    e.kind = PostHistoryStore::ArticleEvent::Kind::Failed;
    e.error = QStringLiteral("server rejected");
    events << e;

    QVERIFY2(store.applyArticleEvents(events, &err), qPrintable(err));

    PostHistoryStore::PostDetails details;
    QVERIFY2(store.loadPostDetails(postId, &details, &err), qPrintable(err));
    const QList<PostHistoryStore::ArticleSummary> articles =
        details.articlesByFile.value(fileId);
    QCOMPARE(articles.size(), 2);
    QCOMPARE(articles.at(0).status, QStringLiteral("posted"));
    QCOMPARE(articles.at(0).bytes, 4);
    QCOMPARE(articles.at(1).status, QStringLiteral("failed"));
    QCOMPARE(articles.at(1).msgId, QStringLiteral("two@ngpost"));

    ResumePlanner planner(&store);
    const ResumePlanner::Decision d = planner.check(postId, &err);
    QCOMPARE(d.state, ResumePlanner::ResumeState::PartiallyResumable);
    QCOMPARE(d.postedArticles, 1);
    QCOMPARE(d.failedArticles, 1);
}

void TestPostHistory::terminal_status_purges_attempt_audit_rows()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString dbPath = dir.filePath(QStringLiteral("history.sqlite"));
    PostHistoryStore store(dbPath, true);
    QString err;
    QVERIFY2(store.initialize(&err), qPrintable(err));

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("purge.nzb");
    post.nzbPath = dir.filePath(QStringLiteral("purge.nzb"));
    post.from = QStringLiteral("poster@example.invalid");
    post.groups = { QStringLiteral("alt.binaries.test") };
    const qint64 postId = store.createPost(post, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    PostHistoryStore::FileRecord file;
    file.postId = postId;
    file.ordinal = 1;
    file.postedName = QStringLiteral("payload.bin");
    file.sizeBytes = 8;
    file.totalArticles = 2;
    file.groups = post.groups;
    const qint64 fileId = store.upsertFile(file, &err);
    QVERIFY2(fileId > 0, qPrintable(err));

    // Each posting attempt writes an audit row in post_article_attempts.
    for (int part = 1; part <= 2; ++part) {
        const QString msgId = QStringLiteral("a%1@ngpost").arg(part);
        QVERIFY2(store.markArticlePosting(fileId, part, msgId, 1, &err), qPrintable(err));
        QVERIFY2(store.markArticlePosted(fileId, part, msgId, &err), qPrintable(err));
        store.updateArticlePayload(fileId, part, (part - 1) * 4, 4, &err);
    }
    QCOMPARE(countAttemptRows(dbPath, postId), 2);

    // Reaching a terminal status drops the now-useless audit rows...
    QVERIFY2(store.updatePostStatus(postId, QStringLiteral("success"), 1, 2, 0, 8,
                                    QStringLiteral("1 KB/s"), &err),
             qPrintable(err));
    QCOMPARE(countAttemptRows(dbPath, postId), 0);

    // ...while post_articles stay intact and the NZB still regenerates fully.
    PostHistoryStore::PostDetails details;
    QVERIFY2(store.loadPostDetails(postId, &details, &err), qPrintable(err));
    QCOMPARE(details.articlesByFile.value(fileId).size(), 2);

    NzbHistoryRegenerator regenerator(&store);
    QString nzb;
    QTextStream stream(&nzb);
    QVERIFY2(regenerator.writeNzb(postId, stream, false, nullptr, &err), qPrintable(err));
    QCOMPARE(countNzbSegments(nzb), 2);
    QVERIFY(nzb.contains(QStringLiteral("a1@ngpost")));
    QVERIFY(nzb.contains(QStringLiteral("a2@ngpost")));
}

void TestPostHistory::list_posts_paginates_with_stable_order()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString err;
    QVERIFY2(store.initialize(&err), qPrintable(err));

    QList<qint64> ids;
    for (int i = 0; i < 405; ++i) {
        const qint64 id = createStoredPost(store,
                                           QStringLiteral("bulk-%1.nzb").arg(i, 3, 10, QLatin1Char('0')),
                                           QStringLiteral("success"),
                                           { QStringLiteral("alt.binaries.bulk") },
                                           &err);
        QVERIFY2(id > 0, qPrintable(err));
        ids << id;
    }

    PostHistoryStore::ListFilter filter;
    filter.limit = 200;

    QList<PostHistoryStore::PostSummary> page1 = store.listPosts(filter, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(page1.size(), 200);
    QCOMPARE(page1.first().id, ids.last());
    QCOMPARE(page1.last().id, ids.at(205));
    QVERIFY2(store.hasPostsAfter(filter, &err), qPrintable(err));

    filter.offset = 200;
    QList<PostHistoryStore::PostSummary> page2 = store.listPosts(filter, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(page2.size(), 200);
    QCOMPARE(page2.first().id, ids.at(204));
    QCOMPARE(page2.last().id, ids.at(5));
    QVERIFY2(store.hasPostsAfter(filter, &err), qPrintable(err));

    filter.offset = 400;
    QList<PostHistoryStore::PostSummary> page3 = store.listPosts(filter, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(page3.size(), 5);
    QCOMPARE(page3.first().id, ids.at(4));
    QCOMPARE(page3.last().id, ids.first());
    QVERIFY2(!store.hasPostsAfter(filter, &err), qPrintable(err));

    QSet<qint64> seen;
    for (const QList<PostHistoryStore::PostSummary> &page : { page1, page2, page3 }) {
        for (const PostHistoryStore::PostSummary &summary : page) {
            QVERIFY2(!seen.contains(summary.id), "duplicate post across history pages");
            seen.insert(summary.id);
        }
    }
    QCOMPARE(seen.size(), ids.size());
}

void TestPostHistory::list_posts_applies_filters_before_pagination()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString dbPath = dir.filePath(QStringLiteral("history.sqlite"));
    PostHistoryStore store(dbPath, true);
    QString err;
    QVERIFY2(store.initialize(&err), qPrintable(err));

    QList<qint64> filteredIds;
    for (int i = 0; i < 260; ++i) {
        const qint64 id = createStoredPost(store,
                                           QStringLiteral("keep-%1.nzb").arg(i, 3, 10, QLatin1Char('0')),
                                           QStringLiteral("success"),
                                           { QStringLiteral("alt.keep") },
                                           &err);
        QVERIFY2(id > 0, qPrintable(err));
        filteredIds << id;
    }
    for (int i = 0; i < 50; ++i) {
        QVERIFY2(createStoredPost(store,
                                  QStringLiteral("keep-failed-%1.nzb").arg(i, 2, 10, QLatin1Char('0')),
                                  QStringLiteral("failed"),
                                  { QStringLiteral("alt.keep") },
                                  &err) > 0,
                 qPrintable(err));
        QVERIFY2(createStoredPost(store,
                                  QStringLiteral("keep-other-%1.nzb").arg(i, 2, 10, QLatin1Char('0')),
                                  QStringLiteral("success"),
                                  { QStringLiteral("alt.other") },
                                  &err) > 0,
                 qPrintable(err));
    }

    const QList<qint64> oldIds = filteredIds.mid(0, 10);
    QVERIFY2(updateCreatedAt(dbPath, oldIds, QStringLiteral("2001-01-01T00:00:00"), &err),
             qPrintable(err));

    PostHistoryStore::ListFilter filter;
    filter.status = QStringLiteral("success");
    filter.search = QStringLiteral("keep-");
    filter.group = QStringLiteral("alt.keep");
    filter.dateFrom = QStringLiteral("2002-01-01");
    filter.limit = 200;

    QList<PostHistoryStore::PostSummary> page1 = store.listPosts(filter, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(page1.size(), 200);
    QVERIFY2(store.hasPostsAfter(filter, &err), qPrintable(err));
    for (const PostHistoryStore::PostSummary &summary : page1) {
        QCOMPARE(summary.status, QStringLiteral("success"));
        QCOMPARE(summary.groups, QStringLiteral("alt.keep"));
        QVERIFY(summary.nzbName.startsWith(QStringLiteral("keep-")));
        QVERIFY(summary.createdAt >= filter.dateFrom);
    }

    filter.offset = 200;
    QList<PostHistoryStore::PostSummary> page2 = store.listPosts(filter, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(page2.size(), 50);
    QVERIFY2(!store.hasPostsAfter(filter, &err), qPrintable(err));
    for (const PostHistoryStore::PostSummary &summary : page2) {
        QCOMPARE(summary.status, QStringLiteral("success"));
        QCOMPARE(summary.groups, QStringLiteral("alt.keep"));
        QVERIFY(summary.nzbName.startsWith(QStringLiteral("keep-")));
        QVERIFY(summary.createdAt >= filter.dateFrom);
    }
}

void TestPostHistory::service_flushes_batches_and_returns_snapshots()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PostHistoryService service(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString err;
    QVERIFY2(service.initialize(&err), qPrintable(err));

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("service.nzb");
    post.nzbPath = dir.filePath(QStringLiteral("service.nzb"));
    post.from = QStringLiteral("poster@example.invalid");
    post.groups = { QStringLiteral("alt.binaries.test") };
    const qint64 postId = service.createPost(post, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    QFile payload(dir.filePath(QStringLiteral("payload.bin")));
    QVERIFY(payload.open(QIODevice::WriteOnly));
    payload.write(QByteArray(8, 'x'));
    payload.close();

    PostHistoryStore::FileRecord file;
    file.postId = postId;
    file.ordinal = 1;
    file.originalPath = dir.filePath(QStringLiteral("payload.bin"));
    file.postedName = QStringLiteral("payload.bin");
    file.sizeBytes = 8;
    file.totalArticles = 2;
    file.groups = post.groups;
    const qint64 fileId = service.upsertFile(file, &err);
    QVERIFY2(fileId > 0, qPrintable(err));

    service.enqueueArticlePosting(fileId, 1, QStringLiteral("one@ngpost"), 1, 0, 4);
    service.enqueueArticlePosted(fileId, 1, QStringLiteral("one@ngpost"), 0, 4);
    service.enqueueArticlePosting(fileId, 2, QStringLiteral("two@ngpost"), 1, 4, 4);
    service.enqueueArticleFailed(fileId, 2, QStringLiteral("two@ngpost"), QStringLiteral("nope"), 4, 4);
    QVERIFY2(service.flush(&err), qPrintable(err));

    PostHistoryStore::PostDetails details;
    QVERIFY2(service.loadPostDetails(postId, &details, &err), qPrintable(err));
    QCOMPARE(details.articlesByFile.value(fileId).at(0).status, QStringLiteral("posted"));
    QCOMPARE(details.articlesByFile.value(fileId).at(1).status, QStringLiteral("failed"));

    QEventLoop loop;
    bool called = false;
    PostHistoryService::HistorySnapshot snapshot;
    service.requestHistorySnapshot(PostHistoryStore::ListFilter(), QSet<qint64>(), &loop,
                                   [&](const PostHistoryService::HistorySnapshot &s) {
        snapshot = s;
        called = true;
        loop.quit();
    });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();

    QVERIFY(called);
    QVERIFY2(snapshot.error.isEmpty(), qPrintable(snapshot.error));
    QCOMPARE(snapshot.posts.size(), 1);
    QCOMPARE(snapshot.resumeRows.size(), 1);
    QCOMPARE(snapshot.resumeRows.first().failedArticles, 1);
}

void TestPostHistory::service_resume_queries_fail_closed_when_flush_fails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString dbPath = dir.filePath(QStringLiteral("history.sqlite"));
    PostHistoryService service(dbPath, true);
    QString err;
    QVERIFY2(service.initialize(&err), qPrintable(err));

    const QString sourcePath = dir.filePath(QStringLiteral("payload.bin"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(QByteArray(4, 'x')), qint64(4));
    source.close();

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("flush-failure.nzb");
    post.nzbPath = dir.filePath(post.nzbName);
    post.from = QStringLiteral("poster@example.invalid");
    post.groups = { QStringLiteral("alt.binaries.test") };
    PostHistoryStore::PostInfo info;
    info.articleSizeBytes = 4;
    const qint64 postId = service.createPost(post, info, {}, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    const QFileInfo sourceInfo(sourcePath);
    PostHistoryStore::FileRecord file;
    file.postId = postId;
    file.ordinal = 1;
    file.originalPath = sourceInfo.absoluteFilePath();
    file.postedName = sourceInfo.fileName();
    file.sizeBytes = sourceInfo.size();
    file.mtimeEpoch = sourceInfo.lastModified().toSecsSinceEpoch();
    file.totalArticles = 1;
    file.groups = post.groups;
    const qint64 fileId = service.upsertFile(file, &err);
    QVERIFY2(fileId > 0, qPrintable(err));

    PostHistoryService::ResumeRow baseline;
    QVERIFY2(service.checkResume(postId, &baseline, &err), qPrintable(err));
    QCOMPARE(baseline.state, QStringLiteral("resumable"));

    // Break a table used by the batched writer, then queue a valid event. The
    // service keeps that event pending after the failed transaction, which
    // makes every resume-facing read prove that it propagates the flush
    // failure rather than consulting the older, superficially resumable
    // database state.
    QString schemaErr;
    QVERIFY2(execRawSql(dbPath,
                        QStringLiteral("DROP TABLE post_article_attempts"),
                        {},
                        &schemaErr),
             qPrintable(schemaErr));
    service.enqueueArticlePosting(fileId,
                                  1,
                                  QStringLiteral("pending@ngpost"),
                                  1,
                                  0,
                                  4);

    PostHistoryService::ResumeRow decision;
    decision.state = QStringLiteral("stale-sentinel");
    err.clear();
    QVERIFY(!service.checkResume(postId, &decision, &err));
    QVERIFY2(!err.isEmpty(), "the flush failure was not propagated");
    QCOMPARE(decision.state, QStringLiteral("not_resumable"));
    QCOMPARE(decision.reason, err);
    QCOMPARE(decision.postedArticles, 0);
    QCOMPARE(decision.pendingArticles, 0);

    PostHistoryStore::PostDetails details;
    err.clear();
    QVERIFY(!service.loadPostDetails(postId, &details, &err));
    QVERIFY2(!err.isEmpty(), "loadPostDetails hid the flush failure");

    err.clear();
    const QList<PostHistoryStore::PostSummary> candidates = service.resumeCandidates(&err);
    QVERIFY(candidates.isEmpty());
    QVERIFY2(!err.isEmpty(), "resumeCandidates hid the flush failure");

    QEventLoop loop;
    bool called = false;
    PostHistoryService::HistorySnapshot snapshot;
    service.requestHistorySnapshot(PostHistoryStore::ListFilter(),
                                   QSet<qint64>(),
                                   &loop,
                                   [&](const PostHistoryService::HistorySnapshot &result) {
        snapshot = result;
        called = true;
        loop.quit();
    });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();

    QVERIFY(called);
    QVERIFY2(!snapshot.error.isEmpty(), "the history snapshot hid the flush failure");
    QVERIFY(snapshot.posts.isEmpty());
    QVERIFY(snapshot.resumeRows.isEmpty());
}

void TestPostHistory::service_snapshot_paginates_history_but_stats_are_complete()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString dbPath = dir.filePath(QStringLiteral("history.sqlite"));
    QString err;
    {
        PostHistoryStore store(dbPath, true);
        QVERIFY2(store.initialize(&err), qPrintable(err));
        for (int i = 0; i < 205; ++i) {
            const qint64 id = createStoredPost(store,
                                               QStringLiteral("stats-%1.nzb").arg(i, 3, 10, QLatin1Char('0')),
                                               QStringLiteral("success"),
                                               { QStringLiteral("alt.stats") },
                                               &err);
            QVERIFY2(id > 0, qPrintable(err));
        }
        store.closeConnection();
    }

    PostHistoryService service(dbPath, true);
    QVERIFY2(service.initialize(&err), qPrintable(err));

    PostHistoryStore::ListFilter pageFilter;
    pageFilter.limit = 200;

    QEventLoop pageLoop1;
    bool page1Called = false;
    PostHistoryService::HistorySnapshot page1;
    service.requestHistorySnapshot(pageFilter, QSet<qint64>(), &pageLoop1,
                                   [&](const PostHistoryService::HistorySnapshot &snapshot) {
        page1 = snapshot;
        page1Called = true;
        pageLoop1.quit();
    });
    QTimer::singleShot(3000, &pageLoop1, &QEventLoop::quit);
    pageLoop1.exec();

    QVERIFY(page1Called);
    QVERIFY2(page1.error.isEmpty(), qPrintable(page1.error));
    QCOMPARE(page1.posts.size(), 200);
    QCOMPARE(page1.pageOffset, 0);
    QCOMPARE(page1.pageLimit, 200);
    QVERIFY(!page1.hasPreviousPage);
    QVERIFY(page1.hasNextPage);

    pageFilter.offset = 200;
    QEventLoop pageLoop2;
    bool page2Called = false;
    PostHistoryService::HistorySnapshot page2;
    service.requestHistorySnapshot(pageFilter, QSet<qint64>(), &pageLoop2,
                                   [&](const PostHistoryService::HistorySnapshot &snapshot) {
        page2 = snapshot;
        page2Called = true;
        pageLoop2.quit();
    });
    QTimer::singleShot(3000, &pageLoop2, &QEventLoop::quit);
    pageLoop2.exec();

    QVERIFY(page2Called);
    QVERIFY2(page2.error.isEmpty(), qPrintable(page2.error));
    QCOMPARE(page2.posts.size(), 5);
    QCOMPARE(page2.pageOffset, 200);
    QCOMPARE(page2.pageLimit, 200);
    QVERIFY(page2.hasPreviousPage);
    QVERIFY(!page2.hasNextPage);

    QEventLoop statsLoop;
    bool statsCalled = false;
    PostHistoryService::StatsSnapshot stats;
    service.requestStatsSnapshot(QString(), QString(), QString(), &statsLoop,
                                 [&](const PostHistoryService::StatsSnapshot &snapshot) {
        stats = snapshot;
        statsCalled = true;
        statsLoop.quit();
    });
    QTimer::singleShot(3000, &statsLoop, &QEventLoop::quit);
    statsLoop.exec();

    QVERIFY(statsCalled);
    QVERIFY2(stats.error.isEmpty(), qPrintable(stats.error));
    int totalPosts = 0;
    for (const PostHistoryStore::DayStats &day : stats.days)
        totalPosts += day.nbPosts;
    QCOMPARE(totalPosts, 205);

    bool foundStatsGroup = false;
    for (const PostHistoryStore::GroupStats &group : stats.groupStats) {
        if (group.group == QStringLiteral("alt.stats")) {
            foundStatsGroup = true;
            QCOMPARE(group.nbPosts, 205);
        }
    }
    QVERIFY(foundStatsGroup);
    QCOMPARE(stats.topPosts.size(), 20);
}

void TestPostHistory::schema_migrates_v1_database_without_losing_posts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath("history.sqlite");

    qint64 postId = 0;
    {
        PostHistoryStore store(dbPath, true);
        QString error;
        QVERIFY2(store.initialize(&error), qPrintable(error));
        postId =
            createStoredPost(store, QStringLiteral("old.nzb"), QStringLiteral("success"), {}, &error);
        QVERIFY(postId > 0);
    }

    // forge a v1 database: the new tables did not exist back then
    QVERIFY(rawSql(dbPath, QStringLiteral("DROP TABLE post_info")).isNull());
    QVERIFY(rawSql(dbPath, QStringLiteral("DROP TABLE post_meta")).isNull());
    QVERIFY(rawSql(dbPath,
                   QStringLiteral("INSERT OR REPLACE INTO schema_meta(key, value) "
                                  "VALUES('version', '1')"))
                .isNull());

    {
        PostHistoryStore store(dbPath, true);
        QString error;
        QVERIFY2(store.initialize(&error), qPrintable(error));

        // the tables are back, the version is recorded, and the post survived
        QCOMPARE(rawSql(dbPath,
                        QStringLiteral("SELECT value FROM schema_meta WHERE key='version'"))
                     .toInt(),
                 PostHistoryStore::kSchemaVersion);
        QCOMPARE(rawSql(dbPath, QStringLiteral("SELECT COUNT(*) FROM posts")).toInt(), 1);

        // a post from before the migration has no facts: they are missing, not invented
        PostHistoryStore::PostInfoRecord record;
        QVERIFY2(store.loadPostInfoRecord(postId, &record, &error), qPrintable(error));
        QVERIFY(record.partial);
        QCOMPARE(record.info.par2Pct, -1);
        QCOMPARE(record.info.postSizeBytes, static_cast<qint64>(-1));
        // stays unknown all the way to the sheet, where it renders empty
        QCOMPARE(record.toPostInfoData().postSizeBytes, static_cast<qint64>(-1));
    }
}

void TestPostHistory::schema_migrates_v2_and_derives_article_size_safely()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("history.sqlite"));

    qint64 provenPostId = 0;
    qint64 ambiguousPostId = 0;
    {
        PostHistoryStore store(dbPath, true);
        QString error;
        QVERIFY2(store.initialize(&error), qPrintable(error));

        auto addPost = [&](const QString &name, bool withPayloadEvidence) {
            PostHistoryStore::PostRecord post;
            post.nzbName = name;
            post.nzbPath = dir.filePath(name);
            post.from = QStringLiteral("poster@example.invalid");
            post.groups = { QStringLiteral("alt.binaries.test") };

            PostHistoryStore::PostInfo info;
            info.articleSizeBytes = 4;
            const qint64 postId = store.createPost(post, info, {}, &error);
            if (!postId)
                return static_cast<qint64>(0);

            PostHistoryStore::FileRecord file;
            file.postId = postId;
            file.ordinal = 1;
            file.originalPath = dir.filePath(name + QStringLiteral(".bin"));
            file.postedName = name + QStringLiteral(".bin");
            file.sizeBytes = 10;
            file.totalArticles = 3;
            const qint64 fileId = store.upsertFile(file, &error);
            if (!fileId)
                return static_cast<qint64>(0);

            if (withPayloadEvidence) {
                for (int part = 1; part <= 3; ++part) {
                    PostHistoryStore::ArticleRecord article;
                    article.fileId = fileId;
                    article.part = part;
                    article.pos = (part - 1) * 4;
                    article.bytes = part < 3 ? 4 : 2;
                    if (!store.upsertArticle(article, &error))
                        return static_cast<qint64>(0);
                }
            }
            return postId;
        };

        provenPostId = addPost(QStringLiteral("proven.nzb"), true);
        QVERIFY2(provenPostId > 0, qPrintable(error));
        ambiguousPostId = addPost(QStringLiteral("ambiguous.nzb"), false);
        QVERIFY2(ambiguousPostId > 0, qPrintable(error));
    }

    // Rebuild only post_info with its exact v2 shape, then advertise v2.
    // This does not depend on SQLite's newer DROP COLUMN support.
    QString sqlError;
    QVERIFY2(execRawSql(dbPath,
                        QStringLiteral("CREATE TABLE post_info_v2 ("
                                       "post_id INTEGER PRIMARY KEY, par2_pct INTEGER, "
                                       "post_size_bytes INTEGER, active_seconds INTEGER, "
                                       "source_path TEXT, original_name TEXT, app_version TEXT, "
                                       "FOREIGN KEY(post_id) REFERENCES posts(id) ON DELETE CASCADE)"),
                        {}, &sqlError), qPrintable(sqlError));
    QVERIFY2(execRawSql(dbPath,
                        QStringLiteral("INSERT INTO post_info_v2 "
                                       "SELECT post_id, par2_pct, post_size_bytes, active_seconds, "
                                       "source_path, original_name, app_version FROM post_info"),
                        {}, &sqlError), qPrintable(sqlError));
    QVERIFY2(execRawSql(dbPath, QStringLiteral("DROP TABLE post_info"), {}, &sqlError),
             qPrintable(sqlError));
    QVERIFY2(execRawSql(dbPath,
                        QStringLiteral("ALTER TABLE post_info_v2 RENAME TO post_info"),
                        {}, &sqlError), qPrintable(sqlError));
    QVERIFY2(execRawSql(dbPath,
                        QStringLiteral("INSERT OR REPLACE INTO schema_meta(key, value) "
                                       "VALUES('version', '2')"),
                        {}, &sqlError), qPrintable(sqlError));

    PostHistoryStore store(dbPath, true);
    QString error;
    QVERIFY2(store.initialize(&error), qPrintable(error));
    QCOMPARE(rawSql(dbPath, QStringLiteral("SELECT value FROM schema_meta WHERE key='version'"))
                 .toInt(),
             PostHistoryStore::kSchemaVersion);
    QCOMPARE(rawSql(dbPath,
                    QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('post_info') "
                                   "WHERE name='article_size_bytes'"))
                 .toInt(),
             1);

    PostHistoryStore::PostDetails details;
    QVERIFY2(store.loadPostDetails(provenPostId, &details, &error), qPrintable(error));
    QCOMPARE(details.articleSizeBytes, static_cast<qint64>(4));
    QVERIFY2(store.loadPostDetails(ambiguousPostId, &details, &error), qPrintable(error));
    QCOMPARE(details.articleSizeBytes, static_cast<qint64>(-1));
}

void TestPostHistory::schema_refuses_a_newer_database()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath("history.sqlite");

    {
        PostHistoryStore store(dbPath, true);
        QString error;
        QVERIFY2(store.initialize(&error), qPrintable(error));
    }

    rawSql(dbPath,
           QStringLiteral("INSERT OR REPLACE INTO schema_meta(key, value) VALUES('version', ?)"),
           { QString::number(PostHistoryStore::kSchemaVersion + 1) });

    PostHistoryStore store(dbPath, true);
    QString error;
    QVERIFY(!store.initialize(&error));
    QVERIFY2(error.contains(QStringLiteral("newer ngPost")), qPrintable(error));
}

void TestPostHistory::schema_refuses_an_invalid_version()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("history.sqlite"));

    {
        PostHistoryStore store(dbPath, true);
        QString error;
        QVERIFY2(store.initialize(&error), qPrintable(error));
    }

    QString sqlError;
    QVERIFY2(execRawSql(dbPath,
                        QStringLiteral("INSERT OR REPLACE INTO schema_meta(key, value) "
                                       "VALUES('version', 'not-a-version')"),
                        {}, &sqlError), qPrintable(sqlError));

    PostHistoryStore store(dbPath, true);
    QString error;
    QVERIFY(!store.initialize(&error));
    QVERIFY2(error.contains(QStringLiteral("invalid history schema version")), qPrintable(error));
}

void TestPostHistory::started_at_is_set_by_the_transfer_and_never_rewritten()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PostHistoryStore store(dir.filePath("history.sqlite"), true);
    QString error;
    QVERIFY2(store.initialize(&error), qPrintable(error));

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("queued.nzb");
    PostHistoryStore::PostInfo info;
    info.sourcePath = dir.filePath(QStringLiteral("queued.bin"));
    const qint64 postId = store.createPost(post, info, {}, &error);
    QVERIFY2(postId > 0, qPrintable(error));

    // created, but not started: the job may sit in the queue for a long while
    PostHistoryStore::PostInfoRecord record;
    QVERIFY2(store.loadPostInfoRecord(postId, &record, &error), qPrintable(error));
    QVERIFY(record.startedAt.isEmpty());
    QVERIFY(!record.toPostInfoData().startedAt.isValid());

    QVERIFY(store.markPostStarted(postId, &error));
    QVERIFY2(store.loadPostInfoRecord(postId, &record, &error), qPrintable(error));
    const QString firstStart = record.startedAt;
    QVERIFY(!firstStart.isEmpty());

    // a resume keeps the date of the first attempt
    QVERIFY(store.markPostStarted(postId, &error));
    QVERIFY2(store.loadPostInfoRecord(postId, &record, &error), qPrintable(error));
    QCOMPARE(record.startedAt, firstStart);
}

void TestPostHistory::resume_marker_preserves_previous_terminal_facts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString error;

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("resume.nzb");
    post.nzbPath = dir.filePath(post.nzbName);
    PostHistoryStore::PostInfo info;
    info.sourcePath = dir.filePath(QStringLiteral("payload.bin"));
    const qint64 postId = store.createPost(post, info, {}, &error);
    QVERIFY2(postId > 0, qPrintable(error));

    QVERIFY2(store.updatePostStatus(postId,
                                    QStringLiteral("partial"),
                                    0,
                                    0,
                                    0,
                                    0,
                                    QStringLiteral("12.3 MB/s"),
                                    &error),
             qPrintable(error));
    PostHistoryStore::PostInfoRecord before;
    QVERIFY2(store.loadPostInfoRecord(postId, &before, &error), qPrintable(error));
    QVERIFY(!before.post.finishedAt.isEmpty());

    QVERIFY2(store.markPostResuming(postId, &error), qPrintable(error));
    PostHistoryStore::PostInfoRecord during;
    QVERIFY2(store.loadPostInfoRecord(postId, &during, &error), qPrintable(error));
    QCOMPARE(during.post.status, QStringLiteral("posting"));
    QCOMPARE(during.post.finishedAt, before.post.finishedAt);
    QCOMPARE(during.post.avgSpeed, before.post.avgSpeed);
}

void TestPostHistory::post_size_is_written_once_and_active_seconds_accumulate()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PostHistoryStore store(dir.filePath("history.sqlite"), true);
    QString error;
    QVERIFY2(store.initialize(&error), qPrintable(error));

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("big.nzb");
    post.nzbPath = dir.filePath("big.nzb");
    PostHistoryStore::PostInfo info;
    info.par2Pct = 8;
    info.sourcePath = QStringLiteral("/data/backup/Photos-2026.tar");
    info.originalName = QStringLiteral("Photos-2026.tar");
    const qint64 postId = store.createPost(post, info, {}, &error);
    QVERIFY2(postId > 0, qPrintable(error));

    QVERIFY(store.setPostSizeIfUnset(postId, 2505484398LL, &error));
    // a resume only sees the leftovers: it must not shrink the whole post
    QVERIFY(store.setPostSizeIfUnset(postId, 42, &error));

    QVERIFY(store.addActiveSeconds(postId, 200, &error));
    QVERIFY(store.addActiveSeconds(postId, 50, &error));

    PostHistoryStore::PostInfoRecord record;
    QVERIFY2(store.loadPostInfoRecord(postId, &record, &error), qPrintable(error));
    QVERIFY(!record.partial);
    QCOMPARE(record.info.postSizeBytes, 2505484398LL);
    QCOMPARE(record.info.activeSeconds, static_cast<qint64>(250));
    QCOMPARE(record.info.par2Pct, 8);
    QCOMPARE(record.info.originalName, QStringLiteral("Photos-2026.tar"));
}

void TestPostHistory::post_info_counts_only_explicitly_posted_articles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString error;
    QVERIFY2(store.initialize(&error), qPrintable(error));

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("counters.nzb");
    PostHistoryStore::PostInfo info;
    info.sourcePath = dir.filePath(QStringLiteral("source.bin"));
    const qint64 postId = store.createPost(post, info, {}, &error);
    QVERIFY2(postId > 0, qPrintable(error));

    PostHistoryStore::FileRecord file;
    file.postId       = postId;
    file.ordinal      = 1;
    file.originalPath = info.sourcePath;
    file.postedName   = QStringLiteral("source.bin");
    file.totalArticles = 5; // the fifth article has no row yet: pending
    const qint64 fileId = store.upsertFile(file, &error);
    QVERIFY2(fileId > 0, qPrintable(error));

    auto addArticle = [&](int part, const QString &status) {
        PostHistoryStore::ArticleRecord article;
        article.fileId = fileId;
        article.part   = part;
        article.status = status;
        QVERIFY2(store.upsertArticle(article, &error), qPrintable(error));
    };
    addArticle(1, QStringLiteral("posted"));
    addArticle(2, QStringLiteral("posted"));
    addArticle(3, QStringLiteral("failed"));
    addArticle(4, QStringLiteral("unknown"));

    QVERIFY2(store.updatePostStatus(postId,
                                    QStringLiteral("partial"),
                                    0,
                                    0,
                                    0,
                                    0,
                                    QString(),
                                    &error),
             qPrintable(error));

    PostHistoryStore::PostInfoRecord record;
    QVERIFY2(store.loadPostInfoRecord(postId, &record, &error), qPrintable(error));
    QCOMPARE(record.post.nbArticles, 5);
    QCOMPARE(record.post.nbFailedArticles, 2);
    // total - failed would incorrectly report 3 by counting the absent/pending
    // fifth article. Only the two explicit posted rows are successes.
    QCOMPARE(record.nbArticlesPosted, 2);
    QCOMPARE(record.toPostInfoData().nbArticlesPosted, static_cast<uint>(2));
}

void TestPostHistory::post_meta_keeps_scope_and_refuses_the_password_key()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath("history.sqlite");
    PostHistoryStore store(dbPath, true);
    QString error;
    QVERIFY2(store.initialize(&error), qPrintable(error));

    const qint64 postId =
        createStoredPost(store, QStringLiteral("meta.nzb"), QStringLiteral("posting"), {}, &error);
    QVERIFY(postId > 0);

    QMap<QString, MetaValue> meta;
    meta.insert(QStringLiteral("titre"),
                MetaValue(QString::fromUtf8("L'\xC3\x89t\xC3\xA9 & Cie <\"x\">"), MetaScope::Nzb));
    meta.insert(QStringLiteral("portail1"),
                MetaValue(QStringLiteral("https://x.fr/f=326598.html"), MetaScope::Local));
    meta.insert(QStringLiteral("password"), MetaValue(QStringLiteral("leaked"), MetaScope::Nzb));
    QVERIFY2(store.setPostMeta(postId, meta, &error), qPrintable(error));

    PostHistoryStore::PostInfoRecord record;
    QVERIFY2(store.loadPostInfoRecord(postId, &record, &error), qPrintable(error));

    QCOMPARE(record.meta.size(), 2); // the password was refused
    QVERIFY(!record.meta.contains(QStringLiteral("password")));
    QCOMPARE(record.meta.value(QStringLiteral("titre")).value,
             QString::fromUtf8("L'\xC3\x89t\xC3\xA9 & Cie <\"x\">"));
    QCOMPARE(record.meta.value(QStringLiteral("titre")).scope, MetaScope::Nzb);
    QCOMPARE(record.meta.value(QStringLiteral("portail1")).scope, MetaScope::Local);
    // the URL keeps its '=' sign
    QVERIFY(record.meta.value(QStringLiteral("portail1")).value.contains(QStringLiteral("=326598")));

    // deleting the post takes its metadata with it
    QVERIFY(store.deletePost(postId, &error));
    QCOMPARE(rawSql(dbPath, QStringLiteral("SELECT COUNT(*) FROM post_meta")).toInt(), 0);
}

void TestPostHistory::regenerated_nzb_publishes_only_nzb_scoped_meta()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PostHistoryStore store(dir.filePath("history.sqlite"), true);
    QString error;
    QVERIFY2(store.initialize(&error), qPrintable(error));

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("scoped.nzb");
    post.nzbPath = dir.filePath("scoped.nzb");
    post.rarPass = QStringLiteral("s3cr3t");
    post.hasPassword = true;
    QMap<QString, MetaValue> meta;
    meta.insert(QStringLiteral("titre"), MetaValue(QStringLiteral("Public"), MetaScope::Nzb));
    meta.insert(QStringLiteral("portail1"), MetaValue(QStringLiteral("Private"), MetaScope::Local));
    const qint64 postId = store.createPost(post, PostHistoryStore::PostInfo(), meta, &error);
    QVERIFY2(postId > 0, qPrintable(error));

    PostHistoryStore::FileRecord file;
    file.postId = postId;
    file.ordinal = 1;
    file.postedName = QStringLiteral("payload.bin");
    file.sizeBytes = 4;
    file.totalArticles = 1;
    const qint64 fileId = store.upsertFile(file, &error);
    QVERIFY2(fileId > 0, qPrintable(error));
    QVERIFY2(store.markArticlePosted(fileId, 1, QStringLiteral("meta@ngpost"), &error),
             qPrintable(error));
    QVERIFY2(store.updatePostStatus(postId,
                                    QStringLiteral("success"),
                                    1, 1, 0, 4,
                                    QStringLiteral("1 KB/s"),
                                    &error),
             qPrintable(error));

    NzbHistoryRegenerator regenerator(&store);

    QString nzb;
    {
        QTextStream stream(&nzb);
        QStringList warnings;
        QVERIFY2(regenerator.writeNzb(postId, stream, false, &warnings, &error), qPrintable(error));
    }
    QVERIFY(nzb.contains(QStringLiteral("<meta type=\"titre\">Public</meta>")));
    QVERIFY(!nzb.contains(QStringLiteral("Private"))); // never leaves the machine
    QVERIFY(!nzb.contains(QStringLiteral("s3cr3t")));  // not asked for

    QString withPass;
    {
        QTextStream stream(&withPass);
        QStringList warnings;
        QVERIFY2(regenerator.writeNzb(postId, stream, true, &warnings, &error), qPrintable(error));
    }
    QCOMPARE(withPass.count(QStringLiteral("<meta type=\"password\">")), 1);
    QVERIFY(withPass.contains(QStringLiteral("s3cr3t")));
    QVERIFY(!withPass.contains(QStringLiteral("Private")));
}

void TestPostHistory::resume_options_never_replay_compression_or_par2()
{
    PostHistoryStore::PostDetails details;
    details.post.id = 12;
    details.rarName = QStringLiteral("obfuscated");
    details.rarPass = QStringLiteral("s3cr3t");
    details.doCompress = true; // the original post did compress...
    details.doPar2 = true;     // ...and did generate par2

    // globals currently ask for compression and par2
    PostingJobOptions base;
    base.doCompress = true;
    base.doPar2 = true;
    base.par2Pct = 15;
    base.delFilesAfterPost = true;

    ResumePlanner::JobPlan plan;
    const PostingJobOptions resumed = ResumePlanner::jobOptions(
        base, details, QStringLiteral("/tmp/out.nzb"), QList<QString>(), plan, std::string("me@x.y"));

    // orders are off: rar and par2 must not run again on the leftovers
    QVERIFY(!resumed.doCompress);
    QVERIFY(!resumed.doPar2);
    // and the sources of a resumed post are never deleted
    QVERIFY(!resumed.delFilesAfterPost);

    // but what the original post did is kept, as a fact
    QVERIFY(resumed.originalDidCompress);
    QVERIFY(resumed.originalDidPar2);
    QCOMPARE(resumed.resumeHistoryPostId, static_cast<qint64>(12));
    QCOMPARE(resumed.rarName, QStringLiteral("obfuscated"));
}

void TestPostHistory::resume_options_take_obfuscation_from_history()
{
    PostHistoryStore::PostDetails details;
    details.post.id = 3;
    details.obfuscateArticles = true;
    details.obfuscateFileName = false;
    details.from = QStringLiteral("original@poster.net");

    PostingJobOptions base; // globals say the opposite of the original post
    base.obfuscateArticles = false;
    base.obfuscateFileName = true;

    const PostingJobOptions resumed =
        ResumePlanner::jobOptions(base,
                                  details,
                                  QStringLiteral("/tmp/out.nzb"),
                                  QList<QString>(),
                                  ResumePlanner::JobPlan(),
                                  std::string("fallback@x.y"));

    QVERIFY(resumed.obfuscateArticles);
    QVERIFY(!resumed.obfuscateFileName);
    QCOMPARE(QString::fromStdString(resumed.from), QStringLiteral("original@poster.net"));

    // no poster recorded: fall back on the current one rather than posting anonymously
    details.from.clear();
    const PostingJobOptions fallback =
        ResumePlanner::jobOptions(base,
                                  details,
                                  QStringLiteral("/tmp/out.nzb"),
                                  QList<QString>(),
                                  ResumePlanner::JobPlan(),
                                  std::string("fallback@x.y"));
    QCOMPARE(QString::fromStdString(fallback.from), QStringLiteral("fallback@x.y"));
}

void TestPostHistory::resume_describes_the_original_par2_percentage()
{
    PostHistoryStore::PostDetails details;
    details.post.id = 7;
    details.doPar2 = true;
    details.par2Pct = 8; // what the original post really used

    PostingJobOptions base;
    base.doPar2 = true;
    base.par2Pct = 15; // the globals moved on since then

    const PostingJobOptions resumed =
        ResumePlanner::jobOptions(base,
                                  details,
                                  QStringLiteral("/tmp/out.nzb"),
                                  QList<QString>(),
                                  ResumePlanner::JobPlan(),
                                  std::string("me@x.y"));

    QCOMPARE(resumed.originalPar2Pct, 8);
    QCOMPARE(resumed.describedPar2Pct(), 8);

    // a post that had no par2 describes none, whatever the globals say
    details.doPar2 = false;
    details.par2Pct = -1;
    const PostingJobOptions noPar2 =
        ResumePlanner::jobOptions(base,
                                  details,
                                  QStringLiteral("/tmp/out.nzb"),
                                  QList<QString>(),
                                  ResumePlanner::JobPlan(),
                                  std::string("me@x.y"));
    QCOMPARE(noPar2.describedPar2Pct(), -1);
}

void TestPostHistory::resume_file_state_detects_a_source_that_changed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("source.bin");
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(1000, 'a'));
    }

    QFileInfo fi(path);
    PostingJobResumeFileState state;
    state.sizeBytes  = fi.size();
    state.mtimeEpoch = fi.lastModified().toSecsSinceEpoch();
    QVERIFY(state.matches(QFileInfo(path)));

    // the file grew while the job waited in the queue
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::Append));
        f.write(QByteArray(10, 'b'));
    }
    QVERIFY(!state.matches(QFileInfo(path)));

    // and a source that vanished is not usable either
    QVERIFY(QFile::remove(path));
    QVERIFY(!state.matches(QFileInfo(path)));
}

void TestPostHistory::resume_options_drop_the_current_metadata_and_password()
{
    PostHistoryStore::PostDetails details;
    details.post.id = 11;
    details.rarPass = QStringLiteral("password-of-that-post");

    // whatever the current run was configured with
    PostingJobOptions base;
    base.meta.insert(QStringLiteral("album"),
                     MetaValue(QStringLiteral("another post"), MetaScope::Nzb));
    base.declaredPassword = QStringLiteral("password-of-another-post");

    ResumePlanner::JobPlan plan;
    const QString path = QDir::tempPath() + QStringLiteral("/tst-resume-source.bin");
    plan.files << QFileInfo(path);

    const PostingJobOptions resumed =
        ResumePlanner::jobOptions(base,
                                  details,
                                  QStringLiteral("/tmp/out.nzb"),
                                  QList<QString>(),
                                  plan,
                                  std::string("me@x.y"));

    QVERIFY(resumed.meta.isEmpty());
    QVERIFY(resumed.declaredPassword.isEmpty());
    // the password of the post being resumed, on the other hand, is kept
    QCOMPARE(resumed.rarPass, QStringLiteral("password-of-that-post"));

    // and the sources of the attempt are known, so nothing can overwrite them
    QCOMPARE(resumed.inputPaths, QStringList{ QFileInfo(path).absoluteFilePath() });
}

void TestPostHistory::resume_recovers_unstored_password_from_existing_nzb()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString nzbPath = dir.filePath(QStringLiteral("protected.nzb"));

    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::WriteOnly | QIODevice::Text));
    const QByteArray contents =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<nzb><head>"
        "<meta type=\"title\">Protected post</meta>"
        "<meta type=\"password\">sword&amp;fish</meta>"
        "</head><file><groups/><segments/></file></nzb>\n";
    QCOMPARE(nzb.write(contents), static_cast<qint64>(contents.size()));
    nzb.close();

    PostHistoryStore::PostDetails details;
    details.post.id = 21;
    details.post.hasPassword = true;
    details.post.passwordStored = false;
    QVERIFY(details.rarPass.isEmpty());

    PostingJobOptions base;
    base.rarPass = QStringLiteral("password-of-another-post");
    base.declaredPassword = QStringLiteral("declared-by-the-current-run");

    const PostingJobOptions resumed =
        ResumePlanner::jobOptions(base,
                                  details,
                                  nzbPath,
                                  QList<QString>(),
                                  ResumePlanner::JobPlan(),
                                  std::string("me@x.y"));

    // QXmlStreamReader decodes entities: the recovered value is the exact
    // password that the original NZB advertised, not its XML representation.
    QCOMPARE(resumed.rarPass, QStringLiteral("sword&fish"));
    QVERIFY(!resumed.resumePasswordUnavailable);
    QVERIFY(resumed.declaredPassword.isEmpty());
}

void TestPostHistory::resume_fails_closed_when_unstored_password_is_absent_from_nzb()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString nzbPath = dir.filePath(QStringLiteral("no-password.nzb"));

    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::WriteOnly | QIODevice::Text));
    const QByteArray contents =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<nzb><head><meta type=\"title\">No password here</meta></head>"
        "<file><groups/><segments/></file></nzb>\n";
    QCOMPARE(nzb.write(contents), static_cast<qint64>(contents.size()));
    nzb.close();

    PostHistoryStore::PostDetails details;
    details.post.id = 22;
    details.post.hasPassword = true;
    details.post.passwordStored = false;

    PostingJobOptions base;
    base.rarPass = QStringLiteral("password-of-another-post");
    base.declaredPassword = QStringLiteral("declared-by-the-current-run");

    const auto optionsFor = [&](const QString &path) {
        return ResumePlanner::jobOptions(base,
                                         details,
                                         path,
                                         QList<QString>(),
                                         ResumePlanner::JobPlan(),
                                         std::string("me@x.y"));
    };

    const PostingJobOptions withoutMeta = optionsFor(nzbPath);
    QVERIFY(withoutMeta.rarPass.isEmpty());
    QVERIFY(withoutMeta.declaredPassword.isEmpty());
    QVERIFY(withoutMeta.resumePasswordUnavailable);

    // A missing NZB has the same fail-closed outcome. In neither case may the
    // current run's unrelated password leak into the resumed post.
    QVERIFY(QFile::remove(nzbPath));
    const PostingJobOptions withoutFile = optionsFor(nzbPath);
    QVERIFY(withoutFile.rarPass.isEmpty());
    QVERIFY(withoutFile.resumePasswordUnavailable);
}

void TestPostHistory::nzb_regeneration_keeps_prior_files_after_resume()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString err;
    QVERIFY2(store.initialize(&err), qPrintable(err));

    const QString firstPath = dir.filePath(QStringLiteral("first.bin"));
    const QString secondPath = dir.filePath(QStringLiteral("second.bin"));
    for (const QString &path : { firstPath, secondPath }) {
        QFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write(QByteArray(8, 'x'));
    }

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("resume.nzb");
    post.nzbPath = dir.filePath(QStringLiteral("resume.nzb"));
    post.from = QStringLiteral("poster@example.invalid");
    post.groups = { QStringLiteral("alt.binaries.test") };
    const qint64 postId = store.createPost(post, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    auto addFile = [&](int ordinal, const QString &path) {
        const QFileInfo sourceInfo(path);
        PostHistoryStore::FileRecord file;
        file.postId = postId;
        file.ordinal = ordinal;
        file.originalPath = sourceInfo.absoluteFilePath();
        file.postedName = sourceInfo.fileName();
        file.sizeBytes = sourceInfo.size();
        file.mtimeEpoch = sourceInfo.lastModified().toSecsSinceEpoch();
        file.totalArticles = 2;
        file.groups = post.groups;
        return store.upsertFile(file, &err);
    };

    const qint64 firstId = addFile(1, firstPath);
    const qint64 secondId = addFile(2, secondPath);
    QVERIFY2(firstId > 0, qPrintable(err));
    QVERIFY2(secondId > 0, qPrintable(err));

    auto addArticle = [&](qint64 fileId, int part, qint64 pos, const QString &msgId,
                          bool posted) {
        PostHistoryStore::ArticleRecord article;
        article.fileId = fileId;
        article.part = part;
        article.pos = pos;
        article.bytes = 4;
        QVERIFY2(store.upsertArticle(article, &err), qPrintable(err));
        QVERIFY2(store.markArticlePosting(fileId, part, msgId, 1, &err), qPrintable(err));
        if (posted)
            QVERIFY2(store.markArticlePosted(fileId, part, msgId, &err), qPrintable(err));
        else
            QVERIFY2(store.markArticleFailed(fileId, part, msgId,
                                             QStringLiteral("server rejected"), &err),
                     qPrintable(err));
    };

    addArticle(firstId, 1, 0, QStringLiteral("first-1@ngpost"), true);
    addArticle(firstId, 2, 4, QStringLiteral("first-2@ngpost"), true);
    addArticle(secondId, 1, 0, QStringLiteral("second-1@ngpost"), true);
    addArticle(secondId, 2, 4, QStringLiteral("second-old@ngpost"), false);
    QVERIFY2(store.updateFileStatus(firstId, QStringLiteral("posted"), &err), qPrintable(err));
    QVERIFY2(store.updateFileStatus(secondId, QStringLiteral("partial"), &err), qPrintable(err));
    QVERIFY2(store.updatePostStatus(postId, QStringLiteral("partial"), 2, 4, 1, 16,
                                    QStringLiteral("1 KB/s"), &err),
             qPrintable(err));

    PostHistoryStore::ArticleRecord retry;
    retry.fileId = secondId;
    retry.part = 2;
    retry.pos = 4;
    retry.bytes = 4;
    QVERIFY2(store.upsertArticle(retry, &err), qPrintable(err));
    QVERIFY2(store.markArticlePosting(secondId, 2, QStringLiteral("second-new@ngpost"),
                                      2, &err),
             qPrintable(err));
    QVERIFY2(store.markArticlePosted(secondId, 2, QStringLiteral("second-new@ngpost"),
                                     &err),
             qPrintable(err));
    QVERIFY2(store.updateFileStatus(secondId, QStringLiteral("posted"), &err), qPrintable(err));
    QVERIFY2(store.updatePostStatus(postId, QStringLiteral("success"), 2, 4, 0, 16,
                                    QStringLiteral("1 KB/s"), &err),
             qPrintable(err));

    PostHistoryStore::PostDetails details;
    QVERIFY2(store.loadPostDetails(postId, &details, &err), qPrintable(err));
    QCOMPARE(details.files.size(), 2);
    QCOMPARE(details.files.at(0).ordinal, 1);
    QCOMPARE(details.files.at(1).ordinal, 2);

    NzbHistoryRegenerator regenerator(&store);
    QString nzb;
    QTextStream stream(&nzb);
    QVERIFY2(regenerator.writeNzb(postId, stream, false, nullptr, &err), qPrintable(err));

    QCOMPARE(countNzbSegments(nzb), 4);
    QVERIFY(nzb.contains(QStringLiteral("[1/2]")));
    QVERIFY(nzb.contains(QStringLiteral("[2/2]")));
    QVERIFY(nzb.contains(QStringLiteral("first-1@ngpost")));
    QVERIFY(nzb.contains(QStringLiteral("first-2@ngpost")));
    QVERIFY(nzb.contains(QStringLiteral("second-1@ngpost")));
    QVERIFY(nzb.contains(QStringLiteral("second-new@ngpost")));
    QVERIFY(!nzb.contains(QStringLiteral("second-old@ngpost")));
}

void TestPostHistory::nzb_regeneration_repairs_missing_article_bytes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString err;
    QVERIFY2(store.initialize(&err), qPrintable(err));

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("zero-bytes.nzb");
    post.nzbPath = dir.filePath(QStringLiteral("zero-bytes.nzb"));
    post.from = QStringLiteral("poster@example.invalid");
    post.groups = { QStringLiteral("alt.binaries.test") };
    // Make the original boundary differ from the current setting while both
    // remain plausible for three parts. The regenerator must use the frozen
    // fact, not whichever ARTICLE_SIZE happens to be configured today.
    const qint64 articleBytes = NgPost::articleSize() - 1;
    PostHistoryStore::PostInfo info;
    info.articleSizeBytes = articleBytes;
    const qint64 postId = store.createPost(post, info, {}, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    const qint64 tailBytes = 123;
    PostHistoryStore::FileRecord file;
    file.postId = postId;
    file.ordinal = 1;
    file.postedName = QStringLiteral("payload.bin");
    file.sizeBytes = articleBytes * 2 + tailBytes;
    file.totalArticles = 3;
    file.groups = post.groups;
    const qint64 fileId = store.upsertFile(file, &err);
    QVERIFY2(fileId > 0, qPrintable(err));

    for (int part = 1; part <= 3; ++part) {
        QVERIFY2(store.markArticlePosted(fileId,
                                         part,
                                         QStringLiteral("msg-%1@ngpost").arg(part),
                                         &err),
                 qPrintable(err));
    }
    QVERIFY2(store.updatePostStatus(postId, QStringLiteral("success"), 1, 3, 0,
                                    file.sizeBytes, QStringLiteral("1 KB/s"), &err),
             qPrintable(err));

    NzbHistoryRegenerator regenerator(&store);
    QString nzb;
    QStringList warnings;
    QTextStream stream(&nzb);
    QVERIFY2(regenerator.writeNzb(postId, stream, false, &warnings, &err), qPrintable(err));

    QList<qint64> expected;
    expected << articleBytes << articleBytes << tailBytes;
    QCOMPARE(nzbSegmentBytes(nzb), expected);
    QVERIFY(warnings.join(QLatin1Char('\n')).contains(QStringLiteral("3 article segment sizes")));
}

void TestPostHistory::nzb_regeneration_masks_password_by_default()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString err;

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("secret.nzb");
    post.nzbPath = dir.filePath(QStringLiteral("secret.nzb"));
    post.rarName = QStringLiteral("secret");
    post.rarPass = QStringLiteral("swordfish");
    post.hasPassword = true;
    post.passwordOrigin = QStringLiteral("generated");
    post.from = QStringLiteral("poster@example.invalid");
    post.groups = { QStringLiteral("alt.binaries.test") };
    const qint64 postId = store.createPost(post, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    PostHistoryStore::FileRecord file;
    file.postId = postId;
    file.ordinal = 1;
    file.postedName = QStringLiteral("payload.bin");
    file.sizeBytes = 4;
    file.totalArticles = 1;
    file.groups = post.groups;
    const qint64 fileId = store.upsertFile(file, &err);
    QVERIFY2(fileId > 0, qPrintable(err));

    PostHistoryStore::ArticleRecord article;
    article.fileId = fileId;
    article.part = 1;
    article.bytes = 4;
    QVERIFY2(store.upsertArticle(article, &err), qPrintable(err));
    QVERIFY2(store.markArticlePosted(fileId, 1, QStringLiteral("msg@ngpost"), &err), qPrintable(err));
    QVERIFY2(store.updatePostStatus(postId, QStringLiteral("success"), 1, 1, 0, 4,
                                    QStringLiteral("1 KB/s"), &err),
             qPrintable(err));

    NzbHistoryRegenerator regenerator(&store);
    QString noPassword;
    QTextStream noPasswordStream(&noPassword);
    QVERIFY2(regenerator.writeNzb(postId, noPasswordStream, false, nullptr, &err), qPrintable(err));
    QVERIFY(!noPassword.contains(QStringLiteral("swordfish")));

    QString withPassword;
    QTextStream withPasswordStream(&withPassword);
    QVERIFY2(regenerator.writeNzb(postId, withPasswordStream, true, nullptr, &err), qPrintable(err));
    QVERIFY(withPassword.contains(QStringLiteral("swordfish")));
}

void TestPostHistory::nzb_regeneration_uses_live_password_without_storing_it()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), false);
    QString err;

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("private.nzb");
    post.nzbPath = dir.filePath(QStringLiteral("private.nzb"));
    post.rarPass = QStringLiteral("not-in-sqlite");
    post.hasPassword = true;
    post.passwordOrigin = QStringLiteral("generated");
    post.from = QStringLiteral("poster@example.invalid");
    const qint64 postId = store.createPost(post, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    PostHistoryStore::FileRecord file;
    file.postId = postId;
    file.ordinal = 1;
    file.postedName = QStringLiteral("payload.bin");
    file.sizeBytes = 4;
    file.totalArticles = 1;
    const qint64 fileId = store.upsertFile(file, &err);
    QVERIFY2(fileId > 0, qPrintable(err));
    QVERIFY2(store.markArticlePosted(fileId, 1, QStringLiteral("private@ngpost"), &err),
             qPrintable(err));
    QVERIFY2(store.updatePostStatus(postId,
                                    QStringLiteral("success"),
                                    1,
                                    1,
                                    0,
                                    4,
                                    QStringLiteral("1 KB/s"),
                                    &err),
             qPrintable(err));

    // The storage policy is still honoured.
    PostHistoryStore::PostDetails details;
    QVERIFY2(store.loadPostDetails(postId, &details, &err), qPrintable(err));
    QVERIFY(!details.post.passwordStored);
    QVERIFY(details.rarPass.isEmpty());

    NzbHistoryRegenerator regenerator(&store);
    QString historical;
    QTextStream historicalStream(&historical);
    QVERIFY2(regenerator.writeNzb(postId, historicalStream, true, nullptr, &err), qPrintable(err));
    QVERIFY(!historical.contains(QStringLiteral("not-in-sqlite")));

    // Automatic finalisation can overlay the password still owned by the live
    // job, without persisting it. This prevents the history rewrite from
    // stripping a password already present in the streamed NZB.
    QString automatic;
    QTextStream automaticStream(&automatic);
    QVERIFY2(regenerator.writeNzb(postId,
                                  automaticStream,
                                  true,
                                  nullptr,
                                  &err,
                                  QStringLiteral("not-in-sqlite")),
             qPrintable(err));
    QVERIFY(automatic.contains(
        QStringLiteral("<meta type=\"password\">not-in-sqlite</meta>")));

    QString explicitlyMasked;
    QTextStream maskedStream(&explicitlyMasked);
    QVERIFY2(regenerator.writeNzb(postId,
                                  maskedStream,
                                  false,
                                  nullptr,
                                  &err,
                                  QStringLiteral("not-in-sqlite")),
             qPrintable(err));
    QVERIFY(!explicitlyMasked.contains(QStringLiteral("not-in-sqlite")));
}

void TestPostHistory::nzb_regeneration_refuses_incomplete_success_and_empty_history()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString error;

    PostHistoryStore::PostRecord empty;
    empty.nzbName = QStringLiteral("legacy.nzb");
    const qint64 emptyId = store.createPost(empty, &error);
    QVERIFY2(emptyId > 0, qPrintable(error));
    QVERIFY2(store.updatePostStatus(emptyId,
                                    QStringLiteral("success"),
                                    0, 0, 0, 0,
                                    QString(),
                                    &error),
             qPrintable(error));

    NzbHistoryRegenerator regenerator(&store);
    QString output;
    QTextStream outputStream(&output);
    QVERIFY(!regenerator.writeNzb(emptyId, outputStream, false, nullptr, &error));
    QVERIFY2(error.contains(QStringLiteral("no structured")), qPrintable(error));

    PostHistoryStore::PostRecord incomplete;
    incomplete.nzbName = QStringLiteral("incomplete.nzb");
    PostHistoryStore::PostInfo info;
    info.articleSizeBytes = 4;
    const qint64 postId = store.createPost(incomplete, info, {}, &error);
    QVERIFY2(postId > 0, qPrintable(error));
    PostHistoryStore::FileRecord file;
    file.postId = postId;
    file.ordinal = 1;
    file.postedName = QStringLiteral("two-parts.bin");
    file.sizeBytes = 8;
    file.totalArticles = 2;
    const qint64 fileId = store.upsertFile(file, &error);
    QVERIFY2(fileId > 0, qPrintable(error));
    QVERIFY2(store.markArticlePosted(fileId, 1, QStringLiteral("only-one@ngpost"), &error),
             qPrintable(error));
    QVERIFY2(store.updatePostStatus(postId,
                                    QStringLiteral("success"),
                                    1, 2, 0, 8,
                                    QStringLiteral("1 KB/s"),
                                    &error),
             qPrintable(error));

    output.clear();
    error.clear();
    QVERIFY(!regenerator.writeNzb(postId, outputStream, false, nullptr, &error));
    QVERIFY2(error.contains(QStringLiteral("incomplete")), qPrintable(error));
}

void TestPostHistory::nzb_regeneration_refuses_a_corrupt_stored_article_size()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString error;

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("bad-boundary.nzb");
    PostHistoryStore::PostInfo info;
    info.articleSizeBytes = 4;
    const qint64 postId = store.createPost(post, info, {}, &error);
    QVERIFY2(postId > 0, qPrintable(error));
    PostHistoryStore::FileRecord file;
    file.postId = postId;
    file.ordinal = 1;
    file.postedName = QStringLiteral("payload.bin");
    file.sizeBytes = 10; // 4-byte parts require 3 articles, not the stored 2
    file.totalArticles = 2;
    const qint64 fileId = store.upsertFile(file, &error);
    QVERIFY2(fileId > 0, qPrintable(error));
    QVERIFY2(store.markArticlePosted(fileId, 1, QStringLiteral("one@ngpost"), &error),
             qPrintable(error));
    QVERIFY2(store.markArticlePosted(fileId, 2, QStringLiteral("two@ngpost"), &error),
             qPrintable(error));
    QVERIFY2(store.updatePostStatus(postId,
                                    QStringLiteral("success"),
                                    1, 2, 0, 10,
                                    QStringLiteral("1 KB/s"),
                                    &error),
             qPrintable(error));

    NzbHistoryRegenerator regenerator(&store);
    QString output;
    QTextStream stream(&output);
    QVERIFY(!regenerator.writeNzb(postId, stream, false, nullptr, &error));
    QVERIFY2(error.contains(QStringLiteral("article size")), qPrintable(error));
}

void TestPostHistory::nzb_regeneration_to_file_is_atomic_on_failure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString dbPath  = dir.filePath(QStringLiteral("history.sqlite"));
    const QString nzbPath = dir.filePath(QStringLiteral("keep-me.nzb"));
    const QByteArray original("an existing streamed nzb\n");
    {
        QFile nzb(nzbPath);
        QVERIFY(nzb.open(QIODevice::WriteOnly));
        QCOMPARE(nzb.write(original), original.size());
    }

    PostHistoryService service(dbPath, true);
    QStringList warnings;
    QString error;
    QVERIFY(!service.regenerateNzbToFile(999999,
                                         nzbPath,
                                         true,
                                         &warnings,
                                         &error));
    QVERIFY(!error.isEmpty());

    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::ReadOnly));
    QCOMPARE(nzb.readAll(), original);
}

void TestPostHistory::import_legacy_csv_is_explicit_history_only()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString csvPath = dir.filePath(QStringLiteral("legacy.csv"));
    QFile csv(csvPath);
    QVERIFY(csv.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&csv);
    out << "date;nzb name;size;avg. speed;archive name;archive pass;groups;from\n";
    out << "2026/05/19 12:00:00;legacy.nzb;42;2 MB/s;legacy;pass;alt.binaries.test;poster@example.invalid\n";
    csv.close();

    PostHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")), true);
    QString err;
    QVERIFY2(store.importLegacyCsv(csvPath, &err), qPrintable(err));

    const QList<PostHistoryStore::PostSummary> posts = store.listPosts(QString(), QString(), false, &err);
    QCOMPARE(posts.size(), 1);
    QCOMPARE(posts.first().nzbName, QStringLiteral("legacy.nzb"));
    QCOMPARE(posts.first().nbArticles, 0);

    PostHistoryStore::PostInfoRecord record;
    QVERIFY2(store.loadPostInfoRecord(posts.first().id, &record, &err), qPrintable(err));
    QVERIFY(record.partial);
    QCOMPARE(rawSql(store.dbPath(), QStringLiteral("SELECT COUNT(*) FROM post_info")).toInt(), 0);
}


void TestPostHistory::service_construction_creates_no_database()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("never_touched.sqlite"));

    {
        PostHistoryService service(dbPath, true);
        // Let the worker thread run its start() the way it does in production.
        QTest::qWait(50);
        QVERIFY2(!QFileInfo::exists(dbPath),
                 "the service created a database it was never asked to use");
    }
    QVERIFY2(!QFileInfo::exists(dbPath), "a database appeared while tearing down");
}

void TestPostHistory::reconfigured_service_only_creates_the_configured_database()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // What NgPost does: build with the default path, then apply POST_DB.
    const QString defaultDb   = dir.filePath(QStringLiteral("default.sqlite"));
    const QString configuredDb = dir.filePath(QStringLiteral("configured.sqlite"));

    PostHistoryService service(defaultDb, true);
    service.configure(configuredDb, true);

    QString err;
    QVERIFY2(service.initialize(&err), qPrintable(err));

    PostHistoryStore::PostRecord rec;
    rec.nzbName = QStringLiteral("real.nzb");
    QVERIFY(service.createPost(rec, &err) > 0);

    QVERIFY2(QFileInfo::exists(configuredDb), "the configured database was not created");
    QVERIFY2(!QFileInfo::exists(defaultDb),
             "a stray database was left at the path POST_DB replaced");
}

void TestPostHistory::prepared_service_uses_configured_database_before_snapshot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString defaultDb = dir.filePath(QStringLiteral("default.sqlite"));
    const QString configuredDb = dir.filePath(QStringLiteral("configured.sqlite"));

    qint64 postId = 0;
    {
        PostHistoryStore store(configuredDb, true);
        QString err;
        QVERIFY2(store.initialize(&err), qPrintable(err));
        PostHistoryStore::PostRecord rec;
        rec.nzbName = QStringLiteral("interrupted-before-start.nzb");
        postId = store.createPost(rec, &err);
        QVERIFY2(postId > 0, qPrintable(err));
    }

    PostHistoryService service(defaultDb, true);
    service.configure(configuredDb, true);
    QSignalSpy preparedSpy(&service, &PostHistoryService::prepared);
    service.prepareForUse();

    QEventLoop loop;
    bool called = false;
    PostHistoryService::HistorySnapshot result;
    service.requestHistorySnapshot(PostHistoryStore::ListFilter(), {}, &loop,
                                   [&](const PostHistoryService::HistorySnapshot &snapshot) {
        result = snapshot;
        called = true;
        loop.quit();
    });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();

    QVERIFY(called);
    QCOMPARE(preparedSpy.size(), 1);
    QVERIFY(preparedSpy.first().first().toBool());
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.posts.size(), 1);
    QCOMPARE(result.posts.first().id, postId);
    QCOMPARE(result.posts.first().status, QStringLiteral("cancelled"));
    QVERIFY2(!QFileInfo::exists(defaultDb),
             "GUI preparation created the stale constructor database");
}

void TestPostHistory::existing_database_with_entries_is_never_reset()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("history.sqlite"));

    qint64 postId = 0;
    {   // an install with real history in it
        PostHistoryStore store(dbPath, true);
        QString err;
        QVERIFY2(store.initialize(&err), qPrintable(err));
        PostHistoryStore::PostRecord rec;
        rec.nzbName = QStringLiteral("precious.nzb");
        rec.rarName = QStringLiteral("precious");
        postId = store.createPost(rec, &err);
        QVERIFY2(postId > 0, qPrintable(err));
    }
    const qint64 sizeBefore = QFileInfo(dbPath).size();

    {   // a later start, exactly as NgPost does it
        PostHistoryService service(dbPath, true);
        QTest::qWait(50);
        QString err;
        QVERIFY2(service.initialize(&err), qPrintable(err));
        QVERIFY2(service.markPostCrashedArticlesUnknown(&err), qPrintable(err));
        QVERIFY2(service.cleanupInvalidResumePosts(&err), qPrintable(err));
    }

    PostHistoryStore store(dbPath, true);
    QString err;
    const QList<PostHistoryStore::PostSummary> posts =
        store.listPosts(PostHistoryStore::ListFilter(), &err);
    QCOMPARE(posts.size(), 1);
    QCOMPARE(posts.first().id, postId);
    QCOMPARE(posts.first().nzbName, QStringLiteral("precious.nzb"));
    QVERIFY2(QFileInfo(dbPath).size() >= sizeBefore, "the database shrank");
}

QTEST_MAIN(TestPostHistory)
#include "tst_PostHistory.moc"
