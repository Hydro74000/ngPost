#ifndef WINDOWS_COMMAND_LINE_H
#define WINDOWS_COMMAND_LINE_H

#include <QStringList>

namespace WindowsCommandLine {
// CRT/CommandLineToArgvW escaping, NOT PowerShell quoting. Always quote, also
// for empty arguments; double backslashes before a quote and at the end.
inline QString quoteArgument(QString const &argument)
{
    QString result = QStringLiteral("\"");
    int slashes = 0;
    for (QChar c : argument) {
        if (c == QLatin1Char('\\')) { ++slashes; continue; }
        result += QString(c == QLatin1Char('"') ? 2 * slashes + 1 : slashes,
                          QLatin1Char('\\'));
        result += c;
        slashes = 0;
    }
    return result + QString(2 * qsizetype(slashes), QLatin1Char('\\')) + QLatin1Char('"');
}

inline QString serialize(QStringList const &arguments)
{
    QStringList escaped;
    for (QString const &argument : arguments) escaped << quoteArgument(argument);
    return escaped.join(QLatin1Char(' '));
}

inline QString powershellLiteral(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

// RunAs exposes only the child exit code. Preserve ERROR_CANCELLED separately
// from other launch failures; PowerShell may wrap the Win32Exception.
inline QString elevatedPowerShellCommand(QString const &executable, QStringList const &arguments)
{
    return QStringLiteral(
        "$ErrorActionPreference='Stop'; try { "
        "$p = Start-Process -FilePath %1 -Verb RunAs -Wait -PassThru -ArgumentList %2; "
        "exit $p.ExitCode "
        "} catch { "
        "$launchException = $_.Exception; "
        "while ($null -ne $launchException) { "
        "if ($launchException -is [System.ComponentModel.Win32Exception] -and "
        "$launchException.NativeErrorCode -eq 1223) { exit 1223 }; "
        "$launchException = $launchException.InnerException "
        "}; exit 1 }")
        .arg(powershellLiteral(executable), powershellLiteral(serialize(arguments)));
}

}
#endif
