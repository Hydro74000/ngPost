// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_SecretMasker.cpp -- the archive password must never reach a log line.
//
// PostingJob logs the archiver command line through maskedArgs(), in CLI
// mode and not only under --debug, so a job output pasted into a bug report
// is exactly where a missed switch would leak the password.
//
//========================================================================

#include <QtTest>

#include "../../../src/utils/SecretMasker.h"

class TestSecretMasker : public QObject
{
    Q_OBJECT

private slots:
    //! The mask has a fixed width, so it says nothing of the password length.
    void mask_is_fixed_width();

    //! Each glued switch keeps its name and loses its value.
    void glued_secret_is_masked();
    void glued_secret_is_masked_data();

    //! Arguments without a secret come back untouched, bare switches included:
    //! rar and 7z then prompt for the password themselves.
    void other_arguments_are_untouched();
    void other_arguments_are_untouched_data();

    //! The line ngPost actually logs: rar and 7z command lines with generated
    //! and hand-written passwords. No password may survive in it.
    void command_line_never_contains_the_password();
    void command_line_never_contains_the_password_data();

    void empty_command_line_is_empty();
};

void TestSecretMasker::mask_is_fixed_width()
{
    QCOMPARE(SecretMasker::mask(), QStringLiteral("********"));
    QCOMPARE(SecretMasker::maskedArg(QStringLiteral("-pa")),
             SecretMasker::maskedArg(QStringLiteral("-p") + QString(200, QLatin1Char('x'))));
}

void TestSecretMasker::glued_secret_is_masked_data()
{
    QTest::addColumn<QString>("arg");
    QTest::addColumn<QString>("expected");

    QTest::newRow("rar -hp") << "-hpS3cret" << "-hp********";
    QTest::newRow("7z -p") << "-pS3cret" << "-p********";
    // "-hp" is tried before "-p": the switch name must survive intact, or the
    // log would claim headers were not encrypted.
    QTest::newRow("-hp is not read as -p") << "-hpX" << "-hp********";
    QTest::newRow("one character") << "-pX" << "-p********";
    QTest::newRow("spaces and quotes") << "-pa b \"c\"" << "-p********";
    // UTF-8 spelled out, so the source stays ASCII whatever code page MSVC reads.
    QTest::newRow("unicode") << QString::fromUtf8("-hpmot de passe \xC3\xA9\xE4\xB8\xAD")
                             << "-hp********";
    // Over-masking an unrelated "-p..." switch costs a less precise log line;
    // under-masking a password costs the password.
    QTest::newRow("any -p prefix") << "-password" << "-p********";
}

void TestSecretMasker::glued_secret_is_masked()
{
    QFETCH(QString, arg);
    QFETCH(QString, expected);
    QCOMPARE(SecretMasker::maskedArg(arg), expected);
}

void TestSecretMasker::other_arguments_are_untouched_data()
{
    QTest::addColumn<QString>("arg");

    QTest::newRow("bare -p") << "-p";
    QTest::newRow("bare -hp") << "-hp";
    QTest::newRow("7z add") << "a";
    QTest::newRow("7z level") << "-mx0";
    QTest::newRow("7z header encryption") << "-mhe=on";
    QTest::newRow("rar volume") << "-v50m";
    QTest::newRow("archive path") << "/data/tmp/LotHvPD000waxYF7W.7z";
    QTest::newRow("empty") << "";
    // Only a prefix counts: a password-looking value elsewhere is a file name.
    QTest::newRow("dash p inside") << "file-pS3cret.bin";
}

void TestSecretMasker::other_arguments_are_untouched()
{
    QFETCH(QString, arg);
    QCOMPARE(SecretMasker::maskedArg(arg), arg);
}

void TestSecretMasker::command_line_never_contains_the_password_data()
{
    QTest::addColumn<QStringList>("args");
    QTest::addColumn<QString>("password");
    QTest::addColumn<QString>("expected");

    QTest::newRow("7z") << QStringList{ "a",           "-mx0", "-mhe=on", "-pGenPass1234",
                                        "/tmp/out.7z", "/in" }
                        << "GenPass1234"
                        << "a -mx0 -mhe=on -p******** /tmp/out.7z /in";
    QTest::newRow("rar") << QStringList{ "a",     "-idp",    "-ep1", "-m0", "-hpHand written",
                                         "-v50m", "out.rar", "in" }
                         << "Hand written"
                         << "a -idp -ep1 -m0 -hp******** -v50m out.rar in";
}

void TestSecretMasker::command_line_never_contains_the_password()
{
    QFETCH(QStringList, args);
    QFETCH(QString, password);
    QFETCH(QString, expected);

    const QString line = SecretMasker::maskedArgs(args);
    QCOMPARE(line, expected);
    QVERIFY2(!line.contains(password), qPrintable(line));
}

void TestSecretMasker::empty_command_line_is_empty()
{
    QCOMPARE(SecretMasker::maskedArgs({}), QString());
}

QTEST_APPLESS_MAIN(TestSecretMasker)
#include "tst_SecretMasker.moc"
