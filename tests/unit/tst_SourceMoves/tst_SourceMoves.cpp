// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// The journaled moves of an obfuscation (plan_queue.md lot C5.7), against a
// real history database and real files. A crash is a store closed and opened
// again by the same session; recovery is sourcemoves::settle() called
// directly (restoring at startup is lot C6):
//   original alone and the same file   -> restored, nothing moved
//   staged copy alone and the same file -> renamed back, restored
//   both, neither, another file, a link -> blocked, nothing overwritten
// and nothing at all while the tools of its set are not proven stopped.
//
//========================================================================

#include "QueueTestHelpers.h"
#include "history/HistoryQueueStore.h"
#include "history/PostHistoryStore.h"
#include "queue/PostQueue.h"
#include "tools/SourceMoves.h"
#include "utils/NativeFileIdentity.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include <memory>

using namespace queue;
using queuetest::makeOptions;
using sourcemoves::Outcome;

namespace
{

bool writeFile(const QString &path, const QByteArray &content)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(content) == content.size();
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class tst_SourceMoves : public QObject
{
    Q_OBJECT

private:
    //! One queued post of session owner-A, its run and its artifact set.
    struct Fixture
    {
        QTemporaryDir dir;
        std::unique_ptr<PostHistoryStore> store;
        std::unique_ptr<HistoryQueueStore> adapter;
        std::unique_ptr<PostQueue> queue;
        qint64 post = 0;
        QString run;
        const QString set = QStringLiteral("set-1");
        QString error;

        Fixture()
        {
            store = std::make_unique<PostHistoryStore>(dbPath(), true);
            store->setOwnerId(QStringLiteral("owner-A"));
            adapter = std::make_unique<HistoryQueueStore>(store.get());
            queue = std::make_unique<PostQueue>(adapter.get(), QStringLiteral("queue-A"));
            post = queue->submit(makeOptions(QStringLiteral("a"), true), Origin::Quick, 3)
                       .postIds.value(0);
            run = queue->find(post)->runId;
            store->createArtifactSet({ set, post, run, 1, 0, dir.path(), path("arch"), {} },
                                     &error);
            QDir().mkpath(path("staging"));
        }
        QString dbPath() const { return dir.filePath(QStringLiteral("history.sqlite")); }
        QString path(const QString &name) const { return dir.filePath(name); }
        sourcemoves::Run current() const { return { post, run, set }; }

        //! As after a crash: the store closed, then opened again by its session.
        void reopen()
        {
            queue.reset();
            adapter.reset();
            store = std::make_unique<PostHistoryStore>(dbPath(), true);
            store->setOwnerId(QStringLiteral("owner-A"));
        }
        QString state(int sequence) const
        {
            return scalar(QStringLiteral("SELECT state FROM post_source_moves WHERE run_id=? "
                                         "AND sequence=?"),
                          { run, sequence })
                .toString();
        }
        //! A statement on a connection of its own (a trigger, a probe).
        QVariant scalar(const QString &sql, const QVariantList &values = {}) const
        {
            const QString name = QUuid::createUuid().toString();
            QVariant value;
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
                db.setDatabaseName(dbPath());
                if (db.open()) {
                    QSqlQuery q(db);
                    q.prepare(sql);
                    for (const QVariant &v : values)
                        q.addBindValue(v);
                    if (q.exec() && q.next())
                        value = q.value(0);
                }
            }
            QSqlDatabase::removeDatabase(name);
            return value;
        }
        bool exec(const QString &sql) const
        {
            const QString name = QUuid::createUuid().toString();
            bool done = false;
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
                db.setDatabaseName(dbPath());
                QSqlQuery q(db);
                done = db.open() && q.exec(sql);
            }
            QSqlDatabase::removeDatabase(name);
            return done;
        }
        //! The intent of a move, as recorded just before a crash.
        bool plan(int sequence, const QString &source, const QString &staged, const QString &setId)
        {
            PostHistoryStore::SourceMove move;
            move.sequence = sequence;
            move.sourcePath = source;
            move.stagedPath = staged;
            move.identityJson = NativeFileIdentity::of(source)->toJson();
            return store->planSourceMoves(post, run, setId, { move }, &error);
        }
        //! A tool of the set started: nothing proves its stop any more.
        bool toolRunning()
        {
            return store->recordToolSupervision(post,
                                                run,
                                                set,
                                                QStringLiteral("linux_subreaper"),
                                                QStringLiteral("starting"),
                                                QStringLiteral("{}"),
                                                &error)
                && store->recordToolSupervision(post,
                                                run,
                                                set,
                                                {},
                                                QStringLiteral("running"),
                                                QStringLiteral("{}"),
                                                &error);
        }
    };

private slots:
    void aMoveIsRecordedBeforeAndAfterIt()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "data"));
        const auto result = sourcemoves::moveOut(*f.store, f.current(), 0, source, staged);
        QCOMPARE(result.outcome, Outcome::Moved);
        QVERIFY(!QFileInfo::exists(source));
        QCOMPARE(readFile(staged), QByteArray("data"));
        QCOMPARE(f.state(0), QStringLiteral("moved"));
        const auto back = sourcemoves::moveBack(*f.store, f.post, f.run, 0);
        QCOMPARE(back.outcome, Outcome::Restored);
        QCOMPARE(readFile(source), QByteArray("data"));
        QVERIFY(!QFileInfo::exists(staged));
        QCOMPARE(f.state(0), QStringLiteral("restored"));
    }

    void aRefusedIntentMovesNothing()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        QVERIFY(writeFile(source, "data"));
        QVERIFY(f.exec(QStringLiteral("CREATE TRIGGER refuse BEFORE INSERT ON post_source_moves "
                                      "BEGIN SELECT RAISE(ABORT, 'injected'); END")));
        const auto result = sourcemoves::moveOut(*f.store,
                                                 f.current(),
                                                 0,
                                                 source,
                                                 f.path(QStringLiteral("staging/x1")));
        QCOMPARE(result.outcome, Outcome::Unrecorded);
        QCOMPARE(result.where, sourcemoves::Where::Source);
        QVERIFY(!result.planned);
        QCOMPARE(readFile(source), QByteArray("data"));
        QVERIFY(!QFileInfo::exists(f.path(QStringLiteral("staging/x1"))));
    }

    void anotherSessionTouchesNothing()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "data"));
        f.store->setOwnerId(QStringLiteral("owner-B"));
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, source, staged).outcome,
                 Outcome::Unrecorded);
        QVERIFY(QFileInfo::exists(source)); // its owner checked before the disk
        f.store->setOwnerId(QStringLiteral("owner-A"));
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, source, staged).outcome,
                 Outcome::Moved);
        f.store->setOwnerId(QStringLiteral("owner-B"));
        QCOMPARE(sourcemoves::moveBack(*f.store, f.post, f.run, 0).outcome, Outcome::Refused);
        QVERIFY(QFileInfo::exists(staged)); // still aside
        QCOMPARE(f.state(0), QStringLiteral("moved"));
    }

    void aCrashBeforeTheRenameSettlesAsIs()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        QVERIFY(writeFile(source, "data"));
        QVERIFY2(f.plan(0, source, f.path(QStringLiteral("staging/x1")), f.set),
                 qPrintable(f.error));
        f.reopen();
        const auto settled = sourcemoves::settle(*f.store, f.post);
        QCOMPARE(settled.restored, 1);
        QCOMPARE(readFile(source), QByteArray("data"));
        QCOMPARE(f.state(0), QStringLiteral("restored"));
    }

    void aCrashAfterTheRenameBringsItBack()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "data"));
        QVERIFY2(f.plan(0, source, staged, f.set), qPrintable(f.error));
        QVERIFY(QFile::rename(source, staged)); // moved, never acknowledged
        f.reopen();
        QCOMPARE(sourcemoves::settle(*f.store, f.post).restored, 1);
        QCOMPARE(readFile(source), QByteArray("data"));
        QVERIFY(!QFileInfo::exists(staged));
        QCOMPARE(f.state(0), QStringLiteral("restored"));
    }

    void aCrashDuringTheRollbackEndsIt()
    {
        Fixture f;
        const QString a = f.path(QStringLiteral("a.mkv"));
        const QString b = f.path(QStringLiteral("b.mkv"));
        const QString stagedA = f.path(QStringLiteral("staging/xa"));
        const QString stagedB = f.path(QStringLiteral("staging/xb"));
        QVERIFY(writeFile(a, "A") && writeFile(b, "B"));
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, a, stagedA).outcome,
                 Outcome::Moved);
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 1, b, stagedB).outcome,
                 Outcome::Moved);
        QVERIFY(QFile::rename(stagedA, a)); // put back, the crash came before its record
        f.reopen();
        const auto settled = sourcemoves::settle(*f.store, f.post);
        QCOMPARE(settled.restored, 2);
        QCOMPARE(readFile(a), QByteArray("A"));
        QCOMPARE(readFile(b), QByteArray("B"));
        QVERIFY(!QFileInfo::exists(stagedB));
    }

    void bothPresentIsBlockedAndNothingOverwritten()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "ours"));
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, source, staged).outcome,
                 Outcome::Moved);
        QVERIFY(writeFile(source, "new one")); // someone dropped a file of that name
        f.reopen();
        QCOMPARE(sourcemoves::settle(*f.store, f.post).blocked, 1);
        QCOMPARE(readFile(source), QByteArray("new one"));
        QCOMPARE(readFile(staged), QByteArray("ours"));
        QCOMPARE(f.state(0), QStringLiteral("blocked"));
        // Once that file goes, the move comes back on its own.
        QVERIFY(QFile::remove(source));
        QCOMPARE(sourcemoves::settle(*f.store, f.post).restored, 1);
        QCOMPARE(readFile(source), QByteArray("ours"));
    }

    void bothAbsentIsBlocked()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "ours"));
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, source, staged).outcome,
                 Outcome::Moved);
        QVERIFY(QFile::remove(staged));
        QCOMPARE(sourcemoves::settle(*f.store, f.post).blocked, 1);
        QCOMPARE(f.state(0), QStringLiteral("blocked"));
    }

    void anotherFileIsBlockedAndLeftAlone()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "ours"));
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, source, staged).outcome,
                 Outcome::Moved);
        // Replaced: same name, another file.
        QVERIFY(QFile::remove(staged));
        QVERIFY(writeFile(f.path(QStringLiteral("other")), "foreign"));
        QVERIFY(QFile::rename(f.path(QStringLiteral("other")), staged));
        QCOMPARE(sourcemoves::settle(*f.store, f.post).blocked, 1);
        QVERIFY(!QFileInfo::exists(source));
        QCOMPARE(readFile(staged), QByteArray("foreign"));
    }

    void aChangedContentIsStillTheSameFile()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "ours"));
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, source, staged).outcome,
                 Outcome::Moved);
        {
            QFile grown(staged);
            QVERIFY(grown.open(QIODevice::Append));
            grown.write(" and more"); // other size and time, same file
        }
        QCOMPARE(sourcemoves::settle(*f.store, f.post).restored, 1);
        QCOMPARE(readFile(source), QByteArray("ours and more"));
    }

    void aSettledMoveIsNeverReplayed()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "ours"));
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, source, staged).outcome,
                 Outcome::Moved);
        QCOMPARE(sourcemoves::settle(*f.store, f.post).restored, 1);
        // A file of the staged name later: the settled move is not done again.
        QVERIFY(QFile::rename(source, staged));
        const auto again = sourcemoves::settle(*f.store, f.post);
        QCOMPARE(again.restored + again.blocked + again.untouched, 0);
        QCOMPARE(sourcemoves::moveBack(*f.store, f.post, f.run, 0).outcome, Outcome::Restored);
        QVERIFY(QFileInfo::exists(staged));
        QVERIFY(!QFileInfo::exists(source));
    }

    void aUsedSequenceIsNeverMovedAgain()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "ours"));
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, source, staged).outcome,
                 Outcome::Moved);
        QCOMPARE(sourcemoves::moveBack(*f.store, f.post, f.run, 0).outcome, Outcome::Restored);
        // The same intent again: refused before the disk, the journal stays right.
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, source, staged).outcome,
                 Outcome::Refused);
        QCOMPARE(readFile(source), QByteArray("ours"));
        QVERIFY(!QFileInfo::exists(staged));
        QCOMPARE(f.state(0), QStringLiteral("restored"));
    }

    void aReturnIsDurableBeforeItIsRecorded()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "ours"));
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, source, staged).outcome,
                 Outcome::Moved);
        qputenv("NGPOST_TEST_FAIL_DIR_SYNC", "1");
        const auto first = sourcemoves::moveBack(*f.store, f.post, f.run, 0);
        const auto replay = sourcemoves::moveBack(*f.store, f.post, f.run, 0);
        qunsetenv("NGPOST_TEST_FAIL_DIR_SYNC");
        // Back on disk, but neither call could make it durable: none recorded it.
        QCOMPARE(first.outcome, Outcome::Unrecorded);
        QCOMPARE(first.where, sourcemoves::Where::Source);
        QCOMPARE(replay.outcome, Outcome::Unrecorded);
        QCOMPARE(readFile(source), QByteArray("ours"));
        QCOMPARE(f.state(0), QStringLiteral("moved"));
        QCOMPARE(sourcemoves::moveBack(*f.store, f.post, f.run, 0).outcome, Outcome::Restored);
        QCOMPARE(f.state(0), QStringLiteral("restored"));
    }

    void nothingIsTouchedWhileItsToolMayRun()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "ours"));
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, source, staged).outcome,
                 Outcome::Moved);
        QVERIFY2(f.toolRunning(), qPrintable(f.error));
        f.reopen();
        const auto settled = sourcemoves::settle(*f.store, f.post);
        QCOMPARE(settled.untouched, 1);
        QVERIFY(QFileInfo::exists(staged));
        QVERIFY(!QFileInfo::exists(source));
        QCOMPARE(f.state(0), QStringLiteral("moved"));
    }

    void aMoveWithoutItsSetProvesNothing()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "ours"));
        QVERIFY2(f.plan(0, source, staged, {}), qPrintable(f.error));
        QVERIFY(QFile::rename(source, staged));
        QCOMPARE(sourcemoves::settle(*f.store, f.post).blocked, 1);
        QVERIFY(QFileInfo::exists(staged)); // left where it is
        // And none is ever started without one.
        QCOMPARE(sourcemoves::moveOut(*f.store, { f.post, f.run, {} }, 1, staged, source).outcome,
                 Outcome::Refused);
    }

    void aRefusedRecordAfterTheMoveIsSettledLater()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "ours"));
        QVERIFY(f.exec(QStringLiteral("CREATE TRIGGER refuse BEFORE UPDATE OF state ON "
                                      "post_source_moves BEGIN SELECT RAISE(ABORT, 'x'); END")));
        const auto result = sourcemoves::moveOut(*f.store, f.current(), 0, source, staged);
        QCOMPARE(result.outcome, Outcome::Unrecorded);
        QCOMPARE(result.where, sourcemoves::Where::Staged);
        QVERIFY(result.planned);
        QCOMPARE(f.state(0), QStringLiteral("planned"));
        QVERIFY(f.exec(QStringLiteral("DROP TRIGGER refuse")));
        f.reopen();
        QCOMPARE(sourcemoves::settle(*f.store, f.post).restored, 1);
        QCOMPARE(readFile(source), QByteArray("ours"));
    }

    void aRefusedRecordAfterTheReturnKeepsItToSettle()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "ours"));
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, source, staged).outcome,
                 Outcome::Moved);
        QVERIFY(f.exec(QStringLiteral("CREATE TRIGGER refuse BEFORE UPDATE OF state ON "
                                      "post_source_moves BEGIN SELECT RAISE(ABORT, 'x'); END")));
        const auto result = sourcemoves::moveBack(*f.store, f.post, f.run, 0);
        QCOMPARE(result.outcome, Outcome::Unrecorded);
        QCOMPARE(result.where, sourcemoves::Where::Source);
        QCOMPARE(readFile(source), QByteArray("ours"));
        QCOMPARE(f.state(0), QStringLiteral("moved"));
        QVERIFY(f.exec(QStringLiteral("DROP TRIGGER refuse")));
        QCOMPARE(sourcemoves::settle(*f.store, f.post).restored, 1); // the disk says so
        QCOMPARE(f.state(0), QStringLiteral("restored"));
    }

    void withoutAnAtomicRenameNothingMoves()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "ours"));
        qputenv("NGPOST_TEST_NO_ATOMIC_RENAME", "1");
        const auto result = sourcemoves::moveOut(*f.store, f.current(), 0, source, staged);
        qunsetenv("NGPOST_TEST_NO_ATOMIC_RENAME");
        QCOMPARE(result.outcome, Outcome::NotMoved); // recorded: another place may be tried
        QCOMPARE(f.state(0), QStringLiteral("restored"));
        QCOMPARE(readFile(source), QByteArray("ours"));
        QVERIFY(!QFileInfo::exists(staged));
    }

    void anExistingTargetIsNeverReplaced()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(source, "ours"));
        QVERIFY(writeFile(staged, "theirs"));
        const auto result = sourcemoves::moveOut(*f.store, f.current(), 0, source, staged);
        QCOMPARE(result.outcome, Outcome::Blocked); // both there: nothing decided
        QCOMPARE(readFile(source), QByteArray("ours"));
        QCOMPARE(readFile(staged), QByteArray("theirs"));
    }

    void aFolderMovesAndComesBack()
    {
        Fixture f;
        const QString source = f.path(QStringLiteral("season"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(QDir().mkpath(source));
        QVERIFY(writeFile(source + QStringLiteral("/e01.mkv"), "one"));
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, source, staged).outcome,
                 Outcome::Moved);
        QCOMPARE(readFile(staged + QStringLiteral("/e01.mkv")), QByteArray("one"));
        QCOMPARE(sourcemoves::moveBack(*f.store, f.post, f.run, 0).outcome, Outcome::Restored);
        QCOMPARE(readFile(source + QStringLiteral("/e01.mkv")), QByteArray("one"));
    }

    void linksAreNeverFollowed()
    {
#ifdef Q_OS_WIN
        QSKIP("symbolic links need a privilege the test runners do not have");
#else
        Fixture f;
        const QString target = f.path(QStringLiteral("real.mkv"));
        const QString source = f.path(QStringLiteral("movie.mkv"));
        const QString staged = f.path(QStringLiteral("staging/x1"));
        QVERIFY(writeFile(target, "real"));
        QVERIFY(QFile::link(target, source));
        // A link as a source: refused, nothing recorded.
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 0, source, staged).outcome,
                 Outcome::Refused);
        QVERIFY(QFile::remove(source));
        QVERIFY(writeFile(source, "ours"));
        QCOMPARE(sourcemoves::moveOut(*f.store, f.current(), 1, source, staged).outcome,
                 Outcome::Moved);
        // Its staged copy replaced by a dangling link: blocked, nothing followed.
        QVERIFY(QFile::remove(staged));
        QVERIFY(QFile::link(f.path(QStringLiteral("gone")), staged));
        QCOMPARE(sourcemoves::settle(*f.store, f.post).blocked, 1);
        QVERIFY(!QFileInfo::exists(source));
        QVERIFY(QFileInfo(staged).isSymLink());
#endif
    }
};

QTEST_GUILESS_MAIN(tst_SourceMoves)
#include "tst_SourceMoves.moc"
