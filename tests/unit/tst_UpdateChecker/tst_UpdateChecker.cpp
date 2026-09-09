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
    QNetworkReply *createRequest(Operation, const QNetworkRequest &, QIODevice *) override {
        last = new FakeUpdateReply(this);
        return last;
    }
};
}

class TestUpdateChecker : public QObject
{
    Q_OBJECT

private slots:
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
    void trusted_urls() {
        QVERIFY(UpdateChecker::isTrustedDownloadUrl(QUrl("https://github.com/a")));
        QVERIFY(UpdateChecker::isTrustedDownloadUrl(QUrl("https://release-assets.githubusercontent.com/a")));
        for (const QString &url : {"http://github.com/a", "https://evilgithub.com/a",
             "https://github.com.attacker.test/a", "https://user:pass@github.com/a",
             "https://github.com:444/a", "file:///tmp/archive"})
            QVERIFY2(!UpdateChecker::isTrustedDownloadUrl(QUrl(url)), qPrintable(url));
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

QTEST_GUILESS_MAIN(TestUpdateChecker)
#include "tst_UpdateChecker.moc"
