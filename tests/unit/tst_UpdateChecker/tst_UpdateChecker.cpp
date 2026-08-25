// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//
// Ordering of release tags. A build must be offered what supersedes it and
// nothing else -- in particular an unstable build must be offered the stable
// it leads to, which a plain numeric comparison can never do because both
// read 5.5.

#include "utils/UpdateChecker.h"

#include <QtTest>

class TestUpdateChecker : public QObject
{
    Q_OBJECT

private slots:
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

QTEST_APPLESS_MAIN(TestUpdateChecker)
#include "tst_UpdateChecker.moc"
