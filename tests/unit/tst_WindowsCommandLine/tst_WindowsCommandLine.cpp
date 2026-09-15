#include <QtTest>
#include "../../../src/utils/WindowsCommandLine.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

class TestWindowsCommandLine : public QObject {
    Q_OBJECT
private slots:
    void quoting() {
        QCOMPARE(WindowsCommandLine::quoteArgument(""), QString("\"\""));
        QCOMPARE(WindowsCommandLine::quoteArgument("a b"), QString("\"a b\""));
        QCOMPARE(WindowsCommandLine::quoteArgument("a\"b"), QString("\"a\\\"b\""));
        QCOMPARE(WindowsCommandLine::quoteArgument("C:\\end\\"), QString("\"C:\\end\\\\\""));
        QCOMPARE(WindowsCommandLine::powershellLiteral("O'Brien $test"), QString("'O''Brien $test'"));
    }
    void nativeRoundTrip() {
#ifdef Q_OS_WIN
        QStringList expected = {"powershell.exe", "-File", "D:\\Program Files\\O'Brien\\install.ps1",
                                "-ConfPath", "D:\\users\\é $&;[]\\test.conf", "", "tail\\", "a\\\"b"};
        QString const line = WindowsCommandLine::serialize(expected);
        int count = 0;
        auto argv = CommandLineToArgvW(reinterpret_cast<LPCWSTR>(line.utf16()), &count);
        QVERIFY(argv);
        QStringList actual;
        for (int i = 0; i < count; ++i) actual << QString::fromWCharArray(argv[i]);
        LocalFree(argv);
        QCOMPARE(actual, expected);
#else
        QSKIP("Native Win32 parser requires Windows");
#endif
    }
    void elevationResult_data() {
        QTest::addColumn<QString>("behavior");
        QTest::addColumn<int>("expected");
        QTest::newRow("cancelled") << QStringLiteral("throw [System.ComponentModel.Win32Exception]::new(1223)") << 1223;
        QTest::newRow("wrapped-cancellation") << QStringLiteral("throw [System.InvalidOperationException]::new('wrapped', [System.ComponentModel.Win32Exception]::new(1223))") << 1223;
        QTest::newRow("access-denied") << QStringLiteral("throw [System.ComponentModel.Win32Exception]::new(5)") << 1;
        QTest::newRow("text-is-not-a-native-code") << QStringLiteral("throw 'error 1223'") << 1;
        QTest::newRow("child-failed") << QStringLiteral("[pscustomobject]@{ ExitCode = 5 }") << 5;
        QTest::newRow("success") << QStringLiteral("[pscustomobject]@{ ExitCode = 0 }") << 0;
    }
    void elevationResult() {
#ifdef Q_OS_WIN
        QFETCH(QString, behavior);
        QFETCH(int, expected);
        wchar_t system[MAX_PATH + 1] = {};
        QVERIFY(GetSystemDirectoryW(system, MAX_PATH + 1));
        QString const exe = QString::fromWCharArray(system) + "/WindowsPowerShell/v1.0/powershell.exe";
        // Replace only Start-Process, so no UAC interaction is needed. The
        // production wrapper must handle PowerShell's actual exception chain.
        QString const stub = QStringLiteral(
            "function Start-Process { param($FilePath, $Verb, [switch]$Wait, [switch]$PassThru, $ArgumentList) "
            "if ($Verb -ne 'RunAs' -or -not $Wait -or -not $PassThru) { exit 99 }; %1 }; ").arg(behavior);
        QProcess process;
        process.start(exe, {"-NoProfile", "-NonInteractive", "-Command", stub +
            WindowsCommandLine::elevatedPowerShellCommand(exe, {"-NoProfile", "-File", "unused.ps1"})});
        QVERIFY(process.waitForFinished(15000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), expected);
#else
        QSKIP("PowerShell elevation result handling requires Windows");
#endif
    }
    void powershellFileRoundTrip() {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString const script = directory.filePath("Program Files O'Brien.ps1");
        QString const output = directory.filePath("result.txt");
        QString const value = "D:\\users\\O'Brien $&;[]\\test.conf";
        QFile file(script);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("param([string]$Value,[string]$Output)\n[IO.File]::WriteAllText($Output,$Value)\n");
        file.close();
        wchar_t system[MAX_PATH + 1] = {};
        QVERIFY(GetSystemDirectoryW(system, MAX_PATH + 1));
        QString const exe = QString::fromWCharArray(system) + "/WindowsPowerShell/v1.0/powershell.exe";
        QString const arguments = WindowsCommandLine::serialize({"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script,
                                                                 "-Value", value, "-Output", output});
        QString const command = QString("$ErrorActionPreference='Stop'; $p=Start-Process -FilePath %1 -Wait -PassThru -ArgumentList %2; exit $p.ExitCode")
            .arg(WindowsCommandLine::powershellLiteral(exe), WindowsCommandLine::powershellLiteral(arguments));
        QProcess process;
        process.start(exe, {"-NoProfile", "-Command", command});
        QVERIFY(process.waitForFinished(15000));
        QCOMPARE(process.exitCode(), 0);
        QFile result(output);
        QVERIFY(result.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(result.readAll()), value);
#else
        QSKIP("PowerShell -File round trip requires Windows");
#endif
    }
};
QTEST_APPLESS_MAIN(TestWindowsCommandLine)
#include "tst_WindowsCommandLine.moc"
