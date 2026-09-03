// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Structured posting history for ngPost.
//
//========================================================================

#include "history/PostHistoryStore.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTextStream>
#include <QThread>
#include <utility>

namespace
{

QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

QString joinGroups(const QStringList &groups)
{
    return groups.join(QStringLiteral(","));
}

void setError(QString *error, const QSqlQuery &q)
{
    if (error)
        *error = q.lastError().text();
}

void setError(QString *error, const QSqlDatabase &db)
{
    if (error)
        *error = db.lastError().text();
}

QSqlDatabase dbFor(const QString &connectionName, const QString &dbPath, QString *error)
{
    QSqlDatabase db;
    bool newConnection = false;
    if (QSqlDatabase::contains(connectionName))
        db = QSqlDatabase::database(connectionName);
    else {
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(dbPath);
        newConnection = true;
    }

    if (!db.isOpen() && !db.open()) {
        setError(error, db);
        return db;
    }

    QSqlQuery pragma(db);
    if (newConnection)
        pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    return db;
}

QString valueString(const QSqlQuery &q, const char *name)
{
    return q.value(q.record().indexOf(QString::fromLatin1(name))).toString();
}

qint64 valueI64(const QSqlQuery &q, const char *name)
{
    return q.value(q.record().indexOf(QString::fromLatin1(name))).toLongLong();
}

int valueInt(const QSqlQuery &q, const char *name)
{
    return q.value(q.record().indexOf(QString::fromLatin1(name))).toInt();
}

bool valueBool(const QSqlQuery &q, const char *name)
{
    return q.value(q.record().indexOf(QString::fromLatin1(name))).toInt() != 0;
}

qint64 storedPayloadPos(qint64 pos, qint64 bytes)
{
    return bytes > 0 ? pos : 0;
}

qint64 storedPayloadBytes(qint64 bytes)
{
    return bytes > 0 ? bytes : 0;
}

qint64 ceilDividePositive(qint64 numerator, qint64 denominator)
{
    if (numerator <= 0 || denominator <= 0)
        return 0;
    return numerator / denominator + (numerator % denominator ? 1 : 0);
}

//! Resolve the byte boundary of an old post without guessing. A non-final
//! recorded part or the position of any part after the first proves the exact
//! value. Once a candidate exists, the file sizes, recorded article counts and
//! every known payload boundary must all agree with it.
qint64 resolveArticleSizeBytes(const PostHistoryStore::PostDetails &details,
                               qint64 storedArticleSize)
{
    qint64 candidate = storedArticleSize > 0 ? storedArticleSize : 0;
    bool allFilesHaveOneArticle = !details.files.isEmpty();
    qint64 largestSingleArticle = 0;

    auto acceptCandidate = [&candidate](qint64 value) {
        if (value <= 0)
            return false;
        if (candidate > 0 && candidate != value)
            return false;
        candidate = value;
        return true;
    };

    for (const PostHistoryStore::FileSummary &file : details.files) {
        if (file.sizeBytes <= 0 || file.totalArticles <= 0)
            return -1;
        allFilesHaveOneArticle = allFilesHaveOneArticle && file.totalArticles == 1;
        if (file.totalArticles == 1 && file.sizeBytes > largestSingleArticle)
            largestSingleArticle = file.sizeBytes;

        const QList<PostHistoryStore::ArticleSummary> articles =
            details.articlesByFile.value(file.id);
        for (const PostHistoryStore::ArticleSummary &article : articles) {
            if (article.part <= 0 || article.part > file.totalArticles
                || article.pos < 0 || article.bytes < 0)
                return -1;

            // Every non-final part has exactly the configured payload size.
            if (article.bytes > 0 && article.part < file.totalArticles
                && !acceptCandidate(article.bytes))
                return -1;

            // Old rows sometimes have bytes but a default pos=0. A positive
            // position, however, proves the boundary exactly.
            if (article.pos > 0 && article.part > 1) {
                const qint64 previousParts = article.part - 1;
                if (article.pos % previousParts != 0
                    || !acceptCandidate(article.pos / previousParts))
                    return -1;
            }
        }
    }

    // With only one-part files, the largest file size is a safe boundary even
    // if the original configured value was larger: every file is still read as
    // the same single payload. Multi-part posts require exact evidence.
    if (candidate <= 0) {
        if (!allFilesHaveOneArticle || largestSingleArticle <= 0)
            return -1;
        candidate = largestSingleArticle;
    }

    for (const PostHistoryStore::FileSummary &file : details.files) {
        if (ceilDividePositive(file.sizeBytes, candidate) != file.totalArticles)
            return -1;

        const QList<PostHistoryStore::ArticleSummary> articles =
            details.articlesByFile.value(file.id);
        for (const PostHistoryStore::ArticleSummary &article : articles) {
            const qint64 expectedPos = candidate * static_cast<qint64>(article.part - 1);
            const qint64 expectedBytes = qMin(candidate, file.sizeBytes - expectedPos);
            if (expectedBytes <= 0)
                return -1;
            if (article.pos > 0 && article.pos != expectedPos)
                return -1;
            if (article.bytes > 0 && article.bytes != expectedBytes)
                return -1;
        }
    }
    return candidate;
}

} // namespace

PostHistoryStore::PostHistoryStore(const QString &dbPath, bool storePasswords)
    : _dbPath(dbPath)
    , _storePasswords(storePasswords)
    , _initialized(false)
    , _initializedDbPath()
{
}

PostHistoryStore::~PostHistoryStore()
{
    closeConnection();
}

void PostHistoryStore::configure(const QString &dbPath, bool storePasswords)
{
    if (_dbPath != dbPath || _storePasswords != storePasswords) {
        closeConnection();
        _initialized = false;
        _initializedDbPath.clear();
    }
    _dbPath = dbPath;
    _storePasswords = storePasswords;
}

void PostHistoryStore::closeConnection()
{
    const QString connection = _connectionName();
    _initialized = false;
    _initializedDbPath.clear();

    if (!QSqlDatabase::contains(connection))
        return;

    {
        QSqlDatabase db = QSqlDatabase::database(connection, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

QString PostHistoryStore::dbPath() const
{
    return _dbPath;
}

bool PostHistoryStore::storePasswords() const
{
    return _storePasswords;
}

QString PostHistoryStore::_connectionName() const
{
    return QStringLiteral("ngpost_history_%1_%2")
        .arg(reinterpret_cast<quintptr>(this))
        .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
}

bool PostHistoryStore::initialize(QString *error)
{
    if (_initialized && _initializedDbPath == _dbPath)
        return true;

    if (_dbPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("empty history database path");
        return false;
    }

    QFileInfo fi(_dbPath);
    if (!fi.absoluteDir().exists() && !fi.absoluteDir().mkpath(QStringLiteral("."))) {
        if (error)
            *error = QStringLiteral("cannot create history database directory: %1")
                         .arg(fi.absolutePath());
        return false;
    }

    if (!_execSchema(error))
        return false;

    _initialized = true;
    _initializedDbPath = _dbPath;
    return true;
}

bool PostHistoryStore::_exec(const QString &sql, QString *error)
{
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    if (!db.isOpen())
        return false;
    QSqlQuery q(db);
    if (!q.exec(sql)) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::_execSchema(QString *error)
{
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    if (!db.isOpen())
        return false;

    if (!db.transaction()) {
        setError(error, db);
        return false;
    }

    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS schema_meta ("
                       "key TEXT PRIMARY KEY, value TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS posts ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "created_at TEXT NOT NULL,"
                       "started_at TEXT,"
                       "finished_at TEXT,"
                       "nzb_name TEXT NOT NULL,"
                       "nzb_path TEXT,"
                       "status TEXT NOT NULL,"
                       "size_bytes INTEGER DEFAULT 0,"
                       "avg_speed TEXT,"
                       "nb_files INTEGER DEFAULT 0,"
                       "nb_articles INTEGER DEFAULT 0,"
                       "nb_failed_articles INTEGER DEFAULT 0,"
                       "rar_name TEXT,"
                       "rar_pass TEXT,"
                       "has_password INTEGER DEFAULT 0,"
                       "password_stored INTEGER DEFAULT 0,"
                       "password_origin TEXT,"
                       "from_addr TEXT,"
                       "do_compress INTEGER DEFAULT 0,"
                       "do_par2 INTEGER DEFAULT 0,"
                       "obfuscate_articles INTEGER DEFAULT 0,"
                       "obfuscate_file_name INTEGER DEFAULT 0,"
                       "resume_state TEXT,"
                       "resume_reason TEXT)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS post_groups ("
                       "post_id INTEGER NOT NULL,"
                       "group_name TEXT NOT NULL,"
                       "FOREIGN KEY(post_id) REFERENCES posts(id) ON DELETE CASCADE)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS post_files ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "post_id INTEGER NOT NULL,"
                       "ordinal INTEGER NOT NULL,"
                       "original_path TEXT,"
                       "posted_name TEXT,"
                       "size_bytes INTEGER DEFAULT 0,"
                       "mtime_epoch INTEGER DEFAULT 0,"
                       "total_articles INTEGER DEFAULT 0,"
                       "groups_text TEXT,"
                       "status TEXT NOT NULL,"
                       "FOREIGN KEY(post_id) REFERENCES posts(id) ON DELETE CASCADE,"
                       "UNIQUE(post_id, ordinal))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS post_articles ("
                       "file_id INTEGER NOT NULL,"
                       "part INTEGER NOT NULL,"
                       "pos INTEGER DEFAULT 0,"
                       "bytes INTEGER DEFAULT 0,"
                       "body_bytes INTEGER DEFAULT 0,"
                       "status TEXT NOT NULL,"
                       "msg_id TEXT,"
                       "error TEXT,"
                       "updated_at TEXT,"
                       "FOREIGN KEY(file_id) REFERENCES post_files(id) ON DELETE CASCADE,"
                       "PRIMARY KEY(file_id, part))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS post_article_attempts ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "file_id INTEGER NOT NULL,"
                       "part INTEGER NOT NULL,"
                       "attempt_no INTEGER NOT NULL,"
                       "msg_id TEXT,"
                       "status TEXT NOT NULL,"
                       "error TEXT,"
                       "created_at TEXT NOT NULL,"
                       "finished_at TEXT,"
                       "FOREIGN KEY(file_id) REFERENCES post_files(id) ON DELETE CASCADE)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_posts_status ON posts(status)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_articles_status ON post_articles(status)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_groups_post ON post_groups(post_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_post ON post_files(post_id)"),
        // Without this index the per-article status UPDATEs (posted/failed/unknown,
        // keyed on file_id+part) full-scan post_article_attempts, a table that grows
        // for the whole queue and is never pruned for successful posts. That turns
        // finalization flushing into O(N^2) work and makes the inter-post stall grow
        // post after post. (file_id, part) also covers the resume-purge delete.
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_attempts_file_part "
                       "ON post_article_attempts(file_id, part)"),
        // Facts about a post that the aggregates of `posts` cannot express.
        // post_size_bytes is NOT a duplicate of posts.size_bytes: the latter
        // sums everything that was posted, this one is the archive and its
        // parity only, without the .nfo copied next to the rar volumes.
        QStringLiteral("CREATE TABLE IF NOT EXISTS post_info ("
                       "post_id INTEGER PRIMARY KEY,"
                       "par2_pct INTEGER,"
                       "post_size_bytes INTEGER,"
                       "active_seconds INTEGER,"
                       "article_size_bytes INTEGER,"
                       "source_path TEXT,"
                       "original_name TEXT,"
                       "app_version TEXT,"
                       "FOREIGN KEY(post_id) REFERENCES posts(id) ON DELETE CASCADE)"),
        // User metadata. The scope decides whether it may leave the machine:
        // 'local' stays in the post info file, 'nzb' is also published in the
        // <head> of the nzb, which circulates.
        QStringLiteral("CREATE TABLE IF NOT EXISTS post_meta ("
                       "post_id INTEGER NOT NULL,"
                       "key TEXT NOT NULL,"
                       "value TEXT,"
                       "scope TEXT NOT NULL DEFAULT 'local' CHECK(scope IN ('local','nzb')),"
                       "PRIMARY KEY(post_id, key),"
                       "FOREIGN KEY(post_id) REFERENCES posts(id) ON DELETE CASCADE)")
    };

    for (const QString &sql : statements) {
        QSqlQuery q(db);
        if (!q.exec(sql)) {
            setError(error, q);
            db.rollback();
            return false;
        }
    }

    if (!_migrateSchema(db, error)) {
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        setError(error, db);
        return false;
    }
    return true;
}

//! Reads the stored schema version, refuses a database written by a newer
//! ngPost, applies the pending migrations, then records the new version. Runs
//! inside the transaction opened by _execSchema().
//!
//! Until now the version row was rewritten to '1' on every open and never read
//! back, which meant there was no migration story at all.
bool PostHistoryStore::_migrateSchema(QSqlDatabase &db, QString *error)
{
    int storedVersion = 0;
    bool hasStoredVersion = false;
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT value FROM schema_meta WHERE key='version'"));
        if (!q.exec()) {
            setError(error, q);
            return false;
        }
        if (q.next()) {
            hasStoredVersion = true;
            bool ok = false;
            storedVersion = q.value(0).toString().toInt(&ok);
            if (!ok || storedVersion <= 0) {
                if (error)
                    *error = QStringLiteral("invalid history schema version '%1'")
                                 .arg(q.value(0).toString());
                return false;
            }
        }
    }

    if (!hasStoredVersion) {
        // No version yet: either a brand new database, or one from before the
        // version row was read back. Both have the v1 shape at this point,
        // since the CREATE TABLE statements above just ran.
        storedVersion = 1;
    }

    if (storedVersion > kSchemaVersion) {
        if (error)
            *error = QStringLiteral(
                         "this history database was written by a newer ngPost "
                         "(schema v%1, this version understands v%2); "
                         "update ngPost rather than risking your history")
                         .arg(storedVersion)
                         .arg(kSchemaVersion);
        return false;
    }

    // v1 -> v2 adds post_info and post_meta, both created above by
    // CREATE TABLE IF NOT EXISTS, which does apply to an existing database
    // (unlike a new column, which would need an ALTER TABLE).

    // v2 -> v3 records the exact part boundary. CREATE TABLE above already
    // gives brand-new and v1 databases the current shape; an existing v2 table
    // needs the column added explicitly.
    bool hasArticleSizeColumn = false;
    {
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral("PRAGMA table_info(post_info)"))) {
            setError(error, q);
            return false;
        }
        while (q.next()) {
            if (q.value(1).toString() == QStringLiteral("article_size_bytes")) {
                hasArticleSizeColumn = true;
                break;
            }
        }
    }
    if (!hasArticleSizeColumn) {
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral(
                "ALTER TABLE post_info ADD COLUMN article_size_bytes INTEGER"))) {
            setError(error, q);
            return false;
        }
    }

    // v3 -> v4 records the size of the article as posted (yEnc encoded), which
    // is what an nzb <segment bytes> must advertise. `bytes` keeps meaning the
    // slice of the source file, because resolveArticleSizeBytes() reconstructs
    // the configured article size from it. Rows written before this migration
    // keep body_bytes = 0 and fall back to `bytes`, as they always did.
    bool hasArticleBodyBytesColumn = false;
    {
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral("PRAGMA table_info(post_articles)"))) {
            setError(error, q);
            return false;
        }
        while (q.next()) {
            if (q.value(1).toString() == QStringLiteral("body_bytes")) {
                hasArticleBodyBytesColumn = true;
                break;
            }
        }
    }
    if (!hasArticleBodyBytesColumn) {
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral(
                "ALTER TABLE post_articles ADD COLUMN body_bytes INTEGER DEFAULT 0"))) {
            setError(error, q);
            return false;
        }
    }

    if (storedVersion != kSchemaVersion) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("INSERT OR REPLACE INTO schema_meta(key, value) "
                                 "VALUES('version', ?)"));
        q.addBindValue(QString::number(kSchemaVersion));
        if (!q.exec()) {
            setError(error, q);
            return false;
        }
    }
    return true;
}

qint64 PostHistoryStore::createPost(const PostRecord &record, QString *error)
{
    // This overload predates post_info. Keeping it genuinely legacy matters to
    // CSV imports: empty values are unknown facts, not a complete modern row.
    return _createPost(record, nullptr, QMap<QString, MetaValue>(), error);
}

qint64 PostHistoryStore::createPost(const PostRecord &record,
                                    const PostInfo &info,
                                    const QMap<QString, MetaValue> &meta,
                                    QString *error)
{
    return _createPost(record, &info, meta, error);
}

qint64 PostHistoryStore::_createPost(const PostRecord &record,
                                     const PostInfo *info,
                                     const QMap<QString, MetaValue> &meta,
                                     QString *error)
{
    if (!initialize(error))
        return 0;

    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    if (!db.transaction()) {
        setError(error, db);
        return 0;
    }

    QSqlQuery q(db);
    // started_at is left NULL on purpose: a job can wait a long time in the
    // queue, and it is markPostStarted() that records when the transfer really
    // began.
    q.prepare(QStringLiteral("INSERT INTO posts("
                             "created_at, nzb_name, nzb_path, status,"
                             "rar_name, rar_pass, has_password, password_stored,"
                             "password_origin, from_addr, do_compress, do_par2,"
                             "obfuscate_articles, obfuscate_file_name, resume_state)"
                             "VALUES(?, ?, ?, 'posting', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'resumable')"));
    const bool passwordStored = _storePasswords && !record.rarPass.isEmpty();
    q.addBindValue(nowIso());
    q.addBindValue(record.nzbName);
    q.addBindValue(record.nzbPath);
    q.addBindValue(record.rarName);
    q.addBindValue(passwordStored ? record.rarPass : QString());
    q.addBindValue(record.hasPassword ? 1 : 0);
    q.addBindValue(passwordStored ? 1 : 0);
    q.addBindValue(record.passwordOrigin);
    q.addBindValue(record.from);
    q.addBindValue(record.doCompress ? 1 : 0);
    q.addBindValue(record.doPar2 ? 1 : 0);
    q.addBindValue(record.obfuscateArticles ? 1 : 0);
    q.addBindValue(record.obfuscateFileName ? 1 : 0);
    if (!q.exec()) {
        setError(error, q);
        db.rollback();
        return 0;
    }
    const qint64 postId = q.lastInsertId().toLongLong();

    for (const QString &group : record.groups) {
        QSqlQuery g(db);
        g.prepare(QStringLiteral("INSERT INTO post_groups(post_id, group_name) VALUES(?, ?)"));
        g.addBindValue(postId);
        g.addBindValue(group);
        if (!g.exec()) {
            setError(error, g);
            db.rollback();
            return 0;
        }
    }

    if ((info && !_writePostInfo(db, postId, *info, error))
        || !_writePostMeta(db, postId, meta, error)) {
        db.rollback();
        return 0;
    }

    if (!db.commit()) {
        setError(error, db);
        return 0;
    }
    return postId;
}

bool PostHistoryStore::_writePostInfo(QSqlDatabase &db,
                                      qint64 postId,
                                      const PostInfo &info,
                                      QString *error)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT INTO post_info("
                             "post_id, par2_pct, post_size_bytes, active_seconds,"
                             "article_size_bytes, source_path, original_name, app_version)"
                             "VALUES(?, ?, ?, ?, ?, ?, ?, ?)"
                             "ON CONFLICT(post_id) DO UPDATE SET"
                             " par2_pct=excluded.par2_pct,"
                             " article_size_bytes=excluded.article_size_bytes,"
                             " source_path=excluded.source_path,"
                             " original_name=excluded.original_name,"
                             " app_version=excluded.app_version"));
    q.addBindValue(postId);
    q.addBindValue(info.par2Pct < 0 ? QVariant() : QVariant(info.par2Pct));
    q.addBindValue(info.postSizeBytes < 0 ? QVariant() : QVariant(info.postSizeBytes));
    q.addBindValue(info.activeSeconds < 0 ? QVariant() : QVariant(info.activeSeconds));
    q.addBindValue(info.articleSizeBytes <= 0 ? QVariant() : QVariant(info.articleSizeBytes));
    q.addBindValue(info.sourcePath);
    q.addBindValue(info.originalName);
    q.addBindValue(info.appVersion);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::_writePostMeta(QSqlDatabase &db,
                                      qint64 postId,
                                      const QMap<QString, MetaValue> &meta,
                                      QString *error)
{
    for (auto it = meta.cbegin(); it != meta.cend(); ++it) {
        const QString key = it.key().trimmed();
        if (key.isEmpty())
            continue;
        // "password" is a secret, stored and purged like the archive password;
        // letting it in here would bypass HISTORY_STORE_PASSWORDS, survive
        // purgePassword() and come back out despite includePassword=false.
        if (key.compare(QStringLiteral("password"), Qt::CaseInsensitive) == 0)
            continue;

        QSqlQuery q(db);
        q.prepare(QStringLiteral("INSERT INTO post_meta(post_id, key, value, scope)"
                                 "VALUES(?, ?, ?, ?)"
                                 "ON CONFLICT(post_id, key) DO UPDATE SET"
                                 " value=excluded.value, scope=excluded.scope"));
        q.addBindValue(postId);
        q.addBindValue(key);
        q.addBindValue(it.value().value);
        q.addBindValue(it.value().scope == MetaScope::Nzb ? QStringLiteral("nzb")
                                                          : QStringLiteral("local"));
        if (!q.exec()) {
            setError(error, q);
            return false;
        }
    }
    return true;
}

bool PostHistoryStore::setPostMeta(qint64 postId,
                                   const QMap<QString, MetaValue> &meta,
                                   QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    if (!db.transaction()) {
        setError(error, db);
        return false;
    }
    if (!_writePostMeta(db, postId, meta, error)) {
        db.rollback();
        return false;
    }
    if (!db.commit()) {
        setError(error, db);
        return false;
    }
    return true;
}

bool PostHistoryStore::markPostStarted(qint64 postId, QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);

    QSqlQuery q(db);
    // "AND started_at IS NULL": a resume keeps the date of the first attempt.
    q.prepare(QStringLiteral("UPDATE posts SET started_at=? WHERE id=? AND started_at IS NULL"));
    q.addBindValue(nowIso());
    q.addBindValue(postId);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::updatePostNzbPath(qint64 postId, const QString &nzbPath, QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE posts SET nzb_path=?, nzb_name=? WHERE id=?"));
    q.addBindValue(nzbPath);
    q.addBindValue(QFileInfo(nzbPath).fileName());
    q.addBindValue(postId);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::setPostSizeIfUnset(qint64 postId, qint64 sizeBytes, QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    // Only the first attempt knows the size of the whole post; a resume only
    // ever sees the leftovers.
    q.prepare(QStringLiteral("UPDATE post_info SET post_size_bytes=? "
                             "WHERE post_id=? AND post_size_bytes IS NULL"));
    q.addBindValue(sizeBytes);
    q.addBindValue(postId);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::addActiveSeconds(qint64 postId, qint64 seconds, QString *error)
{
    if (!initialize(error))
        return false;
    if (seconds <= 0)
        return true;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE post_info SET active_seconds=COALESCE(active_seconds, 0) + ? "
                             "WHERE post_id=?"));
    q.addBindValue(seconds);
    q.addBindValue(postId);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::finalizePost(qint64 postId,
                                   const QString &status,
                                   const QString &avgSpeed,
                                   qint64 activeSeconds,
                                   QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    if (!db.transaction()) {
        setError(error, db);
        return false;
    }
    // updatePostStatus() recomputes its counters from post_files/post_articles,
    // so the sizes and counts it is given are ignored on purpose.
    if (!updatePostStatus(postId, status, 0, 0, 0, 0, avgSpeed, error)
        || !addActiveSeconds(postId, activeSeconds, error)) {
        db.rollback();
        return false;
    }
    if (!db.commit()) {
        setError(error, db);
        return false;
    }
    return true;
}

bool PostHistoryStore::updatePostStatus(qint64 postId,
                                        const QString &status,
                                        int nbFiles,
                                        int nbArticles,
                                        int nbFailedArticles,
                                        qint64 sizeBytes,
                                        const QString &avgSpeed,
                                        QString *error)
{
    Q_UNUSED(nbFiles)
    Q_UNUSED(nbArticles)
    Q_UNUSED(nbFailedArticles)
    Q_UNUSED(sizeBytes)

    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    bool hasFiles = false;
    {
        QSqlQuery resumable(db);
        resumable.prepare(QStringLiteral("SELECT EXISTS(SELECT 1 FROM post_files WHERE post_id=?)"));
        resumable.addBindValue(postId);
        if (!resumable.exec() || !resumable.next()) {
            setError(error, resumable);
            return false;
        }
        hasFiles = resumable.value(0).toBool();
    }
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE posts SET status=?, finished_at=?,"
                             "nb_files=(SELECT COUNT(*) FROM post_files WHERE post_id=?),"
                             "nb_articles=(SELECT COALESCE(SUM(total_articles), 0) "
                             "FROM post_files WHERE post_id=?),"
                             "nb_failed_articles=(SELECT COUNT(*) FROM post_articles "
                             "WHERE file_id IN (SELECT id FROM post_files WHERE post_id=?) "
                             "AND status IN ('failed','unknown')),"
                             "size_bytes=(SELECT COALESCE(SUM(size_bytes), 0) "
                             "FROM post_files WHERE post_id=?),"
                             "avg_speed=?, resume_state=?, resume_reason=? WHERE id=?"));
    q.addBindValue(status);
    q.addBindValue(nowIso());
    q.addBindValue(postId);
    q.addBindValue(postId);
    q.addBindValue(postId);
    q.addBindValue(postId);
    q.addBindValue(avgSpeed);
    const bool canResume = status != QStringLiteral("success") && hasFiles;
    q.addBindValue(canResume ? QStringLiteral("resumable") : QString());
    q.addBindValue(status == QStringLiteral("success")
                       ? QString()
                       : (canResume ? QStringLiteral("failed or unknown articles remain")
                                    : QStringLiteral("posting never started; nothing to resume")));
    q.addBindValue(postId);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }

    // Once a post reaches a terminal status, its per-attempt audit rows are dead
    // weight: nothing reads post_article_attempts (NZB regeneration and resume
    // both rely on post_articles), and keeping them lets the table grow unbounded
    // across a posting queue. Drop them here, best-effort: the status update is
    // the critical part and must not fail because of this housekeeping, so a purge
    // error is not propagated (the rows stay and get collected on a later pass).
    if (status == QStringLiteral("success") || status == QStringLiteral("partial")
        || status == QStringLiteral("failed")) {
        QSqlQuery purge(db);
        purge.prepare(QStringLiteral("DELETE FROM post_article_attempts "
                                     "WHERE file_id IN (SELECT id FROM post_files WHERE post_id=?)"));
        purge.addBindValue(postId);
        purge.exec();
    }
    return true;
}

bool PostHistoryStore::markPostResuming(qint64 postId, QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    // Keep the previous terminal facts until this attempt itself completes.
    // In particular, a resume refused before transfer must be able to restore
    // the row without inventing a new finish time or losing its average speed.
    q.prepare(QStringLiteral("UPDATE posts SET status='posting',"
                             "resume_state='resumable', resume_reason='resume in progress' "
                             "WHERE id=?"));
    q.addBindValue(postId);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::setPostAbandoned(qint64 postId, QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE posts SET status='cancelled', resume_state=NULL,"
                             "resume_reason='abandoned by user' WHERE id=?"));
    q.addBindValue(postId);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::purgeResumeData(qint64 postId, QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    if (!db.transaction()) {
        setError(error, db);
        return false;
    }
    QSqlQuery delAttempts(db);
    delAttempts.prepare(QStringLiteral("DELETE FROM post_article_attempts "
                                       "WHERE file_id IN (SELECT id FROM post_files WHERE post_id=?)"));
    delAttempts.addBindValue(postId);
    if (!delAttempts.exec()) {
        setError(error, delAttempts);
        db.rollback();
        return false;
    }
    QSqlQuery upd(db);
    upd.prepare(QStringLiteral("UPDATE posts SET resume_state=NULL,"
                               "resume_reason='resume data purged' WHERE id=?"));
    upd.addBindValue(postId);
    if (!upd.exec()) {
        setError(error, upd);
        db.rollback();
        return false;
    }
    if (!db.commit()) {
        setError(error, db);
        return false;
    }
    return true;
}

bool PostHistoryStore::purgePassword(qint64 postId, QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE posts SET rar_pass='', password_stored=0 WHERE id=?"));
    q.addBindValue(postId);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

qint64 PostHistoryStore::upsertFile(const FileRecord &record, QString *error)
{
    if (!initialize(error))
        return 0;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT INTO post_files(post_id, ordinal, original_path,"
                             "posted_name, size_bytes, mtime_epoch, total_articles,"
                             "groups_text, status) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)"
                             "ON CONFLICT(post_id, ordinal) DO UPDATE SET "
                             "original_path=excluded.original_path,"
                             "posted_name=excluded.posted_name,"
                             "size_bytes=excluded.size_bytes,"
                             "mtime_epoch=excluded.mtime_epoch,"
                             "total_articles=excluded.total_articles,"
                             "groups_text=excluded.groups_text,"
                             "status=excluded.status"));
    q.addBindValue(record.postId);
    q.addBindValue(record.ordinal);
    q.addBindValue(record.originalPath);
    q.addBindValue(record.postedName);
    q.addBindValue(record.sizeBytes);
    q.addBindValue(record.mtimeEpoch);
    q.addBindValue(record.totalArticles);
    q.addBindValue(joinGroups(record.groups));
    q.addBindValue(record.status);
    if (!q.exec()) {
        setError(error, q);
        return 0;
    }

    QSqlQuery s(db);
    s.prepare(QStringLiteral("SELECT id FROM post_files WHERE post_id=? AND ordinal=?"));
    s.addBindValue(record.postId);
    s.addBindValue(record.ordinal);
    if (!s.exec() || !s.next()) {
        setError(error, s);
        return 0;
    }
    return s.value(0).toLongLong();
}

bool PostHistoryStore::updateFileStatus(qint64 fileId, const QString &status, QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE post_files SET status=? WHERE id=?"));
    q.addBindValue(status);
    q.addBindValue(fileId);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::upsertArticle(const ArticleRecord &record, QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT INTO post_articles(file_id, part, pos, bytes,"
                             "status, msg_id, error, updated_at)"
                             "VALUES(?, ?, ?, ?, ?, ?, ?, ?)"
                             "ON CONFLICT(file_id, part) DO UPDATE SET "
                             "pos=excluded.pos, bytes=excluded.bytes,"
                             "status=excluded.status, msg_id=excluded.msg_id,"
                             "error=excluded.error, updated_at=excluded.updated_at"));
    q.addBindValue(record.fileId);
    q.addBindValue(record.part);
    q.addBindValue(record.pos);
    q.addBindValue(record.bytes);
    q.addBindValue(record.status);
    q.addBindValue(record.msgId);
    q.addBindValue(record.error);
    q.addBindValue(nowIso());
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::updateArticlePayload(qint64 fileId,
                                            int part,
                                            qint64 pos,
                                            qint64 bytes,
                                            QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT INTO post_articles(file_id, part, pos, bytes,"
                             "status, updated_at)"
                             "VALUES(?, ?, ?, ?, 'pending', ?)"
                             "ON CONFLICT(file_id, part) DO UPDATE SET "
                             "pos=excluded.pos, bytes=excluded.bytes,"
                             "updated_at=excluded.updated_at"));
    q.addBindValue(fileId);
    q.addBindValue(part);
    q.addBindValue(pos);
    q.addBindValue(bytes);
    q.addBindValue(nowIso());
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::markArticlePosting(qint64 fileId,
                                          int part,
                                          const QString &msgId,
                                          int attemptNo,
                                          QString *error)
{
    return markArticlePosting(fileId, part, msgId, attemptNo, -1, -1, error);
}

bool PostHistoryStore::markArticlePosting(qint64 fileId,
                                          int part,
                                          const QString &msgId,
                                          int attemptNo,
                                          qint64 pos,
                                          qint64 bytes,
                                          QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    if (!db.transaction()) {
        setError(error, db);
        return false;
    }
    QSqlQuery a(db);
    a.prepare(QStringLiteral("INSERT INTO post_articles(file_id, part, pos, bytes,"
                             "status, msg_id, error, updated_at)"
                             "VALUES(?, ?, ?, ?, 'posting', ?, '', ?)"
                             "ON CONFLICT(file_id, part) DO UPDATE SET "
                             "pos=CASE WHEN excluded.bytes > 0 THEN excluded.pos ELSE pos END,"
                             "bytes=CASE WHEN excluded.bytes > 0 THEN excluded.bytes ELSE bytes END,"
                             "status='posting', msg_id=excluded.msg_id,"
                             "updated_at=excluded.updated_at"));
    a.addBindValue(fileId);
    a.addBindValue(part);
    a.addBindValue(storedPayloadPos(pos, bytes));
    a.addBindValue(storedPayloadBytes(bytes));
    a.addBindValue(msgId);
    a.addBindValue(nowIso());
    if (!a.exec()) {
        setError(error, a);
        db.rollback();
        return false;
    }
    QSqlQuery t(db);
    t.prepare(QStringLiteral("INSERT INTO post_article_attempts(file_id, part,"
                             "attempt_no, msg_id, status, created_at)"
                             "VALUES(?, ?, ?, ?, 'posting', ?)"));
    t.addBindValue(fileId);
    t.addBindValue(part);
    t.addBindValue(attemptNo);
    t.addBindValue(msgId);
    t.addBindValue(nowIso());
    if (!t.exec()) {
        setError(error, t);
        db.rollback();
        return false;
    }
    if (!db.commit()) {
        setError(error, db);
        return false;
    }
    return true;
}

bool PostHistoryStore::markArticlePosted(qint64 fileId, int part, const QString &msgId, QString *error)
{
    return markArticlePosted(fileId, part, msgId, -1, -1, error);
}

bool PostHistoryStore::markArticlePosted(qint64 fileId,
                                         int part,
                                         const QString &msgId,
                                         qint64 pos,
                                         qint64 bytes,
                                         QString *error)
{
    // Same guard as every other entry point: never touch dbFor() before the
    // schema exists, or the database file gets created empty and every query
    // against it fails.
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery a(db);
    a.prepare(QStringLiteral("INSERT INTO post_articles(file_id, part, pos, bytes,"
                             "status, msg_id, error, updated_at)"
                             "VALUES(?, ?, ?, ?, 'posted', ?, '', ?)"
                             "ON CONFLICT(file_id, part) DO UPDATE SET "
                             "pos=CASE WHEN excluded.bytes > 0 THEN excluded.pos ELSE pos END,"
                             "bytes=CASE WHEN excluded.bytes > 0 THEN excluded.bytes ELSE bytes END,"
                             "status='posted', msg_id=excluded.msg_id, error='',"
                             "updated_at=excluded.updated_at"));
    a.addBindValue(fileId);
    a.addBindValue(part);
    a.addBindValue(storedPayloadPos(pos, bytes));
    a.addBindValue(storedPayloadBytes(bytes));
    a.addBindValue(msgId);
    a.addBindValue(nowIso());
    if (!a.exec()) {
        setError(error, a);
        return false;
    }
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE post_article_attempts SET status='posted',"
                             "finished_at=? WHERE file_id=? AND part=? AND msg_id=?"
                             " AND status='posting'"));
    q.addBindValue(nowIso());
    q.addBindValue(fileId);
    q.addBindValue(part);
    q.addBindValue(msgId);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::markArticleFailed(qint64 fileId,
                                         int part,
                                         const QString &msgId,
                                         const QString &err,
                                         QString *error)
{
    return markArticleFailed(fileId, part, msgId, err, -1, -1, error);
}

bool PostHistoryStore::markArticleFailed(qint64 fileId,
                                         int part,
                                         const QString &msgId,
                                         const QString &err,
                                         qint64 pos,
                                         qint64 bytes,
                                         QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery a(db);
    a.prepare(QStringLiteral("INSERT INTO post_articles(file_id, part, pos, bytes,"
                             "status, msg_id, error, updated_at)"
                             "VALUES(?, ?, ?, ?, 'failed', ?, ?, ?)"
                             "ON CONFLICT(file_id, part) DO UPDATE SET "
                             "pos=CASE WHEN excluded.bytes > 0 THEN excluded.pos ELSE pos END,"
                             "bytes=CASE WHEN excluded.bytes > 0 THEN excluded.bytes ELSE bytes END,"
                             "status='failed', msg_id=excluded.msg_id, error=excluded.error,"
                             "updated_at=excluded.updated_at"));
    a.addBindValue(fileId);
    a.addBindValue(part);
    a.addBindValue(storedPayloadPos(pos, bytes));
    a.addBindValue(storedPayloadBytes(bytes));
    a.addBindValue(msgId);
    a.addBindValue(err);
    a.addBindValue(nowIso());
    if (!a.exec()) {
        setError(error, a);
        return false;
    }
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE post_article_attempts SET status='failed',"
                             "error=?, finished_at=? WHERE file_id=? AND part=?"
                             " AND msg_id=? AND status='posting'"));
    q.addBindValue(err);
    q.addBindValue(nowIso());
    q.addBindValue(fileId);
    q.addBindValue(part);
    q.addBindValue(msgId);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::markArticleUnknown(qint64 fileId,
                                          int part,
                                          const QString &msgId,
                                          const QString &err,
                                          QString *error)
{
    return markArticleUnknown(fileId, part, msgId, err, -1, -1, error);
}

bool PostHistoryStore::markArticleUnknown(qint64 fileId,
                                          int part,
                                          const QString &msgId,
                                          const QString &err,
                                          qint64 pos,
                                          qint64 bytes,
                                          QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery a(db);
    a.prepare(QStringLiteral("INSERT INTO post_articles(file_id, part, pos, bytes,"
                             "status, msg_id, error, updated_at)"
                             "VALUES(?, ?, ?, ?, 'unknown', ?, ?, ?)"
                             "ON CONFLICT(file_id, part) DO UPDATE SET "
                             "pos=CASE WHEN excluded.bytes > 0 THEN excluded.pos ELSE pos END,"
                             "bytes=CASE WHEN excluded.bytes > 0 THEN excluded.bytes ELSE bytes END,"
                             "status='unknown', msg_id=excluded.msg_id, error=excluded.error,"
                             "updated_at=excluded.updated_at"));
    a.addBindValue(fileId);
    a.addBindValue(part);
    a.addBindValue(storedPayloadPos(pos, bytes));
    a.addBindValue(storedPayloadBytes(bytes));
    a.addBindValue(msgId);
    a.addBindValue(err);
    a.addBindValue(nowIso());
    if (!a.exec()) {
        setError(error, a);
        return false;
    }
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE post_article_attempts SET status='unknown',"
                             "error=?, finished_at=? WHERE file_id=? AND part=?"
                             " AND msg_id=? AND status='posting'"));
    q.addBindValue(err);
    q.addBindValue(nowIso());
    q.addBindValue(fileId);
    q.addBindValue(part);
    q.addBindValue(msgId);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::applyArticleEvents(const QList<ArticleEvent> &events, QString *error)
{
    if (events.isEmpty())
        return true;
    if (!initialize(error))
        return false;

    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    if (!db.transaction()) {
        setError(error, db);
        return false;
    }

    QSqlQuery postingArticle(db);
    QSqlQuery postingAttempt(db);
    QSqlQuery postedArticle(db);
    QSqlQuery postedAttempt(db);
    QSqlQuery failedArticle(db);
    QSqlQuery failedAttempt(db);
    QSqlQuery unknownArticle(db);
    QSqlQuery unknownAttempt(db);

    auto prepare = [&](QSqlQuery &q, const QString &sql) {
        if (q.prepare(sql))
            return true;
        setError(error, q);
        db.rollback();
        return false;
    };

    if (!prepare(postingArticle,
                 QStringLiteral("INSERT INTO post_articles(file_id, part, pos, bytes,"
                                "status, msg_id, error, updated_at, body_bytes)"
                                "VALUES(?, ?, ?, ?, 'posting', ?, '', ?, ?)"
                                "ON CONFLICT(file_id, part) DO UPDATE SET "
                                "pos=CASE WHEN excluded.bytes > 0 THEN excluded.pos ELSE pos END,"
                                "bytes=CASE WHEN excluded.bytes > 0 THEN excluded.bytes ELSE bytes END,"
                                "body_bytes=CASE WHEN excluded.body_bytes > 0"
                                " THEN excluded.body_bytes ELSE body_bytes END,"
                                "status='posting', msg_id=excluded.msg_id,"
                                "updated_at=excluded.updated_at "
                                "WHERE post_articles.status!='posted'"))
        || !prepare(postingAttempt,
                    QStringLiteral("INSERT INTO post_article_attempts(file_id, part,"
                                   "attempt_no, msg_id, status, created_at)"
                                   "VALUES(?, ?, ?, ?, 'posting', ?)"))
        || !prepare(postedArticle,
                    QStringLiteral("INSERT INTO post_articles(file_id, part, pos, bytes,"
                                   "status, msg_id, error, updated_at, body_bytes)"
                                   "VALUES(?, ?, ?, ?, 'posted', ?, '', ?, ?)"
                                   "ON CONFLICT(file_id, part) DO UPDATE SET "
                                   "pos=CASE WHEN excluded.bytes > 0 THEN excluded.pos ELSE pos END,"
                                   "bytes=CASE WHEN excluded.bytes > 0 THEN excluded.bytes ELSE bytes END,"
                                   "body_bytes=CASE WHEN excluded.body_bytes > 0"
                                   " THEN excluded.body_bytes ELSE body_bytes END,"
                                   "status='posted', msg_id=excluded.msg_id, error='',"
                                   "updated_at=excluded.updated_at"))
        || !prepare(postedAttempt,
                    QStringLiteral("UPDATE post_article_attempts SET status='posted',"
                                   "finished_at=? WHERE file_id=? AND part=? AND msg_id=?"
                                   " AND status='posting'"))
        || !prepare(failedArticle,
                    QStringLiteral("INSERT INTO post_articles(file_id, part, pos, bytes,"
                                   "status, msg_id, error, updated_at, body_bytes)"
                                   "VALUES(?, ?, ?, ?, 'failed', ?, ?, ?, ?)"
                                   "ON CONFLICT(file_id, part) DO UPDATE SET "
                                   "pos=CASE WHEN excluded.bytes > 0 THEN excluded.pos ELSE pos END,"
                                   "bytes=CASE WHEN excluded.bytes > 0 THEN excluded.bytes ELSE bytes END,"
                                   "body_bytes=CASE WHEN excluded.body_bytes > 0"
                                   " THEN excluded.body_bytes ELSE body_bytes END,"
                                   "status='failed', msg_id=excluded.msg_id, error=excluded.error,"
                                   "updated_at=excluded.updated_at "
                                   "WHERE post_articles.status!='posted'"))
        || !prepare(failedAttempt,
                    QStringLiteral("UPDATE post_article_attempts SET status='failed',"
                                   "error=?, finished_at=? WHERE file_id=? AND part=?"
                                   " AND msg_id=? AND status='posting'"))
        || !prepare(unknownArticle,
                    QStringLiteral("INSERT INTO post_articles(file_id, part, pos, bytes,"
                                   "status, msg_id, error, updated_at, body_bytes)"
                                   "VALUES(?, ?, ?, ?, 'unknown', ?, ?, ?, ?)"
                                   "ON CONFLICT(file_id, part) DO UPDATE SET "
                                   "pos=CASE WHEN excluded.bytes > 0 THEN excluded.pos ELSE pos END,"
                                   "bytes=CASE WHEN excluded.bytes > 0 THEN excluded.bytes ELSE bytes END,"
                                   "body_bytes=CASE WHEN excluded.body_bytes > 0"
                                   " THEN excluded.body_bytes ELSE body_bytes END,"
                                   "status='unknown', msg_id=excluded.msg_id, error=excluded.error,"
                                   "updated_at=excluded.updated_at "
                                   "WHERE post_articles.status!='posted'"))
        || !prepare(unknownAttempt,
                    QStringLiteral("UPDATE post_article_attempts SET status='unknown',"
                                   "error=?, finished_at=? WHERE file_id=? AND part=?"
                                   " AND msg_id=? AND status='posting'"))) {
        return false;
    }

    auto execPrepared = [&](QSqlQuery &q) {
        if (q.exec())
            return true;
        setError(error, q);
        db.rollback();
        return false;
    };

    for (const ArticleEvent &event : events) {
        if (!event.fileId || event.part <= 0)
            continue;

        const QString stamp = nowIso();
        switch (event.kind) {
        case ArticleEvent::Kind::Posting:
            postingArticle.bindValue(0, event.fileId);
            postingArticle.bindValue(1, event.part);
            postingArticle.bindValue(2, storedPayloadPos(event.pos, event.bytes));
            postingArticle.bindValue(3, storedPayloadBytes(event.bytes));
            postingArticle.bindValue(4, event.msgId);
            postingArticle.bindValue(5, stamp);
            postingArticle.bindValue(6, storedPayloadBytes(event.bodyBytes));
            if (!execPrepared(postingArticle))
                return false;

            postingAttempt.bindValue(0, event.fileId);
            postingAttempt.bindValue(1, event.part);
            postingAttempt.bindValue(2, event.attemptNo);
            postingAttempt.bindValue(3, event.msgId);
            postingAttempt.bindValue(4, stamp);
            if (!execPrepared(postingAttempt))
                return false;
            break;

        case ArticleEvent::Kind::Posted:
            postedArticle.bindValue(0, event.fileId);
            postedArticle.bindValue(1, event.part);
            postedArticle.bindValue(2, storedPayloadPos(event.pos, event.bytes));
            postedArticle.bindValue(3, storedPayloadBytes(event.bytes));
            postedArticle.bindValue(4, event.msgId);
            postedArticle.bindValue(5, stamp);
            postedArticle.bindValue(6, storedPayloadBytes(event.bodyBytes));
            if (!execPrepared(postedArticle))
                return false;

            postedAttempt.bindValue(0, stamp);
            postedAttempt.bindValue(1, event.fileId);
            postedAttempt.bindValue(2, event.part);
            postedAttempt.bindValue(3, event.msgId);
            if (!execPrepared(postedAttempt))
                return false;
            break;

        case ArticleEvent::Kind::Failed:
            failedArticle.bindValue(0, event.fileId);
            failedArticle.bindValue(1, event.part);
            failedArticle.bindValue(2, storedPayloadPos(event.pos, event.bytes));
            failedArticle.bindValue(3, storedPayloadBytes(event.bytes));
            failedArticle.bindValue(4, event.msgId);
            failedArticle.bindValue(5, event.error);
            failedArticle.bindValue(6, stamp);
            failedArticle.bindValue(7, storedPayloadBytes(event.bodyBytes));
            if (!execPrepared(failedArticle))
                return false;

            failedAttempt.bindValue(0, event.error);
            failedAttempt.bindValue(1, stamp);
            failedAttempt.bindValue(2, event.fileId);
            failedAttempt.bindValue(3, event.part);
            failedAttempt.bindValue(4, event.msgId);
            if (!execPrepared(failedAttempt))
                return false;
            break;

        case ArticleEvent::Kind::Unknown:
            unknownArticle.bindValue(0, event.fileId);
            unknownArticle.bindValue(1, event.part);
            unknownArticle.bindValue(2, storedPayloadPos(event.pos, event.bytes));
            unknownArticle.bindValue(3, storedPayloadBytes(event.bytes));
            unknownArticle.bindValue(4, event.msgId);
            unknownArticle.bindValue(5, event.error);
            unknownArticle.bindValue(6, stamp);
            unknownArticle.bindValue(7, storedPayloadBytes(event.bodyBytes));
            if (!execPrepared(unknownArticle))
                return false;

            unknownAttempt.bindValue(0, event.error);
            unknownAttempt.bindValue(1, stamp);
            unknownAttempt.bindValue(2, event.fileId);
            unknownAttempt.bindValue(3, event.part);
            unknownAttempt.bindValue(4, event.msgId);
            if (!execPrepared(unknownAttempt))
                return false;
            break;
        }
    }

    if (!db.commit()) {
        setError(error, db);
        return false;
    }
    return true;
}

bool PostHistoryStore::markPostCrashedArticlesUnknown(QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("UPDATE post_articles SET status='unknown',"
                               "error='application stopped before confirmation',"
                               "updated_at=datetime('now') WHERE status='posting'"))) {
        setError(error, q);
        return false;
    }
    QSqlQuery a(db);
    if (!a.exec(QStringLiteral("UPDATE post_article_attempts SET status='unknown',"
                               "error='application stopped before confirmation',"
                               "finished_at=datetime('now') WHERE status='posting'"))) {
        setError(error, a);
        return false;
    }
    QSqlQuery empty(db);
    if (!empty.exec(QStringLiteral(
            "UPDATE posts SET status='cancelled', resume_state=NULL,"
            " resume_reason='posting never started; nothing to resume',"
            " finished_at=COALESCE(finished_at, datetime('now'))"
            " WHERE status='posting'"
            " AND NOT EXISTS (SELECT 1 FROM post_files WHERE post_id=posts.id)"))) {
        setError(error, empty);
        return false;
    }

    // Posts still in 'posting' status survived a crash. They are resumable only
    // once at least one file row exists; zero-file posts never reached upload.
    QSqlQuery p(db);
    if (!p.exec(QStringLiteral(
            "UPDATE posts SET status='resumable', resume_state='resumable',"
            " resume_reason='application stopped while posting'"
            " WHERE status='posting'"
            " AND EXISTS (SELECT 1 FROM post_files WHERE post_id=posts.id)"))) {
        setError(error, p);
        return false;
    }
    return true;
}

bool PostHistoryStore::cleanupInvalidResumePosts(QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "UPDATE posts SET status='cancelled', resume_state=NULL,"
            " resume_reason='posting never started; nothing to resume',"
            " finished_at=COALESCE(finished_at, datetime('now'))"
            " WHERE (status IN ('posting','resumable') OR resume_state='resumable')"
            " AND NOT EXISTS (SELECT 1 FROM post_files WHERE post_id=posts.id)"))) {
        setError(error, q);
        return false;
    }
    return true;
}

namespace {

PostHistoryStore::PostSummary summaryFromQuery(const QSqlQuery &q, bool forceResumable = false)
{
    PostHistoryStore::PostSummary s;
    s.id               = valueI64(q,  "id");
    s.nzbName          = valueString(q, "nzb_name");
    s.nzbPath          = valueString(q, "nzb_path");
    s.status           = valueString(q, "status");
    s.groups           = valueString(q, "groups_text");
    s.createdAt        = valueString(q, "created_at");
    s.finishedAt       = valueString(q, "finished_at");
    s.sizeBytes        = valueI64(q,  "size_bytes");
    s.nbFiles          = valueInt(q,  "nb_files");
    s.nbArticles       = valueInt(q,  "nb_articles");
    s.nbFailedArticles = valueInt(q,  "nb_failed_articles");
    s.hasPassword      = valueBool(q, "has_password");
    s.passwordStored   = valueBool(q, "password_stored");
    s.resumable        = forceResumable ||
                         valueString(q, "resume_state") == QStringLiteral("resumable");
    s.resumeReason     = valueString(q, "resume_reason");
    s.avgSpeed         = valueString(q, "avg_speed");
    return s;
}

void appendListFilterSql(QString *sql, const PostHistoryStore::ListFilter &f)
{
    if (!f.status.isEmpty())
        *sql += QStringLiteral(" AND p.status=:status");
    if (!f.search.isEmpty())
        *sql += QStringLiteral(
                    " AND (p.nzb_name LIKE :search OR p.rar_name LIKE :search "
                    "OR p.nzb_path LIKE :search)");
    if (!f.group.isEmpty())
        *sql += QStringLiteral(
                    " AND EXISTS (SELECT 1 FROM post_groups pg "
                    "WHERE pg.post_id=p.id AND pg.group_name=:group)");
    if (f.onlyWithPassword)
        *sql += QStringLiteral(" AND p.has_password=1");
    if (f.onlyWithErrors)
        *sql += QStringLiteral(" AND p.nb_failed_articles>0");
    if (!f.dateFrom.isEmpty())
        *sql += QStringLiteral(" AND p.created_at>=:dateFrom");
    if (!f.dateTo.isEmpty())
        *sql += QStringLiteral(" AND p.created_at<=:dateTo");
}

void bindListFilterValues(QSqlQuery *q, const PostHistoryStore::ListFilter &f)
{
    if (!f.status.isEmpty())
        q->bindValue(QStringLiteral(":status"), f.status);
    if (!f.search.isEmpty())
        q->bindValue(QStringLiteral(":search"), QStringLiteral("%%1%").arg(f.search));
    if (!f.group.isEmpty())
        q->bindValue(QStringLiteral(":group"), f.group);
    if (!f.dateFrom.isEmpty())
        q->bindValue(QStringLiteral(":dateFrom"), f.dateFrom);
    if (!f.dateTo.isEmpty())
        q->bindValue(QStringLiteral(":dateTo"), f.dateTo + QStringLiteral("T23:59:59"));
}

} // anonymous namespace

QList<PostHistoryStore::PostSummary> PostHistoryStore::listPosts(const ListFilter &f,
                                                                 QString *error)
{
    QList<PostSummary> out;
    if (!initialize(error))
        return out;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);

    QString sql = QStringLiteral(
        "SELECT p.*, COALESCE(group_concat(DISTINCT g.group_name), '') AS groups_text "
        "FROM posts p LEFT JOIN post_groups g ON g.post_id=p.id WHERE 1=1");

    appendListFilterSql(&sql, f);

    sql += QStringLiteral(" GROUP BY p.id ORDER BY p.created_at DESC, p.id DESC");
    if (f.limit > 0)
        sql += QStringLiteral(" LIMIT :limit OFFSET :offset");

    QSqlQuery q(db);
    if (!q.prepare(sql)) {
        setError(error, q);
        return out;
    }
    bindListFilterValues(&q, f);
    if (f.limit > 0) {
        q.bindValue(QStringLiteral(":limit"), f.limit);
        q.bindValue(QStringLiteral(":offset"), f.offset > 0 ? f.offset : 0);
    }
    if (!q.exec()) {
        setError(error, q);
        return out;
    }
    while (q.next())
        out << summaryFromQuery(q);
    if (q.lastError().isValid()) {
        setError(error, q);
        out.clear();
    }
    q.finish();
    return out;
}

bool PostHistoryStore::hasPostsAfter(const ListFilter &f, QString *error)
{
    if (!initialize(error))
        return false;
    if (f.limit <= 0)
        return false;

    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);

    QString sql = QStringLiteral("SELECT 1 FROM posts p WHERE 1=1");
    appendListFilterSql(&sql, f);
    sql += QStringLiteral(" ORDER BY p.created_at DESC, p.id DESC LIMIT 1 OFFSET :offset");

    QSqlQuery q(db);
    if (!q.prepare(sql)) {
        setError(error, q);
        return false;
    }
    bindListFilterValues(&q, f);
    q.bindValue(QStringLiteral(":offset"), (f.offset > 0 ? f.offset : 0) + f.limit);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }

    const bool hasMore = q.next();
    if (q.lastError().isValid()) {
        setError(error, q);
        return false;
    }
    q.finish();
    return hasMore;
}

QList<PostHistoryStore::PostSummary> PostHistoryStore::listPosts(const QString &status,
                                                                 const QString &search,
                                                                 bool onlyWithPassword,
                                                                 QString *error)
{
    ListFilter f;
    f.status           = status;
    f.search           = search;
    f.onlyWithPassword = onlyWithPassword;
    return listPosts(f, error);
}

QList<PostHistoryStore::PostSummary> PostHistoryStore::resumeCandidates(QString *error)
{
    QList<PostSummary> out;
    if (!initialize(error))
        return out;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT p.*, COALESCE(group_concat(DISTINCT g.group_name), '') AS groups_text "
            "FROM posts p LEFT JOIN post_groups g ON g.post_id=p.id "
            "WHERE (p.resume_state='resumable' OR p.status IN ('partial','resumable','unknown')) "
            "AND EXISTS (SELECT 1 FROM post_files f WHERE f.post_id=p.id) "
            "AND EXISTS (SELECT 1 FROM post_files f WHERE f.post_id=p.id "
            "  AND (f.total_articles > (SELECT COUNT(*) FROM post_articles a WHERE a.file_id=f.id) "
            "       OR EXISTS (SELECT 1 FROM post_articles a WHERE a.file_id=f.id "
            "                  AND a.status!='posted'))) "
            "GROUP BY p.id ORDER BY p.created_at DESC"))) {
        setError(error, q);
        return out;
    }
    while (q.next())
        out << summaryFromQuery(q, /*forceResumable=*/true);
    if (q.lastError().isValid()) {
        setError(error, q);
        out.clear();
    }
    q.finish();
    return out;
}

QList<PostHistoryStore::DayStats> PostHistoryStore::statsByDay(const QString &dateFrom,
                                                               const QString &dateTo,
                                                               const QString &group,
                                                               QString *error)
{
    QList<DayStats> out;
    if (!initialize(error))
        return out;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);

    QString sql = QStringLiteral(
        "SELECT substr(p.created_at,1,10) AS day,"
        " COUNT(*) AS nb_posts,"
        " SUM(p.nb_failed_articles) AS nb_failed,"
        " SUM(p.size_bytes) AS total_bytes,"
        " CASE WHEN SUM(CASE WHEN p.finished_at IS NOT NULL AND p.started_at IS NOT NULL"
        "   THEN CAST(strftime('%s',p.finished_at) AS REAL)"
        "      - CAST(strftime('%s',p.started_at) AS REAL) ELSE 0 END) > 0"
        " THEN SUM(CASE WHEN p.finished_at IS NOT NULL THEN CAST(p.size_bytes AS REAL) ELSE 0 END)"
        "    / SUM(CASE WHEN p.finished_at IS NOT NULL AND p.started_at IS NOT NULL"
        "        THEN CAST(strftime('%s',p.finished_at) AS REAL)"
        "           - CAST(strftime('%s',p.started_at) AS REAL) ELSE 0 END)"
        " ELSE 0 END AS avg_speed_bps"
        " FROM posts p WHERE 1=1");

    if (!dateFrom.isEmpty())
        sql += QStringLiteral(" AND p.created_at>=:dateFrom");
    if (!dateTo.isEmpty())
        sql += QStringLiteral(" AND p.created_at<=:dateTo");
    if (!group.isEmpty())
        sql += QStringLiteral(
                   " AND EXISTS (SELECT 1 FROM post_groups pg"
                   " WHERE pg.post_id=p.id AND pg.group_name=:group)");

    sql += QStringLiteral(" GROUP BY day ORDER BY day ASC");

    QSqlQuery q(db);
    if (!q.prepare(sql)) {
        setError(error, q);
        return out;
    }
    if (!dateFrom.isEmpty())
        q.bindValue(QStringLiteral(":dateFrom"), dateFrom);
    if (!dateTo.isEmpty())
        q.bindValue(QStringLiteral(":dateTo"), dateTo + QStringLiteral("T23:59:59"));
    if (!group.isEmpty())
        q.bindValue(QStringLiteral(":group"), group);
    if (!q.exec()) {
        setError(error, q);
        return out;
    }
    while (q.next()) {
        DayStats d;
        d.date        = q.value(0).toString();
        d.nbPosts     = q.value(1).toInt();
        d.nbFailed    = q.value(2).toInt();
        d.totalBytes  = q.value(3).toLongLong();
        d.avgSpeedBps = q.value(4).toDouble();
        out << d;
    }
    if (q.lastError().isValid()) {
        setError(error, q);
        out.clear();
    }
    q.finish();
    return out;
}

QList<PostHistoryStore::GroupStats> PostHistoryStore::statsByGroup(const QString &dateFrom,
                                                                   const QString &dateTo,
                                                                   QString *error)
{
    QList<GroupStats> out;
    if (!initialize(error))
        return out;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);

    QString sql = QStringLiteral(
        "SELECT g.group_name, COUNT(DISTINCT p.id) AS nb_posts, SUM(p.size_bytes) AS total_bytes"
        " FROM post_groups g JOIN posts p ON p.id=g.post_id WHERE 1=1");

    if (!dateFrom.isEmpty())
        sql += QStringLiteral(" AND p.created_at>=:dateFrom");
    if (!dateTo.isEmpty())
        sql += QStringLiteral(" AND p.created_at<=:dateTo");

    sql += QStringLiteral(" GROUP BY g.group_name ORDER BY nb_posts DESC");

    QSqlQuery q(db);
    if (!q.prepare(sql)) {
        setError(error, q);
        return out;
    }
    if (!dateFrom.isEmpty())
        q.bindValue(QStringLiteral(":dateFrom"), dateFrom);
    if (!dateTo.isEmpty())
        q.bindValue(QStringLiteral(":dateTo"), dateTo + QStringLiteral("T23:59:59"));
    if (!q.exec()) {
        setError(error, q);
        return out;
    }
    while (q.next()) {
        GroupStats s;
        s.group      = q.value(0).toString();
        s.nbPosts    = q.value(1).toInt();
        s.totalBytes = q.value(2).toLongLong();
        out << s;
    }
    if (q.lastError().isValid()) {
        setError(error, q);
        out.clear();
    }
    q.finish();
    return out;
}

QList<PostHistoryStore::PostSummary> PostHistoryStore::topPostsBySize(int n, QString *error)
{
    QList<PostSummary> out;
    if (!initialize(error))
        return out;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT p.*, COALESCE(group_concat(DISTINCT g.group_name), '') AS groups_text"
            " FROM posts p LEFT JOIN post_groups g ON g.post_id=p.id"
            " WHERE p.status != 'posting'"
            " GROUP BY p.id ORDER BY p.size_bytes DESC LIMIT %1").arg(n))) {
        setError(error, q);
        return out;
    }
    while (q.next())
        out << summaryFromQuery(q);
    if (q.lastError().isValid()) {
        setError(error, q);
        out.clear();
    }
    q.finish();
    return out;
}

QStringList PostHistoryStore::allGroups(QString *error)
{
    QStringList out;
    if (!initialize(error))
        return out;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT DISTINCT group_name FROM post_groups ORDER BY group_name"))) {
        setError(error, q);
        return out;
    }
    while (q.next())
        out << q.value(0).toString();
    if (q.lastError().isValid()) {
        setError(error, q);
        out.clear();
    }
    q.finish();
    return out;
}

bool PostHistoryStore::deletePost(qint64 postId, QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM posts WHERE id=?"));
    q.addBindValue(postId);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

PostInfoData PostHistoryStore::PostInfoRecord::toPostInfoData() const
{
    // The database stores UTC; everything a user reads is local time.
    auto localTime = [](const QString &iso) {
        QDateTime dt = QDateTime::fromString(iso, Qt::ISODate);
        return dt.isValid() ? dt.toLocalTime() : QDateTime();
    };

    PostInfoData data;
    data.nzbPath = nzbPath;
    if (nzbPath.isEmpty()) {
        const QFileInfo named(post.nzbName);
        data.nzbDir.clear();
        data.nzbName     = named.completeBaseName();
        data.nzbFileName = named.fileName();
    } else {
        const QFileInfo onDisk(nzbPath);
        data.nzbDir      = onDisk.absolutePath();
        data.nzbName     = onDisk.completeBaseName();
        data.nzbFileName = onDisk.fileName();
        if (data.nzbName.isEmpty())
            data.nzbName = QFileInfo(post.nzbName).completeBaseName();
    }

    data.rarName   = rarName;
    data.rarPass   = rarPass;
    data.groups    = post.groups;
    data.nzbPoster = from;
    data.status    = post.status;
    data.avgSpeed  = post.avgSpeed;

    data.originalPath = info.sourcePath.isEmpty() ? QString()
                                                  : QFileInfo(info.sourcePath).absolutePath();
    data.sourcePath   = info.sourcePath;
    data.originalName = info.originalName;
    data.appVersion   = info.appVersion;

    data.postSizeBytes = info.postSizeBytes; // < 0 stays "not recorded"
    data.legacySizeBytes = post.sizeBytes < 0 ? 0 : static_cast<quint64>(post.sizeBytes);
    data.par2Pct         = info.par2Pct;
    data.nbFiles          = static_cast<uint>(post.nbFiles < 0 ? 0 : post.nbFiles);
    data.nbArticles       = static_cast<uint>(post.nbArticles < 0 ? 0 : post.nbArticles);
    data.nbArticlesFailed = static_cast<uint>(post.nbFailedArticles < 0 ? 0 : post.nbFailedArticles);
    data.nbArticlesPosted = static_cast<uint>(nbArticlesPosted < 0 ? 0 : nbArticlesPosted);
    data.durationSec   = info.activeSeconds < 0 ? 0 : info.activeSeconds;
    data.historyPostId = post.id;

    // Old rows had no post_info/real start timestamp, so created_at is the
    // only best-effort value available. A current row with an empty started_at
    // really never began transferring and must stay empty.
    data.startedAt  = localTime(partial && startedAt.isEmpty() ? post.createdAt : startedAt);
    data.finishedAt = localTime(post.finishedAt);

    data.meta    = meta;
    data.inputPaths = filePaths;
    data.partial = partial;
    return data;
}

bool PostHistoryStore::loadPostInfoRecord(qint64 postId, PostInfoRecord *record, QString *error)
{
    if (!record)
        return false;
    *record = PostInfoRecord();
    if (!initialize(error))
        return false;

    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery p(db);
    // Deliberately no join on post_files/post_articles: describing a post does
    // not need them, and a big post has hundreds of thousands of article rows.
    p.prepare(QStringLiteral("SELECT p.*, COALESCE(group_concat(g.group_name, ','), '') AS groups_text,"
                             "(SELECT COUNT(*) FROM post_files f "
                             " JOIN post_articles a ON a.file_id=f.id "
                             " WHERE f.post_id=p.id AND a.status='posted') AS nb_articles_posted,"
                             "i.post_id AS info_id, i.par2_pct, i.post_size_bytes,"
                             "i.active_seconds, i.article_size_bytes, i.source_path,"
                             "i.original_name, i.app_version "
                             "FROM posts p "
                             "LEFT JOIN post_groups g ON g.post_id=p.id "
                             "LEFT JOIN post_info i ON i.post_id=p.id "
                             "WHERE p.id=? GROUP BY p.id"));
    p.addBindValue(postId);
    if (!p.exec()) {
        setError(error, p);
        return false;
    }
    if (!p.next()) {
        if (error)
            *error = QStringLiteral("history post %1 was not found").arg(postId);
        return false;
    }

    record->post.id = postId;
    record->post.nzbName = valueString(p, "nzb_name");
    record->post.status = valueString(p, "status");
    record->post.groups = valueString(p, "groups_text");
    record->post.createdAt = valueString(p, "created_at");
    record->post.finishedAt = valueString(p, "finished_at");
    record->post.avgSpeed = valueString(p, "avg_speed");
    record->post.sizeBytes = valueI64(p, "size_bytes");
    record->post.nbFiles = valueInt(p, "nb_files");
    record->post.nbArticles = valueInt(p, "nb_articles");
    record->post.nbFailedArticles = valueInt(p, "nb_failed_articles");
    record->post.hasPassword = valueBool(p, "has_password");
    record->post.passwordStored = valueBool(p, "password_stored");
    record->startedAt = valueString(p, "started_at");
    record->nzbPath = valueString(p, "nzb_path");
    record->rarName = valueString(p, "rar_name");
    record->rarPass = valueString(p, "rar_pass");
    record->from = valueString(p, "from_addr");
    record->nbArticlesPosted = valueInt(p, "nb_articles_posted");

    // No post_info row: the post was made before this ngPost version. Its
    // facts cannot be reconstructed, they stay empty and the record says so.
    record->partial = p.value(p.record().indexOf(QStringLiteral("info_id"))).isNull();
    if (!record->partial) {
        const QVariant par2 = p.value(p.record().indexOf(QStringLiteral("par2_pct")));
        const QVariant size = p.value(p.record().indexOf(QStringLiteral("post_size_bytes")));
        const QVariant active = p.value(p.record().indexOf(QStringLiteral("active_seconds")));
        const QVariant articleSize =
            p.value(p.record().indexOf(QStringLiteral("article_size_bytes")));
        record->info.par2Pct = par2.isNull() ? -1 : par2.toInt();
        record->info.postSizeBytes = size.isNull() ? -1 : size.toLongLong();
        record->info.activeSeconds = active.isNull() ? -1 : active.toLongLong();
        record->info.articleSizeBytes = articleSize.isNull() ? -1 : articleSize.toLongLong();
        record->info.sourcePath = valueString(p, "source_path");
        record->info.originalName = valueString(p, "original_name");
        record->info.appVersion = valueString(p, "app_version");
    }
    p.finish();

    QSqlQuery m(db);
    m.prepare(QStringLiteral("SELECT key, value, scope FROM post_meta WHERE post_id=?"));
    m.addBindValue(postId);
    if (!m.exec()) {
        setError(error, m);
        return false;
    }
    while (m.next()) {
        const MetaScope scope = m.value(2).toString() == QStringLiteral("nzb") ? MetaScope::Nzb
                                                                              : MetaScope::Local;
        record->meta.insert(m.value(0).toString(), MetaValue(m.value(1).toString(), scope));
    }
    m.finish();

    QSqlQuery files(db);
    files.prepare(QStringLiteral("SELECT original_path FROM post_files WHERE post_id=? "
                                 "ORDER BY ordinal"));
    files.addBindValue(postId);
    if (!files.exec()) {
        setError(error, files);
        return false;
    }
    while (files.next()) {
        const QString path = files.value(0).toString();
        if (!path.isEmpty())
            record->filePaths << path;
    }
    if (files.lastError().isValid()) {
        setError(error, files);
        record->filePaths.clear();
        return false;
    }
    return true;
}

bool PostHistoryStore::loadPostDetails(qint64 postId, PostDetails *details, QString *error)
{
    if (!details)
        return false;
    *details = PostDetails();
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery p(db);
    p.prepare(QStringLiteral("SELECT p.*, COALESCE(group_concat(g.group_name, ','), '') AS groups_text,"
                             "i.par2_pct AS info_par2_pct,"
                             "i.article_size_bytes AS info_article_size_bytes "
                             "FROM posts p LEFT JOIN post_groups g ON g.post_id=p.id "
                             "LEFT JOIN post_info i ON i.post_id=p.id "
                             "WHERE p.id=? GROUP BY p.id"));
    p.addBindValue(postId);
    if (!p.exec()) {
        setError(error, p);
        return false;
    }
    if (!p.next()) {
        if (error)
            *error = QStringLiteral("history post %1 was not found").arg(postId);
        return false;
    }
    details->post.id = postId;
    details->post.nzbName = valueString(p, "nzb_name");
    details->post.status = valueString(p, "status");
    details->post.groups = valueString(p, "groups_text");
    details->post.createdAt = valueString(p, "created_at");
    details->post.finishedAt = valueString(p, "finished_at");
    details->post.sizeBytes = valueI64(p, "size_bytes");
    details->post.nbFiles = valueInt(p, "nb_files");
    details->post.nbArticles = valueInt(p, "nb_articles");
    details->post.nbFailedArticles = valueInt(p, "nb_failed_articles");
    details->post.hasPassword = valueBool(p, "has_password");
    details->post.passwordStored = valueBool(p, "password_stored");
    details->post.resumable = valueString(p, "resume_state") == QStringLiteral("resumable");
    details->post.resumeReason = valueString(p, "resume_reason");
    details->nzbPath = valueString(p, "nzb_path");
    details->rarName = valueString(p, "rar_name");
    details->rarPass = valueString(p, "rar_pass");
    details->passwordOrigin = valueString(p, "password_origin");
    details->from = valueString(p, "from_addr");
    details->obfuscateArticles = valueBool(p, "obfuscate_articles");
    details->obfuscateFileName = valueBool(p, "obfuscate_file_name");
    details->doCompress = valueBool(p, "do_compress");
    details->doPar2 = valueBool(p, "do_par2");
    {
        const QVariant par2 = p.value(p.record().indexOf(QStringLiteral("info_par2_pct")));
        details->par2Pct = par2.isNull() ? -1 : par2.toInt();
        const QVariant articleSize =
            p.value(p.record().indexOf(QStringLiteral("info_article_size_bytes")));
        details->articleSizeWasStored = !articleSize.isNull();
        details->articleSizeBytes = articleSize.isNull() ? -1 : articleSize.toLongLong();
    }
    p.finish();

    QSqlQuery f(db);
    f.prepare(QStringLiteral("SELECT * FROM post_files WHERE post_id=? ORDER BY ordinal"));
    f.addBindValue(postId);
    if (!f.exec()) {
        setError(error, f);
        return false;
    }
    while (f.next()) {
        FileSummary fs;
        fs.id = valueI64(f, "id");
        fs.ordinal = valueInt(f, "ordinal");
        fs.originalPath = valueString(f, "original_path");
        fs.postedName = valueString(f, "posted_name");
        fs.sizeBytes = valueI64(f, "size_bytes");
        fs.mtimeEpoch = valueI64(f, "mtime_epoch");
        fs.totalArticles = valueInt(f, "total_articles");
        fs.groups = valueString(f, "groups_text");
        fs.status = valueString(f, "status");
        details->files << fs;
    }
    if (f.lastError().isValid()) {
        setError(error, f);
        f.finish();
        return false;
    }
    f.finish();

    for (const FileSummary &fs : std::as_const(details->files)) {
        QSqlQuery a(db);
        a.prepare(QStringLiteral("SELECT * FROM post_articles WHERE file_id=? ORDER BY part"));
        a.addBindValue(fs.id);
        if (!a.exec()) {
            setError(error, a);
            return false;
        }
        while (a.next()) {
            ArticleSummary as;
            as.fileId = fs.id;
            as.part = valueInt(a, "part");
            as.pos = valueI64(a, "pos");
            as.bytes = valueI64(a, "bytes");
            as.bodyBytes = valueI64(a, "body_bytes");
            as.msgId = valueString(a, "msg_id");
            as.status = valueString(a, "status");
            details->articlesByFile[fs.id] << as;
        }
        if (a.lastError().isValid()) {
            setError(error, a);
            a.finish();
            return false;
        }
        a.finish();
    }
    details->articleSizeBytes = resolveArticleSizeBytes(*details, details->articleSizeBytes);
    return true;
}

bool PostHistoryStore::exportCsv(QTextStream &stream, bool includePasswords, QString *error)
{
    const QList<PostSummary> posts = listPosts(QString(), QString(), false, error);
    if (error && !error->isEmpty())
        return false;
    stream << "id;date;nzb name;status;size;avg. speed;archive name;archive pass;groups;from\n";
    for (const PostSummary &summary : posts) {
        PostDetails details;
        QString err;
        if (!loadPostDetails(summary.id, &details, &err))
            continue;
        stream << summary.id << ';'
               << summary.createdAt << ';'
               << summary.nzbName << ';'
               << summary.status << ';'
               << summary.sizeBytes << ';'
               << "" << ';'
               << details.rarName << ';'
               << (includePasswords ? details.rarPass : (summary.hasPassword ? "***" : "")) << ';'
               << summary.groups << ';'
               << details.from << '\n';
    }
    return true;
}

bool PostHistoryStore::importLegacyCsv(const QString &path, QString *error)
{
    if (!initialize(error))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("cannot open legacy csv: %1").arg(path);
        return false;
    }
    QTextStream in(&file);
    bool first = true;
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.trimmed().isEmpty())
            continue;
        const QStringList cols = line.split(';');
        if (first && cols.value(0).contains(QStringLiteral("date"), Qt::CaseInsensitive)) {
            first = false;
            continue;
        }
        first = false;
        PostRecord rec;
        rec.nzbName = cols.value(1);
        rec.rarName = cols.value(4);
        rec.rarPass = cols.value(5);
        rec.hasPassword = !rec.rarPass.isEmpty();
        rec.passwordOrigin = rec.hasPassword ? QStringLiteral("legacy_csv") : QStringLiteral("absent");
        rec.groups = cols.value(6).split(',', Qt::SkipEmptyParts);
        rec.from = cols.value(7);
        const qint64 postId = createPost(rec, error);
        if (!postId)
            return false;
        updatePostStatus(postId, QStringLiteral("success"), 0, 0, 0, 0, cols.value(3), error);
    }
    return true;
}
