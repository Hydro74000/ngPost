// Minimal coordinator dependency; the updater itself comes verbatim from v5.5.1.
#pragma once
#include <QString>
class NgPost
{
public:
    inline static const QString sVersion = QStringLiteral("5.5.1");
    qint64 _lastUpdateCheckEpoch = 0;
    void saveConfig() { }
};
