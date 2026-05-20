//========================================================================
//
// tst_PostHistory.cpp -- SQLite history, resume classification and NZB
// regeneration checks for the LevelUp posting history.
//
//========================================================================

#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QXmlStreamReader>

#include "history/NzbHistoryRegenerator.h"
#include "history/PostHistoryStore.h"
#include "history/ResumePlanner.h"

class TestPostHistory : public QObject
{
    Q_OBJECT

private slots:
    void sqlite_lifecycle_and_resume_states();
    void empty_resume_post_is_cancelled_by_cleanup();
    void missing_source_is_not_resumable();
    void nzb_regeneration_keeps_prior_files_after_resume();
    void nzb_regeneration_masks_password_by_default();
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
}

QTEST_MAIN(TestPostHistory)
#include "tst_PostHistory.moc"
