// Copyright (C) 2026 Hydro74000. GPL-3.0-or-later.
#ifndef LOGTIMESTAMP_H
#define LOGTIMESTAMP_H
#include <QDateTime>
#include <QRegularExpression>
#include <QString>

// Used only by diagnostic sinks, never by JSON/NZB output. A QProcess can
// deliver half a line at a time; progress fragments share a single timestamp.
class LogTimestamp
{
public:
    //! What a bare carriage return means to the sink: a log pane starts a new
    //! line, a terminal (and a file read like one) rewrites the current one.
    enum class CarriageReturn {
        BreaksLine,
        RewritesLine
    };

    explicit LogTimestamp(CarriageReturn carriageReturn = CarriageReturn::BreaksLine)
        : _carriageReturn(carriageReturn)
    {
    }

    QString format(QString text, bool newEntry)
    {
        if (newEntry) {
            // The caller starts the new line itself, so a carriage return
            // still held back has nothing left to rewrite.
            _lineStart = true;
            _previousCr = false;
        }
        QString result;
        result.reserve(text.size() + 32);
        const QString stamp = QDateTime::currentDateTime().toString(
            QStringLiteral("[HH:mm:ss.zzz] "));
        static const QRegularExpression existing(
            QStringLiteral("^\\[\\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\]"));
        for (qsizetype pos = 0; pos < text.size();) {
            const QChar ch = text.at(pos);
            if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n') || ch == QChar(0x2028)
                || ch == QChar(0x2029)) {
                // CRLF may itself be split between two QProcess chunks. When
                // CR rewrites the line, it is held back until the next
                // character shows it is not the first half of a CRLF.
                if (_carriageReturn == CarriageReturn::RewritesLine) {
                    if (ch != QLatin1Char('\r'))
                        result += QLatin1Char('\n');
                } else if (!(ch == QLatin1Char('\n') && _previousCr))
                    result += QLatin1Char('\n');
                _previousCr = ch == QLatin1Char('\r');
                _lineStart = true;
                ++pos;
                continue;
            }
            if (_previousCr && _carriageReturn == CarriageReturn::RewritesLine)
                result += QLatin1Char('\r');
            _previousCr = false;
            if (_lineStart) {
                if (!existing.match(text.mid(pos, 15)).hasMatch())
                    result += stamp;
                _lineStart = false;
            }
            qsizetype end = pos + 1;
            while (end < text.size() && text.at(end) != QLatin1Char('\r')
                   && text.at(end) != QLatin1Char('\n') && text.at(end) != QChar(0x2028)
                   && text.at(end) != QChar(0x2029))
                ++end;
            result += text.mid(pos, end - pos);
            pos = end;
        }
        return result;
    }

private:
    CarriageReturn _carriageReturn;
    bool _lineStart = true, _previousCr = false;
};
#endif
