// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//
// Ordering of release tags. A build must be offered what supersedes it and
// nothing else -- in particular an unstable build must be offered the stable
// it leads to, which a plain numeric comparison can never do because both
// read 5.5.

#include "utils/UpdateChecker.h"

#include <QtTest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace {
class FakeUpdateReply : public QNetworkReply {
public:
    explicit FakeUpdateReply(QObject *parent) : QNetworkReply(parent) {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        open(QIODevice::ReadOnly);
    }
    QByteArray body;
    bool aborted = false;
    void abort() override {
        aborted = true;
        setError(OperationCanceledError, "canceled");
        emit finished(); // deliberate synchronous cancellation callback
    }
    qint64 bytesAvailable() const override { return body.size() + QNetworkReply::bytesAvailable(); }
    qint64 readData(char *data, qint64 maximum) override {
        qint64 size = qMin(maximum, qint64(body.size()));
        memcpy(data, body.constData(), size_t(size));
        body.remove(0, size);
        return size;
    }
    void deliver(QByteArray bytes) { body = bytes; emit readyRead(); }
    void finish() { emit finished(); }
};
class FakeUpdateNetwork : public QNetworkAccessManager {
public:
    FakeUpdateReply *last = nullptr;
    QHash<QString, QByteArray> responses;
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        last = new FakeUpdateReply(this);
        if (!responses.isEmpty()) {
            auto reply = last;
            const auto bytes = responses.value(request.url().toString());
            QTimer::singleShot(0, reply, [reply, bytes] {
                reply->deliver(bytes);
                reply->finish();
            });
        }
        return last;
    }
};
}

class TestUpdateChecker : public QObject
{
    Q_OBJECT

private slots:
    void release_channels_data()
    {
        QTest::addColumn<QString>("current");
        QTest::addColumn<QString>("expected");
        QTest::newRow("stable-to-stable") << QString("v5.5.1") << QString("v5.6");
        QTest::newRow("stable-stays-stable") << QString("v5.6") << QString();
        QTest::newRow("unstable-to-stable")
            << QString("v5.6-unstable.1") << QString("v5.7-unstable.2");
        QTest::newRow("unstable-to-new-build")
            << QString("v5.7-unstable.1") << QString("v5.7-unstable.2");
        QTest::newRow("unstable-current") << QString("v5.7-unstable.2") << QString();
        QTest::newRow("never-downgrade") << QString("v6.0") << QString();
    }
    void release_channels()
    {
        QFETCH(QString, current);
        QFETCH(QString, expected);
        const QJsonArray releases{
            QJsonObject{ { "tag_name", "v100.0" }, { "draft", true } },
            QJsonObject{ { "tag_name", "v99.0/bad" } },
            QJsonObject{ { "tag_name", "v5.7-unstable.2" }, { "prerelease", true } },
            QJsonObject{ { "tag_name", "v5.6" } },
            QJsonObject{ { "tag_name", "v5.6-unstable.3" }, { "prerelease", true } }
        };
        QCOMPARE(UpdateChecker::selectRelease(QJsonDocument(releases), current)
                     .value("tag_name")
                     .toString(),
                 expected);
        const QJsonArray sameNumber{ releases.at(3), releases.at(4) };
        QCOMPARE(UpdateChecker::selectRelease(QJsonDocument(sameNumber), "v5.6-unstable.1")
                     .value("tag_name")
                     .toString(),
                 QString("v5.6"));
    }
    void release_notification_data()
    {
        QTest::addColumn<QString>("tag");
        QTest::addColumn<bool>("draft");
        QTest::addColumn<bool>("prerelease");
        QTest::addColumn<bool>("offered");
        QTest::newRow("upgrade") << QString("v99.0") << false << false << true;
        QTest::newRow("same-version") << UpdateChecker::buildTag() << false << false << false;
        QTest::newRow("downgrade") << QString("v1.0") << false << false << false;
        QTest::newRow("draft") << QString("v99.0") << true << false << false;
        QTest::newRow("prerelease-flag") << QString("v99.0") << false << true << false;
        QTest::newRow("prerelease-tag") << QString("v99.0-rc.1") << false << true << false;
        QTest::newRow("bad-tag") << QString("../99.0") << false << false << false;
    }
    void release_notification()
    {
        QFETCH(QString, tag);
        QFETCH(bool, draft);
        QFETCH(bool, prerelease);
        QFETCH(bool, offered);
        FakeUpdateNetwork network;
        UpdateChecker checker(nullptr, &network);
        QSignalSpy available(&checker, &UpdateChecker::newVersionAvailable);
        const auto name = checker.assetNameForCurrentOS(tag);
        const QString url = "https://github.com/Hydro74000/ngPost/releases/download/" + tag + "/"
            + name;
        QJsonObject release{ { "tag_name", tag },
                             { "draft", draft },
                             { "prerelease", prerelease },
                             { "assets",
                               QJsonArray{ QJsonObject{ { "name", name },
                                                        { "browser_download_url", url },
                                                        { "size", 123 } } } } };
        checker.checkLatestRelease();
        QVERIFY(network.last);
        network.last->deliver(QJsonDocument(release).toJson());
        network.last->finish();
        QCOMPARE(available.size(), offered ? 1 : 0);
        if (offered) {
            QCOMPARE(checker._assetFileName, name);
            QCOMPARE(checker._assetUrl, QUrl(url));
            QCOMPARE(checker._assetSize, 123);
        }
    }
    void missing_or_untrusted_asset_keeps_manual_notification_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("missing") << QString();
        QTest::newRow("foreign-host") << QString("https://example.org/archive");
        QTest::newRow("other-repository")
            << QString("https://github.com/other/repo/releases/download/v99.0/archive");
    }
    void missing_or_untrusted_asset_keeps_manual_notification()
    {
        QFETCH(QString, url);
        FakeUpdateNetwork network;
        UpdateChecker checker(nullptr, &network);
        QSignalSpy available(&checker, &UpdateChecker::newVersionAvailable);
        QJsonObject release{ { "tag_name", "v99.0" },
                             { "assets",
                               QJsonArray{
                                   QJsonObject{ { "name", checker.assetNameForCurrentOS("v99.0") },
                                                { "browser_download_url", url },
                                                { "size", 123 } } } } };
        checker.checkLatestRelease();
        network.last->deliver(QJsonDocument(release).toJson());
        network.last->finish();
        QCOMPARE(available.size(), 1);
        QVERIFY(checker._assetUrl.isEmpty());
        QVERIFY(!checker.canInstallAutomatically());
    }
    void appimage_still_checks_for_updates()
    {
        const auto previous = qgetenv("APPIMAGE");
        qputenv("APPIMAGE", "/tmp/ngPost.AppImage");
        FakeUpdateNetwork network;
        UpdateChecker checker(nullptr, &network);
        checker.checkLatestRelease();
        const bool requested = network.last != nullptr;
        const bool automatic = checker.canInstallAutomatically();
        if (previous.isNull())
            qunsetenv("APPIMAGE");
        else
            qputenv("APPIMAGE", previous);
        QVERIFY(requested);
        QVERIFY(!automatic);
    }
    void only_owned_portable_installations_are_replaceable()
    {
        QTemporaryDir root;
        const auto directory = root.path() + "/installation with spaces";
        QVERIFY(QDir().mkpath(directory));
        QVERIFY(!UpdateChecker::isReplaceableInstallation(directory)); // sources / distro packages
        QFile marker(directory + "/.ngpost-installation");
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.close();
        QVERIFY(UpdateChecker::isReplaceableInstallation(directory));
        QFile uninstall(directory + "/unins000.exe");
        QVERIFY(uninstall.open(QIODevice::WriteOnly));
        uninstall.close();
        QVERIFY(
            !UpdateChecker::isReplaceableInstallation(directory)); // Inno Setup owns this directory
        QVERIFY(uninstall.remove());
        for (const auto &name : { "ngPost.pro", "ngPost_core.pri", "Makefile", ".git" }) {
            QFile source(directory + "/" + name);
            QVERIFY(source.open(QIODevice::WriteOnly));
            source.close();
            QVERIFY(!UpdateChecker::isReplaceableInstallation(directory));
            QVERIFY(source.remove());
        }
    }
    void checksum_verifier_is_embedded_in_every_build() {
        QFile file(QStringLiteral(":/update/install_update.py"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray const verifier = file.readAll();
        QVERIFY(verifier.contains("hashlib.sha256()"));
        QVERIFY(!verifier.contains("manifest.sig"));
    }
    void canceled_download_cannot_corrupt_a_retry() {
        FakeUpdateNetwork network;
        UpdateChecker checker(nullptr, &network);
        checker._work.reset(new QTemporaryDir);
        QVERIFY(checker._work->isValid());
        QSignalSpy errors(&checker, &UpdateChecker::downloadFailed);
        bool completed = false;
        checker.downloadFile(QUrl("https://github.com/a"), "first", 8, [&] { completed = true; });
        auto stale = network.last;
        stale->deliver("part");
        checker.cancelDownload();
        QVERIFY(stale->aborted);
        QVERIFY(!completed);
        QCOMPARE(errors.size(), 0);
        checker._cancelled = false;
        checker.downloadFile(QUrl("https://github.com/b"), "second", 8, [&] { completed = true; });
        stale->finish(); // late signal must not clear the new reply or its file
        network.last->deliver("new data");
        network.last->finish();
        QVERIFY(completed);
        QCOMPARE(errors.size(), 0);
        QFile output(checker._work->filePath("second"));
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.readAll(), QByteArray("new data"));
    }
    void download_limit_aborts_without_completing() {
        FakeUpdateNetwork network;
        UpdateChecker checker(nullptr, &network);
        checker._work.reset(new QTemporaryDir);
        bool completed = false;
        QSignalSpy errors(&checker, &UpdateChecker::downloadFailed);
        checker.downloadFile(QUrl("https://github.com/a"), "archive", 3, [&] { completed = true; });
        network.last->deliver("too long");
        QVERIFY(network.last->aborted);
        QVERIFY(!completed);
        QCOMPARE(errors.size(), 1);
    }
    void teardown_preserves_successful_handoff() {
        QTemporaryDir work;
        const QString path = work.path();
        {
            UpdateChecker checker(nullptr, nullptr);
            checker._work.reset(new QTemporaryDir(path + "/transaction-XXXXXX"));
            checker._work->setAutoRemove(false);
            checker._handoff = true;
        }
        QDir directory(path);
        const auto transactions = directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(transactions.size(), 1);
        QVERIFY(!QFileInfo::exists(path + "/" + transactions.first() + "/cancelled"));
    }
    //! The marker is how a cancellation reaches the detached installer, which
    //! polls for it (install_update.py). Dropping it is the whole job of
    //! cancelDownload(); nothing is reported because nothing went wrong.
    void cancel_writes_the_marker_the_detached_installer_polls_for() {
        UpdateChecker checker(nullptr, nullptr);
        checker._work.reset(new QTemporaryDir);
        QVERIFY(checker._work->isValid());
        QSignalSpy errors(&checker, &UpdateChecker::downloadFailed);

        checker.cancelDownload();

        QVERIFY(QFileInfo::exists(checker._work->filePath("cancelled")));
        QCOMPARE(errors.size(), 0);
    }

    //! open() used to be called for its side effect alone, so a marker that
    //! could not be written left the user believing the update was called off
    //! while the detached installer went on to replace the installation.
    //! Removing the work folder is what makes the write fail on every
    //! platform, and as any user -- a chmod would not stop root.
    void unwritable_marker_is_reported_once_the_installer_is_detached() {
        UpdateChecker checker(nullptr, nullptr);
        checker._work.reset(new QTemporaryDir);
        QVERIFY(checker._work->isValid());
        QVERIFY(QDir().rmdir(checker._work->path()));
        checker._detached = true;
        QSignalSpy errors(&checker, &UpdateChecker::downloadFailed);

        checker.cancelDownload();

        QVERIFY(!QFileInfo::exists(checker._work->filePath("cancelled")));
        QCOMPARE(errors.size(), 1);
    }

    //! Before startDetached() there is no process to call off, so the same
    //! failed write is not worth a word: the download was aborted in-process.
    void unwritable_marker_is_silent_while_nothing_is_detached() {
        UpdateChecker checker(nullptr, nullptr);
        checker._work.reset(new QTemporaryDir);
        QVERIFY(QDir().rmdir(checker._work->path()));
        QSignalSpy errors(&checker, &UpdateChecker::downloadFailed);

        checker.cancelDownload();

        QCOMPARE(errors.size(), 0);
    }

    //! _detached says "a detached installer is running and only the marker can
    //! stop it". A previous attempt that reached startDetached() and then failed
    //! its readiness wait leaves it set; carrying that into a retry made a
    //! failed marker write warn about an installer that is not running, which
    //! is how a warning the user must trust becomes one they learn to ignore.
    void a_retry_does_not_inherit_the_previous_detached_state() {
        UpdateChecker checker(nullptr, nullptr);
        checker._work.reset(new QTemporaryDir);
        QVERIFY(checker._work->isValid());
        QVERIFY(QDir().rmdir(checker._work->path())); // the marker write will fail
        checker._detached = true;                     // as a failed handoff would leave it
        QSignalSpy errors(&checker, &UpdateChecker::downloadFailed);

        // No trusted asset, so this fails immediately through failDownload().
        checker.startDownloadAndInstall();

        QVERIFY(!checker._detached);
        QCOMPARE(errors.size(), 1);
        QString const message = errors.first().first().toString();
        QVERIFY2(!message.contains(QStringLiteral("already running")), qPrintable(message));
    }

    void trusted_urls() {
        QVERIFY(UpdateChecker::isTrustedDownloadUrl(QUrl("https://github.com/a")));
        QVERIFY(UpdateChecker::isTrustedDownloadUrl(QUrl("https://release-assets.githubusercontent.com/a")));
        for (const char *url : { "http://github.com/a",
                                 "https://evilgithub.com/a",
                                 "https://github.com.attacker.test/a",
                                 "https://user:pass@github.com/a",
                                 "https://github.com:444/a",
                                 "file:///tmp/archive" })
            QVERIFY2(!UpdateChecker::isTrustedDownloadUrl(QUrl(QString::fromLatin1(url))), url);
    }
    //! Plain releases order by their numbers, and equal numbers are not newer.
    void stable_releases_order_by_number();

    //! A pre-release suffix is recognised, a plain version is not.
    void a_suffix_marks_a_pre_release();

    //! The stable supersedes the pre-releases that led to it, never the other
    //! way round. This is the case that left an unstable build stranded.
    void stable_supersedes_the_pre_release_of_the_same_number();

    //! Two unstable builds of one version order by the ordinal the release
    //! workflow puts in the tag: date, then run number.
    void two_pre_releases_order_by_build_ordinal();

    //! A pre-release of a higher number still wins over a lower stable, and a
    //! lower pre-release never displaces a higher stable.
    void numbers_decide_before_the_suffix();

    //! A stable install gets exactly the verdict the previous implementation
    //! gave, over every plausible pair of released versions.
    void a_stable_install_keeps_the_answers_it_had();
};

void TestUpdateChecker::stable_releases_order_by_number()
{
    QVERIFY(UpdateChecker::isVersionNewer(QStringLiteral("v5.5"), QStringLiteral("5.4.2")));
    QVERIFY(UpdateChecker::isVersionNewer(QStringLiteral("v5.4.3"), QStringLiteral("v5.4.2")));
    QVERIFY(!UpdateChecker::isVersionNewer(QStringLiteral("v5.4.2"), QStringLiteral("v5.5")));

    // Same release, spelled two ways and with or without the v.
    QVERIFY(!UpdateChecker::isVersionNewer(QStringLiteral("v5.5"), QStringLiteral("5.5")));
    QVERIFY(!UpdateChecker::isVersionNewer(QStringLiteral("v5.5"), QStringLiteral("5.5.0")));
    QVERIFY(!UpdateChecker::isVersionNewer(QStringLiteral("5.5.0"), QStringLiteral("v5.5")));

    // 10 is after 9, not before it as a text comparison would have it.
    QVERIFY(UpdateChecker::isVersionNewer(QStringLiteral("v5.10"), QStringLiteral("v5.9")));
}

void TestUpdateChecker::a_suffix_marks_a_pre_release()
{
    QVERIFY(UpdateChecker::isPreRelease(
        QStringLiteral("v5.5-unstable.20260824.107.ac4bf63")));
    QVERIFY(!UpdateChecker::isPreRelease(QStringLiteral("v5.5")));
    QVERIFY(!UpdateChecker::isPreRelease(QStringLiteral("5.4.2")));
}

void TestUpdateChecker::stable_supersedes_the_pre_release_of_the_same_number()
{
    const QString unstable = QStringLiteral("v5.5-unstable.20260824.107.ac4bf63");

    QVERIFY2(UpdateChecker::isVersionNewer(QStringLiteral("v5.5"), unstable),
             "an unstable build must be offered the stable of the same number");
    QVERIFY2(!UpdateChecker::isVersionNewer(unstable, QStringLiteral("v5.5")),
             "a stable install must never be offered a pre-release");
}

void TestUpdateChecker::two_pre_releases_order_by_build_ordinal()
{
    const QString older = QStringLiteral("v5.5-unstable.20260824.107.ac4bf63");
    const QString newer = QStringLiteral("v5.5-unstable.20260824.108.deadbee");
    const QString nextDay = QStringLiteral("v5.5-unstable.20260825.3.cafe123");

    QVERIFY(UpdateChecker::isVersionNewer(newer, older));
    QVERIFY(!UpdateChecker::isVersionNewer(older, newer));
    QVERIFY(!UpdateChecker::isVersionNewer(older, older));

    // A later date wins even though its run number is smaller.
    QVERIFY(UpdateChecker::isVersionNewer(nextDay, newer));
}

void TestUpdateChecker::numbers_decide_before_the_suffix()
{
    QVERIFY(UpdateChecker::isVersionNewer(
        QStringLiteral("v5.6-unstable.20260901.1.abc"), QStringLiteral("v5.5")));
    QVERIFY(!UpdateChecker::isVersionNewer(
        QStringLiteral("v5.4-unstable.20260901.1.abc"), QStringLiteral("v5.5")));
}

void TestUpdateChecker::a_stable_install_keeps_the_answers_it_had()
{
    // Regression guard for the users who matter most: someone on a released
    // stable must get exactly the verdict the previous implementation gave.
    // Every pair below was checked against it.
    const QStringList stables{ QStringLiteral("4.16"),  QStringLiteral("5.0"),
                               QStringLiteral("5.0.1"), QStringLiteral("5.4"),
                               QStringLiteral("5.4.2"), QStringLiteral("5.4.3"),
                               QStringLiteral("5.5"),   QStringLiteral("5.5.0"),
                               QStringLiteral("5.6"),   QStringLiteral("5.9"),
                               QStringLiteral("5.10"),  QStringLiteral("6.0") };

    auto numeric = [](const QString &t) {
        QList<int> parts;
        for (const QString &p : t.split(QLatin1Char('.')))
            parts << p.toInt();
        return parts;
    };
    auto expected = [&](const QString &a, const QString &b) {
        const QList<int> l = numeric(a), c = numeric(b);
        for (int i = 0; i < qMax(l.size(), c.size()); ++i) {
            const int lv = i < l.size() ? l.at(i) : 0;
            const int cv = i < c.size() ? c.at(i) : 0;
            if (lv != cv)
                return lv > cv;
        }
        return false;
    };

    for (const QString &a : stables) {
        for (const QString &b : stables) {
            for (const QString &pa : { QString(), QStringLiteral("v") }) {
                for (const QString &pb : { QString(), QStringLiteral("v") }) {
                    QVERIFY2(UpdateChecker::isVersionNewer(pa + a, pb + b) == expected(a, b),
                             qPrintable(pa + a + " vs " + pb + b));
                }
            }
        }
    }
}

// Run the real C++ download / Python preparation / detached handoff in a
// disposable installation. Only the HTTP transport is substituted; trusted
// production URLs, payload limits, resources and installer processes are real.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--update-handoff"))) {
        FakeUpdateNetwork network;
        QFile scenario(qEnvironmentVariable("NGPOST_UPDATE_SCENARIO"));
        if (!scenario.open(QIODevice::ReadOnly))
            return 2;
        const auto object = QJsonDocument::fromJson(scenario.readAll()).object();
        for (auto it = object.begin(); it != object.end(); ++it) {
            QFile payload(it.value().toString());
            if (!payload.open(QIODevice::ReadOnly))
                return 3;
            network.responses.insert(it.key(), payload.readAll());
        }
        UpdateChecker checker(nullptr, &network);
        QObject::connect(&checker,
                         &UpdateChecker::newVersionAvailable,
                         &checker,
                         &UpdateChecker::startDownloadAndInstall);
        QObject::connect(&checker,
                         &UpdateChecker::downloadFailed,
                         &app,
                         [&app](const QString &error) {
            qWarning().noquote() << error;
            app.exit(4);
        });
        QTimer::singleShot(30000, &app, [&app] { app.exit(5); });
        checker.checkLatestRelease();
        return app.exec();
    }
    TestUpdateChecker tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_UpdateChecker.moc"
