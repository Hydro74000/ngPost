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
        QStringLiteral("INSERT OR REPLACE INTO schema_meta(key, value) "
                       "VALUES('version', '1')"),
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
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_post ON post_files(post_id)")
    };

    for (const QString &sql : statements) {
        QSqlQuery q(db);
        if (!q.exec(sql)) {
            setError(error, q);
            db.rollback();
            return false;
        }
    }

    if (!db.commit()) {
        setError(error, db);
        return false;
    }
    return true;
}

qint64 PostHistoryStore::createPost(const PostRecord &record, QString *error)
{
    if (!initialize(error))
        return 0;

    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    if (!db.transaction()) {
        setError(error, db);
        return 0;
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT INTO posts("
                             "created_at, started_at, nzb_name, nzb_path, status,"
                             "rar_name, rar_pass, has_password, password_stored,"
                             "password_origin, from_addr, do_compress, do_par2,"
                             "obfuscate_articles, obfuscate_file_name, resume_state)"
                             "VALUES(?, ?, ?, ?, 'posting', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'resumable')"));
    const bool passwordStored = _storePasswords && !record.rarPass.isEmpty();
    q.addBindValue(nowIso());
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

    if (!db.commit()) {
        setError(error, db);
        return 0;
    }
    return postId;
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
    q.addBindValue((status == QStringLiteral("success")) ? QString() : QStringLiteral("resumable"));
    q.addBindValue((status == QStringLiteral("success")) ? QString() : QStringLiteral("failed or unknown articles remain"));
    q.addBindValue(postId);
    if (!q.exec()) {
        setError(error, q);
        return false;
    }
    return true;
}

bool PostHistoryStore::markPostResuming(qint64 postId, QString *error)
{
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE posts SET status='posting', finished_at=NULL,"
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
                                "status, msg_id, error, updated_at)"
                                "VALUES(?, ?, ?, ?, 'posting', ?, '', ?)"
                                "ON CONFLICT(file_id, part) DO UPDATE SET "
                                "pos=CASE WHEN excluded.bytes > 0 THEN excluded.pos ELSE pos END,"
                                "bytes=CASE WHEN excluded.bytes > 0 THEN excluded.bytes ELSE bytes END,"
                                "status='posting', msg_id=excluded.msg_id,"
                                "updated_at=excluded.updated_at "
                                "WHERE post_articles.status!='posted'"))
        || !prepare(postingAttempt,
                    QStringLiteral("INSERT INTO post_article_attempts(file_id, part,"
                                   "attempt_no, msg_id, status, created_at)"
                                   "VALUES(?, ?, ?, ?, 'posting', ?)"))
        || !prepare(postedArticle,
                    QStringLiteral("INSERT INTO post_articles(file_id, part, pos, bytes,"
                                   "status, msg_id, error, updated_at)"
                                   "VALUES(?, ?, ?, ?, 'posted', ?, '', ?)"
                                   "ON CONFLICT(file_id, part) DO UPDATE SET "
                                   "pos=CASE WHEN excluded.bytes > 0 THEN excluded.pos ELSE pos END,"
                                   "bytes=CASE WHEN excluded.bytes > 0 THEN excluded.bytes ELSE bytes END,"
                                   "status='posted', msg_id=excluded.msg_id, error='',"
                                   "updated_at=excluded.updated_at"))
        || !prepare(postedAttempt,
                    QStringLiteral("UPDATE post_article_attempts SET status='posted',"
                                   "finished_at=? WHERE file_id=? AND part=? AND msg_id=?"
                                   " AND status='posting'"))
        || !prepare(failedArticle,
                    QStringLiteral("INSERT INTO post_articles(file_id, part, pos, bytes,"
                                   "status, msg_id, error, updated_at)"
                                   "VALUES(?, ?, ?, ?, 'failed', ?, ?, ?)"
                                   "ON CONFLICT(file_id, part) DO UPDATE SET "
                                   "pos=CASE WHEN excluded.bytes > 0 THEN excluded.pos ELSE pos END,"
                                   "bytes=CASE WHEN excluded.bytes > 0 THEN excluded.bytes ELSE bytes END,"
                                   "status='failed', msg_id=excluded.msg_id, error=excluded.error,"
                                   "updated_at=excluded.updated_at "
                                   "WHERE post_articles.status!='posted'"))
        || !prepare(failedAttempt,
                    QStringLiteral("UPDATE post_article_attempts SET status='failed',"
                                   "error=?, finished_at=? WHERE file_id=? AND part=?"
                                   " AND msg_id=? AND status='posting'"))
        || !prepare(unknownArticle,
                    QStringLiteral("INSERT INTO post_articles(file_id, part, pos, bytes,"
                                   "status, msg_id, error, updated_at)"
                                   "VALUES(?, ?, ?, ?, 'unknown', ?, ?, ?)"
                                   "ON CONFLICT(file_id, part) DO UPDATE SET "
                                   "pos=CASE WHEN excluded.bytes > 0 THEN excluded.pos ELSE pos END,"
                                   "bytes=CASE WHEN excluded.bytes > 0 THEN excluded.bytes ELSE bytes END,"
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

    if (!f.status.isEmpty())
        sql += QStringLiteral(" AND p.status=:status");
    if (!f.search.isEmpty())
        sql += QStringLiteral(
                   " AND (p.nzb_name LIKE :search OR p.rar_name LIKE :search "
                   "OR p.nzb_path LIKE :search)");
    if (!f.group.isEmpty())
        sql += QStringLiteral(
                   " AND EXISTS (SELECT 1 FROM post_groups pg "
                   "WHERE pg.post_id=p.id AND pg.group_name=:group)");
    if (f.onlyWithPassword)
        sql += QStringLiteral(" AND p.has_password=1");
    if (f.onlyWithErrors)
        sql += QStringLiteral(" AND p.nb_failed_articles>0");
    if (!f.dateFrom.isEmpty())
        sql += QStringLiteral(" AND p.created_at>=:dateFrom");
    if (!f.dateTo.isEmpty())
        sql += QStringLiteral(" AND p.created_at<=:dateTo");

    sql += QStringLiteral(" GROUP BY p.id ORDER BY p.created_at DESC");

    QSqlQuery q(db);
    if (!q.prepare(sql)) {
        setError(error, q);
        return out;
    }
    if (!f.status.isEmpty())
        q.bindValue(QStringLiteral(":status"), f.status);
    if (!f.search.isEmpty())
        q.bindValue(QStringLiteral(":search"), QStringLiteral("%%1%").arg(f.search));
    if (!f.group.isEmpty())
        q.bindValue(QStringLiteral(":group"), f.group);
    if (!f.dateFrom.isEmpty())
        q.bindValue(QStringLiteral(":dateFrom"), f.dateFrom);
    if (!f.dateTo.isEmpty())
        q.bindValue(QStringLiteral(":dateTo"), f.dateTo + QStringLiteral("T23:59:59"));
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

bool PostHistoryStore::loadPostDetails(qint64 postId, PostDetails *details, QString *error)
{
    if (!details)
        return false;
    *details = PostDetails();
    if (!initialize(error))
        return false;
    QSqlDatabase db = dbFor(_connectionName(), _dbPath, error);
    QSqlQuery p(db);
    p.prepare(QStringLiteral("SELECT p.*, COALESCE(group_concat(g.group_name, ','), '') AS groups_text "
                             "FROM posts p LEFT JOIN post_groups g ON g.post_id=p.id "
                             "WHERE p.id=? GROUP BY p.id"));
    p.addBindValue(postId);
    if (!p.exec() || !p.next()) {
        setError(error, p);
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
            as.bytes = valueI64(a, "bytes");
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
