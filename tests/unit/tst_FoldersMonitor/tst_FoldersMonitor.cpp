// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_FoldersMonitor.cpp — the reservations FoldersMonitorForNewFiles takes
// for paths ngPost creates itself (issue #193).
//
// Monitoring a folder with filename obfuscation posted the same file forever:
// PostingJob renamed each input in place, inside the very folder the watcher
// was on, so the random name showed up as a new file to post, and restoring
// the real name produced yet another event. ignoreNextAppearance() is what
// breaks that loop, so the properties tested here are the ones that decide
// whether the loop comes back.
//
// The watcher itself is not driven: onDirectoryChanged() is called directly.
// QFileSystemWatcher coalesces and reorders events on its own schedule, and a
// test that waits on it measures the kernel rather than this class.
//
//========================================================================

#include <QtTest>

#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "FoldersMonitorForNewFiles.h"

class TestFoldersMonitor : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    //! A file dropped by somebody else is what the monitor exists for.
    void a_new_file_is_reported();

    //! The obfuscation rename: ngPost announces the path before creating it,
    //! and the monitor must not hand it back as something to post.
    void a_reserved_path_is_not_reported();

    //! The heart of issue #193. The reservation covers the appearance ngPost
    //! caused, and nothing more: dropping the same name again afterwards is a
    //! genuine new file. A reservation that outlived its appearance would
    //! swallow it and the user would silently lose a post.
    void a_reservation_is_spent_once();

    //! The watcher coalesces events, so a file moved out and put back before
    //! any scan ran never looks like it left. The reservation must still be
    //! released -- otherwise it sits there and eats the next real drop.
    void a_reservation_survived_by_its_file_is_released();

    //! A rename that failed leaves a reservation for a path that will never
    //! appear. Cancelling it must restore the normal behaviour.
    void a_cancelled_reservation_reports_again();

    //! Reservations are keyed by resolved path, not by the spelling used.
    void a_reservation_matches_an_unnormalized_path();

private:
    QString _write(const QString &name, const QByteArray &content = "payload");
    //! One sweep of the folder, as the watcher would trigger it.
    void _sweep();

    QTemporaryDir                        *_dir = nullptr;
    FoldersMonitorForNewFiles            *_mon = nullptr;
    QSignalSpy                           *_spy = nullptr;
};

void TestFoldersMonitor::init()
{
    _dir = new QTemporaryDir;
    QVERIFY(_dir->isValid());
    _mon = new FoldersMonitorForNewFiles(_dir->path());
    _spy = new QSignalSpy(_mon, &FoldersMonitorForNewFiles::newFileToProcess);

    // The constructor records an initial baseline, so only what arrives from
    // now on is new. Sweep once more to be sure we start from a settled state.
    _sweep();
    _spy->clear();
}

void TestFoldersMonitor::cleanup()
{
    delete _spy; _spy = nullptr;
    delete _mon; _mon = nullptr;
    delete _dir; _dir = nullptr;
}

QString TestFoldersMonitor::_write(const QString &name, const QByteArray &content)
{
    const QString path = QDir(_dir->path()).absoluteFilePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(content);
    f.close();
    return path;
}

void TestFoldersMonitor::_sweep()
{
    _mon->onDirectoryChanged(_dir->path());
}

//! Names emitted so far, so an assertion can name the file it expected.
static QStringList reported(QSignalSpy &spy)
{
    QStringList out;
    for (const QList<QVariant> &call : spy)
        out << call.at(0).value<QFileInfo>().fileName();
    return out;
}

void TestFoldersMonitor::a_new_file_is_reported()
{
    QVERIFY(!_write("dropped.rar").isEmpty());
    _sweep();
    QCOMPARE(reported(*_spy), QStringList{ QStringLiteral("dropped.rar") });
}

void TestFoldersMonitor::a_reserved_path_is_not_reported()
{
    const QString obfuscated = QDir(_dir->path()).absoluteFilePath("a3f9c1d0e2b4");
    _mon->ignoreNextAppearance(obfuscated);

    // PostingJob renames the input to its obfuscated name, in the watched folder.
    QVERIFY(!_write("a3f9c1d0e2b4").isEmpty());
    _sweep();

    QVERIFY2(_spy->isEmpty(),
             qPrintable(QStringLiteral("reported: ") + reported(*_spy).join(',')));
}

void TestFoldersMonitor::a_reservation_is_spent_once()
{
    const QString name = QStringLiteral("a3f9c1d0e2b4");
    const QString path = QDir(_dir->path()).absoluteFilePath(name);

    _mon->ignoreNextAppearance(path);
    QVERIFY(!_write(name).isEmpty());
    _sweep();
    QVERIFY(_spy->isEmpty());

    // The file leaves (restored to its real name), and a scan sees it go.
    QVERIFY(QFile::remove(path));
    _sweep();
    QVERIFY(_spy->isEmpty());

    // Somebody now drops a file that happens to carry that same name. The
    // reservation was spent on the rename; this one is a real post.
    QVERIFY(!_write(name).isEmpty());
    _sweep();
    QCOMPARE(reported(*_spy), QStringList{ name });
}

void TestFoldersMonitor::a_reservation_survived_by_its_file_is_released()
{
    const QString name = QStringLiteral("payload.part01.rar");
    const QString path = QDir(_dir->path()).absoluteFilePath(name);

    // The file is already there and known: this is the restore half of the
    // obfuscation dance, and the watcher never got to notice it had left.
    QVERIFY(!_write(name).isEmpty());
    _sweep();
    _spy->clear();

    _mon->ignoreNextAppearance(path);
    _sweep(); // the scan sees the path, but it is not new -- reservation unused
    QVERIFY(_spy->isEmpty());

    // The reservation must not still be standing. Take the file away and drop
    // a genuine new one under the same name.
    QVERIFY(QFile::remove(path));
    _sweep();
    QVERIFY(!_write(name).isEmpty());
    _sweep();

    QCOMPARE(reported(*_spy), QStringList{ name });
}

void TestFoldersMonitor::a_cancelled_reservation_reports_again()
{
    const QString name = QStringLiteral("b7e2f0a1");
    const QString path = QDir(_dir->path()).absoluteFilePath(name);

    _mon->ignoreNextAppearance(path);
    _mon->stopIgnoringMonitorPath(path); // the rename failed: it will never come

    QVERIFY(!_write(name).isEmpty());
    _sweep();

    QCOMPARE(reported(*_spy), QStringList{ name });
}

void TestFoldersMonitor::a_reservation_matches_an_unnormalized_path()
{
    const QString name = QStringLiteral("c4d5e6f7");
    // The posting side builds paths by concatenation and they are not always
    // clean. The reservation is on the file, however it was spelled.
    const QString messy = _dir->path() + QStringLiteral("/./") + name;
    _mon->ignoreNextAppearance(messy);

    QVERIFY(!_write(name).isEmpty());
    _sweep();

    QVERIFY2(_spy->isEmpty(),
             qPrintable(QStringLiteral("reported: ") + reported(*_spy).join(',')));
}

QTEST_MAIN(TestFoldersMonitor)
#include "tst_FoldersMonitor.moc"
