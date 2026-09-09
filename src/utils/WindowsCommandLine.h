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
    return result + QString(2 * slashes, QLatin1Char('\\')) + QLatin1Char('"');
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
}
#endif
