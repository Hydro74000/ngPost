#include <QtTest>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include "par2/Par2Settings.h"
using namespace par2;

class TestPar2Settings : public QObject {
    Q_OBJECT
private slots:
    void guided_arguments_roundtrip_data()
    {
        QTest::addColumn<int>("tool");
        for (auto kind : {Tool::ParPar, Tool::MultiPar, Tool::Par2cmdline})
            QTest::newRow(qPrintable(toolName(kind))) << int(kind);
    }
    void guided_arguments_roundtrip()
    {
        QFETCH(int, tool);
        Settings settings;
        settings.tool = Tool(tool);
        settings.blocks = Blocks::Size;
        settings.blockBytes = 65536;
        settings.volumes = Volumes::Count;
        settings.volumeCount = 5;
        settings.distribution = settings.tool == Tool::Par2cmdline ? Distribution::Uniform : Distribution::Equal;
        settings.threads = 2;
        settings.memory = settings.tool == Tool::MultiPar ? 4 : 256;
        if (settings.tool != Tool::Par2cmdline) settings.gpu = true;
        const auto encoded = joinArguments(settings.arguments(12));
        const auto decoded = Settings::read(settings.tool, encoded);
        QVERIFY2(!decoded.custom, qPrintable(encoded));
        QCOMPARE(decoded.arguments(12), settings.arguments(12));
        QCOMPARE(decoded.exactBlockBytes(), 65536);
    }
    void custom_arguments_are_lossless()
    {
        for (auto pair : {qMakePair(Tool::ParPar, QString("-s1M --min-input-slices=2000 --progress stdout -r8%")),
                          qMakePair(Tool::MultiPar, QString("create /rr8 /rd5 /lc40 /ss250000")),
                          qMakePair(Tool::ParPar, QString("-s1M -r8%")),
                          qMakePair(Tool::ParPar, QString("-s2000 -s4000 -twrong")),
                          qMakePair(Tool::MultiPar, QString("c /ls2 /mwrong")),
                          qMakePair(Tool::Par2cmdline, QString("c -l -n10")),
                          qMakePair(Tool::Par2cmdline, QString("c -s65536 -b2000 -mwrong")),
                          qMakePair(Tool::Par2cmdline, QString("c -r8 -s768000 -q"))}) {
            const auto settings = Settings::read(pair.first, pair.second);
            QVERIFY(settings.custom);
            QCOMPARE(settings.originalArguments, pair.second);
            QVERIFY(!settings.estimate({1000000}, 8).valid);
        }
        const QStringList args{"--opencl-device", "device with spaces", "a\"b"};
        QCOMPARE(QProcess::splitCommand(joinArguments(args)), args);
    }
    void constraints_and_estimates()
    {
        Settings settings;
        settings.tool = Tool::ParPar;
        settings.blocks = Blocks::Size;
        settings.blockBytes = 1024;
        settings.volumes = Volumes::Size;
        settings.volumeBytes = 2048;
        settings.distribution = Distribution::Equal;
        auto estimate = settings.estimate({1024 * 100}, 10);
        QVERIFY(estimate.valid);
        QCOMPARE(estimate.sourceBlocks, 100);
        QCOMPARE(estimate.recoveryBlocks, 10);
        QCOMPARE(estimate.volumeCount, 5);
        QCOMPARE(estimate.largestRecoveryBytes, 2048);
        QVERIFY(!settings.estimate({1024LL * 32769}, 10).valid);
        settings.blockBytes = 1025;
        QVERIFY(!settings.validate().isEmpty());
        settings.blockBytes = 1024;
        settings.tool = Tool::Par2cmdline;
        QVERIFY(!settings.validate().isEmpty()); // arbitrary size limit is unsupported
        settings.tool = Tool::MultiPar;
        settings.volumes = Volumes::Count;
        settings.distribution = Distribution::Decimal;
        QVERIFY(!settings.validate().isEmpty());
        settings.volumes = Volumes::Size;
        QVERIFY(settings.validate().isEmpty());
        QVERIFY(settings.arguments(10).contains("/rd3"));
        QVERIFY(settings.arguments(10).contains("/ls2"));
    }
    void real_tools_generate_repairable_files_data()
    {
        QTest::addColumn<int>("tool");
        QTest::newRow("parpar") << int(Tool::ParPar);
        QTest::newRow("par2cmdline") << int(Tool::Par2cmdline);
#ifdef Q_OS_WIN
        QTest::newRow("multipar") << int(Tool::MultiPar);
#endif
    }
    void real_tools_generate_repairable_files()
    {
        QFETCH(int, tool);
        const auto kind = Tool(tool);
        const auto executable = findExecutable(kind);
        if (executable.isEmpty()) QSKIP("Tool is not installed.");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto input = directory.filePath("sample.bin");
        QFile file(input);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(1024 * 128, 'x'));
        file.close();
        Settings settings;
        settings.tool = kind;
        settings.blocks = Blocks::Size;
        settings.blockBytes = 4096;
        settings.volumes = Volumes::Count;
        settings.volumeCount = 2;
        settings.distribution = kind == Tool::Par2cmdline ? Distribution::Uniform : Distribution::Equal;
        const auto output = directory.filePath("sample.par2");
        auto args = settings.arguments(10);
        if (kind == Tool::ParPar) args << "-o";
        args << output << input;
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(executable, args);
        QVERIFY(process.waitForFinished(30000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QVERIFY2(process.exitCode() == 0, process.readAll().constData());
        QVERIFY(QFileInfo::exists(output));
        QCOMPARE(QDir(directory.path()).entryList({"*.vol*.par2"}, QDir::Files).size(), 2);
        // Damage one source block and verify a real repair, not only argument spelling.
        const auto verifier = findExecutable(Tool::Par2cmdline);
        if (verifier.isEmpty()) return;
        QVERIFY(file.open(QIODevice::ReadWrite));
        file.write(QByteArray(4096, 'z'));
        file.close();
        process.start(verifier, {"r", output});
        QVERIFY(process.waitForFinished(30000));
        QVERIFY2(process.exitCode() == 0, process.readAll().constData());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray(1024 * 128, 'x'));
    }
};
QTEST_GUILESS_MAIN(TestPar2Settings)
#include "tst_Par2Settings.moc"
