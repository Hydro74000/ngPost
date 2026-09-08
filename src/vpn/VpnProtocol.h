//========================================================================
// ngPost VPN helper protocol v2.
//========================================================================

#ifndef VPNPROTOCOL_H
#define VPNPROTOCOL_H

#include <QMap>
#include <QString>
#include <QStringList>

namespace VpnProtocol
{
enum class Type {
    Invalid,
    Protocol,
    Ready,
    Waiting,
    Busy,
    Suspect,
    Healthy,
    Down,
    Error,
    RestartFailed,
    LeaseTimeout,
    LeaseUnavailable,
    RuntimeNotVolatile,
    UnattributedVpnState,
    LegacyOwnerActive,
    Log
};

struct Message {
    Type type = Type::Invalid;
    QString keyword;
    QMap<QString, QString> fields;
    QString detail;
    bool legacy = false;

    bool isTerminal() const;
};

QStringList keywords();
Message parse(QString const &line);
QString escape(QString const &value);
QString unescape(QString const &value);
}

#endif // VPNPROTOCOL_H
