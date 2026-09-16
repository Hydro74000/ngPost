#include <QtTest>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <limits>
#include "par2/Par2Settings.h"
#include "tools/ExternalToolResolver.h"
#include "utils/LogTimestamp.h"
#include <QFile>
#include <QDir>
using namespace par2;

class TestPar2Settings : public QObject {
    Q_OBJECT
private slots:
    void tool_resolution_survives_bundle_moves()
    {
        using namespace externaltool;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto install = [&](const QString &folder, const QString &name) {
            const QString dir = directory.filePath(folder);
            QDir().mkpath(dir);
            QString path = QDir(dir).filePath(name);
#ifdef Q_OS_WIN
            path += QStringLiteral(".exe");
#endif
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly))
                return QString();
            file.write("fixture");
            file.close();
            file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            return path;
        };
        const QString first = install("mount-a", "parpar");
        const QString second = install("mount-b", "parpar");
        const QString system = install("system", "parpar");
        const QString par2 = install("mount-b", "par2");
        QVERIFY(!first.isEmpty() && !second.isEmpty() && !system.isEmpty() && !par2.isEmpty());
        const QStringList paths{ QFileInfo(system).absolutePath() };
        QCOMPARE(
            resolve("parpar", PathMode::Automatic, {}, QFileInfo(first).absolutePath(), paths).path,
            first);
        QCOMPARE(
            resolve("parpar", PathMode::Automatic, first, QFileInfo(second).absolutePath(), paths)
                .path,
            second);
        QVERIFY(QFile::remove(second));
        const auto fallback = resolve("parpar",
                                      PathMode::Automatic,
                                      {},
                                      QFileInfo(second).absolutePath(),
                                      paths);
        QCOMPARE(fallback.path, system);
        QCOMPARE(fallback.origin, Origin::System);
        // With no ParPar anywhere, an available par2cmdline must not receive its arguments.
        QVERIFY(!resolve("parpar", PathMode::Automatic, {}, QFileInfo(second).absolutePath(), {})
                     .available());
        QCOMPARE(
            resolve("auto", PathMode::Automatic, {}, QFileInfo(second).absolutePath(), paths).path,
            par2);
        const auto missing = resolve("parpar",
                                     PathMode::Custom,
                                     second,
                                     QFileInfo(first).absolutePath(),
                                     paths);
        QVERIFY(!missing.available());
        QCOMPARE(missing.path, second);
    }
    void archiver_names_select_the_engine()
    {
        using namespace externaltool;
        QCOMPARE(archiverForFile("/usr/bin/7z"), QString("7zip"));
        QCOMPARE(archiverForFile("C:/Program Files/7-Zip/7z.exe"), QString("7zip"));
        QCOMPARE(archiverForFile("/opt/p7zip/bin/7zr"), QString("7zip"));
        QCOMPARE(archiverForFile("/usr/bin/rar"), QString("rar"));
        QCOMPARE(archiverForFile("C:/Program Files/WinRAR/Rar.exe"), QString("rar"));
        // Only the file name counts: a folder says nothing about what it holds.
        QVERIFY(archiverForFile("/opt/7zip/bin/archiver").isEmpty());
        QVERIFY(archiverForFile("/usr/bin/unrar").isEmpty());
        QVERIFY(archiverForFile({}).isEmpty());
    }
    void migration_recognizes_only_known_bundle_paths()
    {
        using namespace externaltool;
        const QString appDir = QDir::temp().filePath("ngpost-fixture/usr/bin");
        QCOMPARE(bundledTool(appDir + "/parpar", appDir), QString("parpar"));
        QVERIFY(bundledTool("parpar", appDir).isEmpty());
        QVERIFY(bundledTool("/opt/custom/parpar", appDir).isEmpty());
#ifdef Q_OS_LINUX
        QCOMPARE(bundledTool("/tmp/.mount_ngpostOLD/usr/bin/parpar", appDir), QString("parpar"));
        QCOMPARE(bundledTool("/tmp/.mount_ngPostOLD/usr/bin/rar", appDir), QString("rar"));
        QVERIFY(bundledTool("/tmp/.mount_otherOLD/usr/bin/parpar", appDir).isEmpty());
        QVERIFY(bundledTool("/tmp/.mount_ngpostOLD/usr/bin/unknown", appDir).isEmpty());
        QVERIFY(bundledTool("/tmp/.mount_ngpostOLD/custom/parpar", appDir).isEmpty());
#endif
        QVERIFY(bundledTool(appDir + "/custom-parpar", appDir).isEmpty());
    }
    void timestamps_preserve_process_fragments_and_existing_prefixes()
    {
        LogTimestamp log;
        const QRegularExpression stamp(QStringLiteral("^\\[\\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\] "));
        QString first = log.format("first half", true);
        QVERIFY(stamp.match(first).hasMatch());
        QCOMPARE(log.format(" and second half\r", false), QString(" and second half\n"));
        const QString next = log.format("\nnext\nlast", false);
        QVERIFY(!next.startsWith('\n'));
        const auto lines = next.split('\n');
        QCOMPARE(lines.size(), 2);
        for (const auto &line : lines)
            QVERIFY(stamp.match(line).hasMatch());
        QCOMPARE(log.format("[12:34:56.789] already dated", true),
                 QString("[12:34:56.789] already dated"));
        QCOMPARE(log.format("\n\n", true), QString("\n\n"));
        QVERIFY(stamp.match(log.format("[Poster #1] debug", false)).hasMatch());
    }
    void timestamps_let_a_terminal_rewrite_progress_lines()
    {
        LogTimestamp log(LogTimestamp::CarriageReturn::RewritesLine);
        const QRegularExpression stamp(QStringLiteral("^\\[\\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\] "));
        const QString first = log.format("Processing: 1%\r", false);
        QVERIFY(stamp.match(first).hasMatch());
        QVERIFY(first.endsWith("] Processing: 1%"));
        const QString next = log.format("Processing: 2%\rProcessing: 3%\r", false);
        QVERIFY(!next.contains('\n'));
        const auto updates = next.split('\r');
        QCOMPARE(updates.size(), 3);
        QVERIFY(updates.first().isEmpty());
        QVERIFY(stamp.match(updates.at(1)).hasMatch());
        QVERIFY(stamp.match(updates.at(2)).hasMatch());
        // The \n of a CRLF split between two chunks ends the line instead.
        const QString done = log.format("\ndone\r\n", false);
        QVERIFY(!done.contains('\r'));
        QVERIFY(done.startsWith('\n'));
        QVERIFY(done.endsWith("] done\n"));
        // A new entry starts its own line: a pending CR is dropped.
        QVERIFY(log.format("100%\r", false).endsWith("100%"));
        QVERIFY(stamp.match(log.format("entry", true)).hasMatch());
    }
    void recovery_rounding_matches_the_tool()
    {
        Settings settings;
        settings.blocks = Blocks::Size;
        settings.blockBytes = 4096;
        settings.tool = Tool::ParPar;
        QCOMPARE(settings.estimate({35 * 4096}, 10).recoveryBlocks, 4);
        settings.tool = Tool::Par2cmdline;
        QCOMPARE(settings.estimate({35 * 4096}, 10).recoveryBlocks, 4);
        settings.tool = Tool::MultiPar;
        QCOMPARE(settings.estimate({35 * 4096}, 10).recoveryBlocks, 3);
        for (auto kind : {Tool::ParPar, Tool::Par2cmdline, Tool::MultiPar}) {
            settings.tool = kind;
            QCOMPARE(settings.estimate({4096}, 10).recoveryBlocks, 1);
            QCOMPARE(settings.estimate({4096}, 0).recoveryBlocks, 0);
        }
    }
    void gpu_device_ids_keep_native_indices()
    {
        const auto devices = openClDevices(R"({"type":"opencl_list","platforms":[
            {"devices":[{"type":"CPU","name":"CPU","available":true,"supported":true},
                        {"type":"GPU","name":"Card A","available":true,"supported":true},
                        {"type":"GPU","name":"Offline","available":false,"supported":true}]},
            {"devices":[{"type":"GPU","name":"Card B","available":true,"supported":true}]}]})");
        QCOMPARE(devices.size(), 2);
        QCOMPARE(devices[0].id, QString("0:1"));
        QCOMPARE(devices[0].name, QString("Card A"));
        QCOMPARE(devices[1].id, QString("1:0"));
        QVERIFY(openClDevices("No OpenCL platforms found").isEmpty());
        // The dialog has to tell "no driver at all" (ParPar aborts the post)
        // from "devices, but none of them a GPU" (a device ID still works).
        QCOMPARE(openClDeviceCount(R"({"type":"opencl_list","platforms":[
            {"devices":[{"type":"CPU","name":"CPU","available":true,"supported":true}]}]})"), 1);
        QCOMPARE(openClDeviceCount(R"({"type":"opencl_list","platforms":[]})"), 0);
        QCOMPARE(openClDeviceCount("No OpenCL platforms found"), -1);
        QCOMPARE(openClDeviceCount(R"({"platforms":[{"devices":[{"type":"GPU","available":false,"supported":true}]}]})"), 0);
        QCOMPARE(openClDeviceCount(R"({"platforms":[{}]})"), -1);
    }
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
        QCOMPARE(decoded.exactBlockBytes(), settings.tool == Tool::MultiPar ? 0 : 65536);
    }
    void custom_arguments_are_lossless()
    {
        for (auto pair : {qMakePair(Tool::ParPar, QString("-s1M --min-input-slices=2000 --progress stdout -r8%")),
                          qMakePair(Tool::MultiPar, QString("create /rr8 /rd5 /lc40 /ss250000")),
                          qMakePair(Tool::ParPar, QString("-s1M -r8%")),
                          qMakePair(Tool::ParPar, QString("-s2000 -s4000 -twrong")),
                          qMakePair(Tool::ParPar, QString("-s2000 --opencl-process=50%")),
                          qMakePair(Tool::ParPar, QString("-s2000 --opencl-process")),
                          qMakePair(Tool::MultiPar, QString("c /ls2 /mwrong")),
                          qMakePair(Tool::MultiPar, QString("c /rr8 /lr2000")),
                          qMakePair(Tool::MultiPar, QString("c /ss1048576 /rr10 /lr250")),
                          qMakePair(Tool::MultiPar, QString("c /rr8 /sn3000 /lr2000")),
                          qMakePair(Tool::MultiPar, QString("c /ss9223372036854775804 /lr2")),
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
        // par2cmdline 1.x: "the maximum allowed recovery file count is 31".
        settings.volumes = Volumes::Count;
        settings.distribution = Distribution::Uniform;
        settings.volumeCount = 31;
        QVERIFY(settings.validate().isEmpty());
        QVERIFY(settings.arguments(10).contains("-n31"));
        settings.volumeCount = 32;
        QVERIFY(!settings.validate().isEmpty());
        settings.volumeCount = 5;
        settings.tool = Tool::MultiPar;
        settings.volumes = Volumes::Count;
        settings.distribution = Distribution::Decimal;
        QVERIFY(!settings.validate().isEmpty());
        settings.volumes = Volumes::Size;
        QVERIFY(settings.validate().isEmpty());
        QVERIFY(settings.arguments(10).contains("/rd3"));
        // "/ls2" turns "/lr" into a byte limit; it is a mode flag, and does not
        // split the sources the way a real "/ls<size>" would.
        QVERIFY(settings.arguments(10).contains("/ls2"));
        QVERIFY(settings.arguments(10).contains("/lr2048"));
        const auto multiPar = Settings::read(Tool::MultiPar, joinArguments(settings.arguments(10)));
        QVERIFY(!multiPar.custom);
        QCOMPARE(multiPar.volumeBytes, 2048);
        QCOMPARE(multiPar.arguments(10), settings.arguments(10));
        // The byte limit stays available whatever the block policy is.
        for (auto mode : {Blocks::Automatic, Blocks::Count}) {
            auto anyBlocks = settings;
            anyBlocks.blocks = mode;
            anyBlocks.blockCount = 3000;
            QVERIFY(anyBlocks.validate().isEmpty());
            QVERIFY(anyBlocks.arguments(10).contains("/lr2048"));
            const auto back = Settings::read(Tool::MultiPar, joinArguments(anyBlocks.arguments(10)));
            QVERIFY(!back.custom);
            QCOMPARE(back.volumeBytes, 2048);
        }
        // /ss can be adjusted by MultiPar: a block count cannot guarantee a
        // byte target beyond the native /ls2 limit.
        auto largeVolume = settings;
        largeVolume.blockBytes = 1048576;
        largeVolume.volumeBytes = 5LL * 1024 * 1048576;
        QVERIFY(!largeVolume.validate().isEmpty());
        largeVolume.blocks = Blocks::Automatic; // no block size: no block count to derive
        QVERIFY(!largeVolume.validate().isEmpty());
        largeVolume.blocks = Blocks::Size;
        largeVolume.volumeBytes = largeVolume.blockBytes / 2; // below one block
        QVERIFY(largeVolume.validate().isEmpty()); // legal, even if no file fits
        settings.volumeBytes = 2048;
        for (auto mode : {Blocks::Automatic, Blocks::Count}) {
            settings.blocks = mode;
            settings.volumes = Volumes::Automatic;
            QVERIFY(!settings.estimate({std::numeric_limits<qint64>::max()}, 10).valid);
        }
    }
    void multipar_requested_blocks_are_not_a_check_hint()
    {
        auto settings = Settings::read(Tool::MultiPar, "c /ss1048576 /rr10 /ls2 /lr262144000");
        QVERIFY(!settings.custom);
        QCOMPARE(settings.exactBlockBytes(), 0);
        const auto small = settings.estimate({131072}, 10);
        QVERIFY(small.valid);
        QCOMPARE(small.blockBytes, 131072);
        QCOMPARE(small.sourceBlocks, 1);
        QCOMPARE(small.recoveryBytes, 131072);
        // Below one slice, the native byte cap still allocates one slice.
        settings.volumeBytes = 2048;
        settings.distribution = Distribution::Equal;
        const auto capped = settings.estimate({131072}, 10);
        QVERIFY(capped.valid);
        QCOMPARE(capped.largestRecoveryBytes, 131072);
        // MultiPar may grow the requested slice to stay under 32768 blocks.
        // Do not invent the adjusted result when that policy is not estimated.
        QVERIFY(!settings.estimate({40000LL * 1048576}, 10).valid);
    }
    void real_tools_generate_repairable_files_data()
    {
        QTest::addColumn<int>("tool");
        QTest::addColumn<bool>("gpu");
        QTest::newRow("parpar") << int(Tool::ParPar) << false;
        QTest::newRow("par2cmdline") << int(Tool::Par2cmdline) << false;
#ifdef Q_OS_WIN
        QTest::newRow("multipar") << int(Tool::MultiPar) << false;
#endif
        if (!qEnvironmentVariableIsEmpty("NGPOST_TEST_OPENCL_DEVICE"))
            QTest::newRow("parpar-opencl") << int(Tool::ParPar) << true;
    }
    void real_tools_generate_repairable_files()
    {
        QFETCH(int, tool);
        QFETCH(bool, gpu);
        const auto kind = Tool(tool);
        const auto executable = findExecutable(kind);
        if (executable.isEmpty()) QSKIP("Tool is not installed.");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto input = directory.filePath(QString::fromUtf8("sample é.bin"));
        const QByteArray original(gpu ? 64 * 1024 * 1024 : 128 * 1024, 'x');
        QFile file(input);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(original), original.size());
        file.close();
        Settings settings;
        settings.tool = kind;
        settings.blocks = Blocks::Size;
        settings.blockBytes = gpu ? 1048576 : 4096;
        settings.gpu = gpu;
        if (gpu) settings.device = qEnvironmentVariable("NGPOST_TEST_OPENCL_DEVICE");
        settings.volumes = Volumes::Count;
        settings.volumeCount = 2;
        settings.distribution = kind == Tool::Par2cmdline ? Distribution::Uniform : Distribution::Equal;
        const auto output = directory.filePath(QString::fromUtf8("recovery é.par2"));
        auto args = settings.arguments(10);
        if (kind == Tool::ParPar) args << "-o";
        args << output << input;
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        QString gpuName;
        if (gpu) {
            process.start(executable, {"--opencl-list", "--json"});
            QVERIFY(process.waitForFinished(30000));
            const auto listing = process.readAll();
            for (const auto &device : openClDevices(listing, false))
                if (device.id == settings.device) gpuName = device.name;
            QVERIFY2(!gpuName.isEmpty(), listing.constData());
        }
        process.start(executable, args);
        QVERIFY(process.waitForFinished(30000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        const auto generationLog = process.readAll();
        QVERIFY2(process.exitCode() == 0, generationLog.constData());
        if (gpu) QVERIFY2(generationLog.contains(gpuName.toUtf8()), generationLog.constData());
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
        QCOMPARE(file.readAll(), original);
    }
};
QTEST_GUILESS_MAIN(TestPar2Settings)
#include "tst_Par2Settings.moc"
