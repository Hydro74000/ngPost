//========================================================================
// ngPost VPN helper protocol v2.
//========================================================================

#include "VpnProtocol.h"

#include <QUrl>

namespace VpnProtocol
{
namespace
{
Type typeFor(QString const &keyword)
{
    static const QMap<QString, Type> types = {
        {QStringLiteral("PROTOCOL"), Type::Protocol},
        {QStringLiteral("READY"), Type::Ready},
        {QStringLiteral("WAITING"), Type::Waiting},
        {QStringLiteral("BUSY"), Type::Busy},
        {QStringLiteral("SUSPECT"), Type::Suspect},
        {QStringLiteral("HEALTHY"), Type::Healthy},
        {QStringLiteral("DOWN"), Type::Down},
        {QStringLiteral("ERROR"), Type::Error},
        {QStringLiteral("RESTART_FAILED"), Type::RestartFailed},
        {QStringLiteral("LEASE_TIMEOUT"), Type::LeaseTimeout},
        {QStringLiteral("LEASE_UNAVAILABLE"), Type::LeaseUnavailable},
        {QStringLiteral("RUNTIME_NOT_VOLATILE"), Type::RuntimeNotVolatile},
        {QStringLiteral("UNATTRIBUTED_VPN_STATE"), Type::UnattributedVpnState},
        {QStringLiteral("LEGACY_OWNER_ACTIVE"), Type::LegacyOwnerActive},
        {QStringLiteral("LOG"), Type::Log}
    };
    return types.value(keyword, Type::Invalid);
}
}

bool Message::isTerminal() const
{
    switch (type) {
    case Type::Busy:
    case Type::Error:
    case Type::LeaseTimeout:
    case Type::LeaseUnavailable:
    case Type::RuntimeNotVolatile:
    case Type::UnattributedVpnState:
    case Type::LegacyOwnerActive:
        return true;
    default:
        return false;
    }
}

QStringList keywords()
{
    return {QStringLiteral("PROTOCOL"), QStringLiteral("READY"),
            QStringLiteral("WAITING"), QStringLiteral("BUSY"),
            QStringLiteral("SUSPECT"), QStringLiteral("HEALTHY"),
            QStringLiteral("DOWN"), QStringLiteral("ERROR"),
            QStringLiteral("RESTART_FAILED"), QStringLiteral("LEASE_TIMEOUT"),
            QStringLiteral("LEASE_UNAVAILABLE"), QStringLiteral("RUNTIME_NOT_VOLATILE"),
            QStringLiteral("UNATTRIBUTED_VPN_STATE"), QStringLiteral("LEGACY_OWNER_ACTIVE"),
            QStringLiteral("LOG")};
}

QString escape(QString const &value)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

QString unescape(QString const &value)
{
    return QUrl::fromPercentEncoding(value.toLatin1());
}

Message parse(QString const &source)
{
    Message result;
    QString const line = source.trimmed();
    if (line.isEmpty())
        return result;

    QStringList const tokens = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.isEmpty())
        return result;
    result.keyword = tokens.first().toUpper();
    result.type = typeFor(result.keyword);
    if (result.type == Type::Invalid)
        return result;

    // Compatibility with helper v1: READY <iface> <ip> [dns], ERROR <text>
    // and LOG <text>. PROTOCOL 2 makes every subsequent record explicitly v2.
    if (result.type == Type::Ready && tokens.size() >= 3
        && !tokens.at(1).contains(QLatin1Char('='))) {
        result.legacy = true;
        result.fields.insert(QStringLiteral("attempt_id"), QStringLiteral("0"));
        result.fields.insert(QStringLiteral("iface"), tokens.at(1));
        result.fields.insert(QStringLiteral("ip"), tokens.at(2));
        if (tokens.size() >= 4)
            result.fields.insert(QStringLiteral("dns"), tokens.at(3));
        return result;
    }
    if ((result.type == Type::Error || result.type == Type::Log)
        && tokens.size() >= 2 && !tokens.at(1).contains(QLatin1Char('='))) {
        result.legacy = true;
        result.detail = line.mid(result.keyword.size()).trimmed();
        return result;
    }

    if (result.type == Type::Protocol) {
        if (tokens.size() == 2 && tokens.at(1) == QLatin1String("2"))
            result.fields.insert(QStringLiteral("version"), QStringLiteral("2"));
        else
            result.type = Type::Invalid;
        return result;
    }

    for (int i = 1; i < tokens.size(); ++i) {
        QString const token = tokens.at(i);
        int const equal = token.indexOf(QLatin1Char('='));
        if (equal <= 0) {
            result.type = Type::Invalid;
            result.fields.clear();
            return result;
        }
        QString const key = token.left(equal);
        QString const value = unescape(token.mid(equal + 1));
        if (result.fields.contains(key)) {
            result.type = Type::Invalid;
            result.fields.clear();
            return result;
        }
        result.fields.insert(key, value);
    }
    result.detail = result.fields.value(QStringLiteral("detail"));

    QStringList required;
    switch (result.type) {
    case Type::Ready: required = {QStringLiteral("attempt_id"), QStringLiteral("iface"),
                                  QStringLiteral("ip"), QStringLiteral("dns")}; break;
    case Type::Waiting: required = {QStringLiteral("owner_pid"), QStringLiteral("helper_pid"),
                                    QStringLiteral("owner_uid"), QStringLiteral("backend"),
                                    QStringLiteral("since"), QStringLiteral("deadline")}; break;
    case Type::Busy: required = {QStringLiteral("owner_pid"), QStringLiteral("helper_pid"),
                                 QStringLiteral("owner_uid"), QStringLiteral("backend"),
                                 QStringLiteral("since"), QStringLiteral("age")}; break;
    case Type::Suspect:
    case Type::Down: required = {QStringLiteral("backend"), QStringLiteral("reason")}; break;
    case Type::Healthy: required = {QStringLiteral("backend")}; break;
    case Type::Error: required = {QStringLiteral("failure"), QStringLiteral("detail")}; break;
    case Type::RestartFailed: required = {QStringLiteral("attempt_id"),
                                          QStringLiteral("failure"),
                                          QStringLiteral("detail")}; break;
    case Type::LeaseTimeout: required = {QStringLiteral("owner_pid"),
                                         QStringLiteral("waited_seconds")}; break;
    case Type::LeaseUnavailable: required = {QStringLiteral("path"),
                                             QStringLiteral("detail")}; break;
    case Type::RuntimeNotVolatile: required = {QStringLiteral("path"),
                                               QStringLiteral("fstype")}; break;
    case Type::UnattributedVpnState: required = {QStringLiteral("resources")}; break;
    case Type::LegacyOwnerActive: required = {QStringLiteral("owner_pid"),
                                              QStringLiteral("resources")}; break;
    case Type::Log: required = {QStringLiteral("level"), QStringLiteral("detail")}; break;
    default: break;
    }
    for (QString const &key : required) {
        if (!result.fields.contains(key)) {
            result.type = Type::Invalid;
            result.fields.clear();
            result.detail.clear();
            break;
        }
    }
    return result;
}
}
