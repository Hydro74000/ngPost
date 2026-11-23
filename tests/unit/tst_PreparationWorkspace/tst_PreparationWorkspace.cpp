// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Native file identity and the folders of a prepared set (plan_queue.md
// §6.3, lot C5.2): what survives a rename, what is a link, where each
// generation goes, and that nothing not provably ours is ever deleted.
//
//========================================================================

#include "tools/PreparationWorkspace.h"
#include "utils/NativeFileIdentity.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

namespace
{

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

//! A link to a folder: a symbolic link, or on Windows a junction, which needs
//! no privilege.
bool linkFolder(const QString &target, const QString &link)
{
#ifdef Q_OS_WIN
    return QProcess::execute(QStringLiteral("cmd.exe"),
                             { QStringLiteral("/c"),
                               QStringLiteral("mklink"),
                               QStringLiteral("/J"),
                               QDir::toNativeSeparators(link),
                               QDir::toNativeSeparators(target) })
        == 0;
#else
    return QFile::link(target, link);
#endif
}

} // namespace

class tst_PreparationWorkspace : public QObject
{
    Q_OBJECT

private slots:
    void identityFollowsAFileThroughARename()
    {
        QTemporaryDir dir;
        const QString a = dir.filePath(QStringLiteral("a.bin"));
        const QString b = dir.filePath(QStringLiteral("b.bin"));
        QVERIFY(writeFile(a, "first"));
        QVERIFY(writeFile(b, "other"));
        const auto before = NativeFileIdentity::of(a);
        QVERIFY(before);
        QVERIFY(!before->directory && !before->link);
        QCOMPARE(before->size, qint64(5));
        QVERIFY(QFile::rename(a, dir.filePath(QStringLiteral("renamed.bin"))));
        const auto after = NativeFileIdentity::of(dir.filePath(QStringLiteral("renamed.bin")));
        QVERIFY(after && after->unchanged(*before));
        QVERIFY(!NativeFileIdentity::of(a)); // gone from its old name
        QVERIFY(!NativeFileIdentity::of(b)->sameFile(*before));
        const auto parsed = NativeFileIdentity::fromJson(before->toJson());
        QVERIFY(parsed && parsed->unchanged(*before));
        QVERIFY(!NativeFileIdentity::fromJson(QStringLiteral("{\"device\":\"x\"}")));
        QVERIFY(NativeFileIdentity::of(dir.path())->directory);
    }

    void aLinkIsSeenAsALink()
    {
        QTemporaryDir dir;
        QVERIFY(QDir().mkdir(dir.filePath(QStringLiteral("target"))));
        QVERIFY(linkFolder(dir.filePath(QStringLiteral("target")),
                           dir.filePath(QStringLiteral("link"))));
        const auto link = NativeFileIdentity::of(dir.filePath(QStringLiteral("link")));
        QVERIFY(link && link->link);
        QVERIFY(!link->sameFile(*NativeFileIdentity::of(dir.filePath(QStringLiteral("target")))));
    }

    void eachGenerationHasItsOwnFolders()
    {
        QTemporaryDir dir;
        const auto g1 = PreparationWorkspace::plan(dir.path(), 7, 1);
        const auto g2 = PreparationWorkspace::plan(dir.path(), 7, 2);
        QVERIFY(g1 && g2);
        QVERIFY(g1->setDir != g2->setDir);
        QVERIFY(g1->archiveDir != g1->stagingDir);
        QVERIFY(g1->containsStrictly(g1->setDir));
        QVERIFY(g1->containsStrictly(g1->archiveDir));
        QVERIFY(!PreparationWorkspace::plan(dir.filePath(QStringLiteral("absent")), 7, 1));
        QVERIFY(!PreparationWorkspace::plan(QStringLiteral("relative/root"), 7, 1));
        QVERIFY(!PreparationWorkspace::plan(dir.path(), 7, 0));
    }

    void theRootItselfIsNeverASet()
    {
        QTemporaryDir dir;
        const auto w = PreparationWorkspace::plan(dir.path(), 7, 1);
        QVERIFY(w);
        QVERIFY(!w->containsStrictly(dir.path()));
        QVERIFY(!w->containsStrictly(dir.path() + QStringLiteral("/")));
        QVERIFY(!w->containsStrictly(dir.path() + QStringLiteral("/sub/../..")));
        QVERIFY(!w->containsStrictly(dir.path() + QStringLiteral("-sibling/x")));
        QVERIFY(w->containsStrictly(dir.path() + QStringLiteral("/x")));
    }

    void aPathThatClimbsIsNeverInside()
    {
        QTemporaryDir dir;
        QTemporaryDir outside;
        const auto w = PreparationWorkspace::plan(dir.path(), 7, 1);
        // root/run leads elsewhere: root/run/../tmp is not root/tmp.
        QVERIFY(linkFolder(outside.path(), dir.filePath(QStringLiteral("run"))));
        QVERIFY(QDir().mkdir(dir.filePath(QStringLiteral("tmp"))));
        QVERIFY(!w->containsStrictly(dir.filePath(QStringLiteral("run/../tmp"))));
        QVERIFY(!w->containsStrictly(dir.filePath(QStringLiteral("tmp/../tmp"))));
        QVERIFY(!w->containsStrictly(dir.filePath(QStringLiteral("run/x"))));
        QVERIFY(w->containsStrictly(dir.filePath(QStringLiteral("tmp/x"))));
    }

    void aLinkInsideTheArchivesIsRemovedNotFollowed()
    {
        QTemporaryDir dir;
        QTemporaryDir outside;
        const auto w = PreparationWorkspace::plan(dir.path(), 7, 1);
        NativeFileIdentity marker;
        QVERIFY(w->create(QStringLiteral("set-1"), &marker));
        QVERIFY(writeFile(outside.filePath(QStringLiteral("precious")), "keep me"));
        QVERIFY(QDir().mkdir(w->archiveDir + QStringLiteral("/deep")));
        QVERIFY(linkFolder(outside.path(), w->archiveDir + QStringLiteral("/deep/elsewhere")));
        QVERIFY(writeFile(w->archiveDir + QStringLiteral("/a.rar"), "rar"));
        QCOMPARE(w->verify(QStringLiteral("set-1"), marker.toJson()),
                 PreparationWorkspace::Status::Ours);
        QString error;
        QVERIFY2(w->removeArchives(QStringLiteral("set-1"), marker.toJson(), false, &error),
                 qPrintable(error));
        QVERIFY(!QFile::exists(w->archiveDir));
        QVERIFY(QFile::exists(outside.filePath(QStringLiteral("precious"))));
        QVERIFY(!QFile::exists(w->setDir));
    }

    void aSetFolderAlreadyThereIsACollision()
    {
        QTemporaryDir dir;
        const auto w = PreparationWorkspace::plan(dir.path(), 7, 1);
        QVERIFY(QDir().mkdir(w->setDir));
        QVERIFY(writeFile(w->setDir + QStringLiteral("/theirs"), "x"));
        QString error;
        QVERIFY(!w->create(QStringLiteral("set-1"), nullptr, &error));
        QVERIFY(error.startsWith(QStringLiteral("workspace_collision")));
        QVERIFY(QFile::exists(w->setDir + QStringLiteral("/theirs"))); // untouched
        QVERIFY(!QFile::exists(w->markerPath));
    }

    void verifyKnowsOursMissingAndAmbiguous()
    {
        QTemporaryDir dir;
        const auto w = PreparationWorkspace::plan(dir.path(), 7, 1);
        NativeFileIdentity marker;
        QString error;
        QVERIFY2(w->create(QStringLiteral("set-1"), &marker, &error), qPrintable(error));
        const QString recorded = marker.toJson();
        QCOMPARE(w->verify(QStringLiteral("set-1"), recorded), PreparationWorkspace::Status::Ours);
        QCOMPARE(w->verify(QStringLiteral("set-2"), recorded),
                 PreparationWorkspace::Status::Ambiguous); // names another set

        // The marker replaced by another file of the same content.
        QVERIFY(QFile::copy(w->markerPath, dir.filePath(QStringLiteral("copy"))));
        QVERIFY(QFile::remove(w->markerPath));
        QVERIFY(QFile::rename(dir.filePath(QStringLiteral("copy")), w->markerPath));
        QString why;
        QCOMPARE(w->verify(QStringLiteral("set-1"), recorded, &why),
                 PreparationWorkspace::Status::Ambiguous);
        QVERIFY(why.startsWith(QStringLiteral("workspace_marker_replaced")));

        // The whole folder gone, as a temporary folder emptied at reboot.
        QVERIFY(QDir(w->setDir).removeRecursively());
        QCOMPARE(w->verify(QStringLiteral("set-1"), recorded),
                 PreparationWorkspace::Status::Missing);
    }

    void aFolderLeadingOutsideIsNotOurs()
    {
        QTemporaryDir dir;
        QTemporaryDir outside;
        const auto w = PreparationWorkspace::plan(dir.path(), 7, 1);
        NativeFileIdentity marker;
        QVERIFY(w->create(QStringLiteral("set-1"), &marker));
        // Its archives folder swapped for a link to a folder elsewhere.
        QVERIFY(QDir(w->archiveDir).removeRecursively());
        QVERIFY(writeFile(outside.filePath(QStringLiteral("precious")), "keep me"));
        QVERIFY(linkFolder(outside.path(), w->archiveDir));
        QString why;
        QCOMPARE(w->verify(QStringLiteral("set-1"), marker.toJson(), &why),
                 PreparationWorkspace::Status::Ambiguous);
        QString error;
        QVERIFY(!w->removeArchives(QStringLiteral("set-1"), marker.toJson(), false, &error));
        QVERIFY(QFile::exists(outside.filePath(QStringLiteral("precious"))));
        QVERIFY(!w->containsStrictly(w->archiveDir + QStringLiteral("/precious")));
    }

    void removalTouchesOnlyWhatIsProvenOurs()
    {
        QTemporaryDir dir;
        const auto w = PreparationWorkspace::plan(dir.path(), 7, 1);
        NativeFileIdentity marker;
        QVERIFY(w->create(QStringLiteral("set-1"), &marker));
        const QString recorded = marker.toJson();
        QVERIFY(writeFile(w->archiveDir + QStringLiteral("/a.part1.rar"), "rar"));
        QVERIFY(writeFile(w->stagingDir + QStringLiteral("/source.mkv"), "source"));
        QString error;
        QVERIFY(!w->removeArchives(QStringLiteral("set-1"), recorded, true, &error));
        QVERIFY(error.startsWith(QStringLiteral("retained_by_keep_rar")));
        QVERIFY(QFile::exists(w->archiveDir + QStringLiteral("/a.part1.rar")));
        QVERIFY(!w->removeArchives(QStringLiteral("other"), recorded, false, &error));
        QVERIFY(QFile::exists(w->archiveDir + QStringLiteral("/a.part1.rar")));

        // Ours: the archives go; a source still aside keeps its folder, and the set.
        QVERIFY2(w->removeArchives(QStringLiteral("set-1"), recorded, false, &error),
                 qPrintable(error));
        QVERIFY(!QFile::exists(w->archiveDir));
        QVERIFY(QFile::exists(w->stagingDir + QStringLiteral("/source.mkv")));
        QVERIFY(QFile::exists(w->markerPath));
        // Once the source is back in place, the rest goes too.
        QVERIFY(QFile::remove(w->stagingDir + QStringLiteral("/source.mkv")));
        QVERIFY2(w->removeArchives(QStringLiteral("set-1"), recorded, false, &error),
                 qPrintable(error));
        QVERIFY(!QFile::exists(w->setDir));
        QVERIFY(QDir(dir.path()).exists()); // never the root
        QVERIFY(w->removeArchives(QStringLiteral("set-1"), recorded, false, &error)); // gone: fine
    }
};

QTEST_GUILESS_MAIN(tst_PreparationWorkspace)
#include "tst_PreparationWorkspace.moc"
