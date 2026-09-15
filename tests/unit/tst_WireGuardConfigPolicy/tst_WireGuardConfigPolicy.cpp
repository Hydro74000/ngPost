// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_WireGuardConfigPolicy.cpp — what a WireGuard profile may ask of the
// privileged process that reads it.
//
// On Windows the .conf goes to `wireguard.exe /installtunnelservice`, which
// ngPost runs elevated, and the tunnel service it produces runs as SYSTEM.
// The file sits in the user's own configuration folder, so any process running
// as that user can write it. The Linux helper has sanitised these profiles
// since revision 4; this is the same policy, enforced before the UAC prompt.
//
//========================================================================

#include "vpn/WireGuardConfigPolicy.h"

#include <QtTest>
#include <QRegularExpression>
#include <QTemporaryDir>

namespace
{
//! The key lists the privileged helper carries, read back out of the script.
//! They are a second implementation of the same policy, and the two drifting
//! apart is exactly what nobody notices until a key is refused on one side and
//! accepted on the other.
QStringList helperList(QString const &variable, bool *ok)
{
    *ok = false;
    QFile script(QStringLiteral(NGPOST_SOURCE_ROOT "/src/vpn/scripts/ngpost-vpn-helper.sh"));
    if (!script.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QString const text  = QString::fromUtf8(script.readAll());
    QString const start = variable + QStringLiteral("=\"");
    int const     from  = text.indexOf(start);
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

//! The same lists again, this time out of the elevated PowerShell installer.
//! Three implementations of one policy: this one is the copy that actually
//! defends the Windows boundary, because it validates a file the user can no
//! longer rewrite. A key accepted here and refused in C++ would surface as a
//! UAC prompt that then fails; the reverse would be a hole.
QStringList powershellList(QString const &variable, bool *ok)
{
    *ok = false;
    QFile script(QStringLiteral(NGPOST_SOURCE_ROOT
                                "/src/vpn/scripts/win/install-wg-tunnel.ps1"));
    if (!script.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QString const text  = QString::fromUtf8(script.readAll());
    QString const start = QLatin1Char('$') + variable;
    int const     from  = text.indexOf(start);
    if (from < 0)
        return {};
    int const open = text.indexOf(QStringLiteral("@("), from);
    if (open < 0)
        return {};
    int const close = text.indexOf(QLatin1Char(')'), open);
    if (close < 0)
        return {};

    QStringList list;
    QRegularExpression const token(QStringLiteral("'([^']*)'"));
    auto it = token.globalMatch(text.mid(open, close - open));
    while (it.hasNext())
        list << it.next().captured(1);
    list.removeDuplicates();
    list.sort();
    *ok = !list.isEmpty();
    return list;
}

QByteArray profile(QStringList const &lines)
{
    return lines.join(QLatin1Char('\n')).toUtf8();
}

//! A profile that must always be accepted, so a test that adds one line to it
//! is measuring that line and nothing else.
QStringList baseline()
{
    return { QStringLiteral("[Interface]"),
             QStringLiteral("PrivateKey = aGVsbG8gdGhlcmUgZnJpZW5kIG9mIG1pbmUgb2s="),
             QStringLiteral("Address = 10.2.0.2/32"),
             QStringLiteral("DNS = 10.2.0.1"),
             QStringLiteral(""),
             QStringLiteral("[Peer]"),
             QStringLiteral("PublicKey = c29tZSBvdGhlciBrZXkgdGhhdCBpcyBub3QgcmVhbA="),
             QStringLiteral("AllowedIPs = 0.0.0.0/0"),
             QStringLiteral("Endpoint = vpn.example.net:51820") };
}
} // namespace

class TestWireGuardConfigPolicy : public QObject
{
    Q_OBJECT

private slots:
    //! A profile of the shape every provider hands out must go through.
    void a_real_profile_is_accepted();

    //! The four keys whose value is a command line. WireGuard for Windows only
    //! runs them when DangerousScriptExecution is enabled machine-wide, but
    //! ngPost needs none of them and cannot strip them the way the Linux helper
    //! does -- it hands the file over unchanged. So they are refused, and the
    //! message names the key, because the user has to know which line to remove.
    void script_keys_are_refused();
    void script_keys_are_refused_data();

    //! Anything the policy was never taught about is refused rather than passed
    //! through, the same trade OpenVpnConfigPolicy makes.
    void an_unreviewed_key_is_refused();

    //! Keys are section-scoped: a PrivateKey under [Peer] is not a profile
    //! anyone meant to write.
    void a_known_key_in_the_wrong_section_is_refused();

    //! Only [Interface] and [Peer] exist.
    void an_unknown_section_is_refused();

    //! A key before any section header has no meaning.
    void a_key_outside_any_section_is_refused();

    //! Comments, blank lines, CRLF endings and odd spacing are ordinary.
    void comments_blank_lines_and_crlf_are_tolerated();

    //! Case is irrelevant in WireGuard profiles, so it must be here too --
    //! otherwise "postup" would be refused and "PostUp" would not.
    void key_matching_is_case_insensitive();
    void inline_comments_are_removed_before_parsing();

    //! The profile may be any file the caller named. A token that is not one of
    //! ours must never come back verbatim, or the refusal becomes a way to read
    //! a file the caller could not open.
    void an_unknown_key_is_never_echoed_back();

    //! Binary content and oversized files are refused as a whole.
    void binary_and_oversized_profiles_are_refused();

    //! inspectFile() on something that is not a readable file.
    void inspectFile_reports_an_unreadable_profile();
    void inspectFile_accepts_a_real_profile_on_disk();

    //! The privileged helper carries its own copy of these lists.
    void helper_script_lists_match();

    //! And so does the elevated Windows installer, which is the copy that
    //! actually defends the boundary there (it validates a staged file the user
    //! can no longer rewrite). Three implementations, one policy.
    void windows_installer_lists_match();

    //! ...except for the four script keys, where the difference is deliberate:
    //! the helper accepts them because prepare_wg_config strips them before
    //! `wg setconf`, and Windows cannot. Pinned so it stays a decision.
    void dangerous_keys_are_known_but_never_accepted();
};

void TestWireGuardConfigPolicy::a_real_profile_is_accepted()
{
    auto const verdict = WireGuardConfigPolicy::inspect(profile(baseline()));
    QVERIFY2(verdict.isAccepted(), qPrintable(verdict.reason));
}

void TestWireGuardConfigPolicy::script_keys_are_refused_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<QString>("key");

    QTest::newRow("PostUp")   << QStringLiteral("PostUp = /bin/sh -c whoami")   << QStringLiteral("postup");
    QTest::newRow("PreUp")    << QStringLiteral("PreUp = C:\\evil.exe")         << QStringLiteral("preup");
    QTest::newRow("PostDown") << QStringLiteral("PostDown = calc.exe")          << QStringLiteral("postdown");
    QTest::newRow("PreDown")  << QStringLiteral("PreDown = powershell -c ls")   << QStringLiteral("predown");
}

void TestWireGuardConfigPolicy::script_keys_are_refused()
{
    QFETCH(QString, line);
    QFETCH(QString, key);

    QStringList lines = baseline();
    lines.insert(4, line);

    auto const verdict = WireGuardConfigPolicy::inspect(profile(lines));

    QVERIFY(!verdict.isAccepted());
    QCOMPARE(verdict.outcome, WireGuardConfigPolicy::Outcome::DangerousKey);
    QCOMPARE(verdict.key, key);
    QCOMPARE(verdict.lineNumber, 5);
    QVERIFY2(verdict.reason.contains(key), qPrintable(verdict.reason));
}

void TestWireGuardConfigPolicy::an_unreviewed_key_is_refused()
{
    QStringList lines = baseline();
    lines.insert(4, QStringLiteral("SomeFutureKey = 1"));

    auto const verdict = WireGuardConfigPolicy::inspect(profile(lines));

    QVERIFY(!verdict.isAccepted());
    QCOMPARE(verdict.outcome, WireGuardConfigPolicy::Outcome::UnknownKey);
    QCOMPARE(verdict.lineNumber, 5);
}

void TestWireGuardConfigPolicy::a_known_key_in_the_wrong_section_is_refused()
{
    QStringList lines = baseline();
    lines << QStringLiteral("PrivateKey = bm90IHdoZXJlIHRoaXMgYmVsb25ncyBhdCBhbGw=");

    auto const verdict = WireGuardConfigPolicy::inspect(profile(lines));

    QVERIFY(!verdict.isAccepted());
    QCOMPARE(verdict.outcome, WireGuardConfigPolicy::Outcome::UnknownKey);
    // Known key, so naming it back leaks nothing and helps the user.
    QCOMPARE(verdict.key, QStringLiteral("privatekey"));
}

void TestWireGuardConfigPolicy::an_unknown_section_is_refused()
{
    auto const verdict = WireGuardConfigPolicy::inspect(
        profile({ QStringLiteral("[Interface]"),
                  QStringLiteral("Address = 10.2.0.2/32"),
                  QStringLiteral("[Script]"),
                  QStringLiteral("Run = whoami") }));

    QVERIFY(!verdict.isAccepted());
    QCOMPARE(verdict.outcome, WireGuardConfigPolicy::Outcome::UnknownSection);
    QCOMPARE(verdict.lineNumber, 3);
}

void TestWireGuardConfigPolicy::a_key_outside_any_section_is_refused()
{
    auto const verdict = WireGuardConfigPolicy::inspect(
        profile({ QStringLiteral("Address = 10.2.0.2/32"),
                  QStringLiteral("[Interface]") }));

    QVERIFY(!verdict.isAccepted());
    QCOMPARE(verdict.outcome, WireGuardConfigPolicy::Outcome::Malformed);
    QCOMPARE(verdict.lineNumber, 1);
}

void TestWireGuardConfigPolicy::comments_blank_lines_and_crlf_are_tolerated()
{
    QByteArray const config =
        "# provider profile\r\n"
        "; another comment style\r\n"
        "[Interface]\r\n"
        "   PrivateKey   =   aGVsbG8gdGhlcmUgZnJpZW5kIG9mIG1pbmUgb2s=   \r\n"
        "Address = 10.2.0.2/32\r\n"
        "\r\n"
        "[Peer]\r\n"
        "PublicKey = c29tZSBvdGhlciBrZXkgdGhhdCBpcyBub3QgcmVhbA=\r\n"
        "AllowedIPs = 0.0.0.0/0\r\n";

    auto const verdict = WireGuardConfigPolicy::inspect(config);
    QVERIFY2(verdict.isAccepted(), qPrintable(verdict.reason));
}

void TestWireGuardConfigPolicy::inline_comments_are_removed_before_parsing()
{
    auto lines = baseline();
    lines[0] += QStringLiteral(" # interface = ignored");
    lines[5] += QStringLiteral(" # peer");
    lines[1] += QStringLiteral(" # PostUp = ignored comment");
    QVERIFY(WireGuardConfigPolicy::inspect(profile(lines)).isAccepted());
    lines.insert(4, QStringLiteral("PostUp = whoami # still dangerous"));
    const auto verdict = WireGuardConfigPolicy::inspect(profile(lines));
    QCOMPARE(verdict.outcome, WireGuardConfigPolicy::Outcome::DangerousKey);
    QCOMPARE(verdict.lineNumber, 5);
}

void TestWireGuardConfigPolicy::key_matching_is_case_insensitive()
{
    QStringList lines = baseline();
    lines.insert(4, QStringLiteral("pOsTuP = whoami"));

    auto const verdict = WireGuardConfigPolicy::inspect(profile(lines));

    QVERIFY(!verdict.isAccepted());
    QCOMPARE(verdict.outcome, WireGuardConfigPolicy::Outcome::DangerousKey);
    QCOMPARE(verdict.key, QStringLiteral("postup"));
}

void TestWireGuardConfigPolicy::an_unknown_key_is_never_echoed_back()
{
    QStringList lines = baseline();
    lines.insert(4, QStringLiteral("root:x:0:0:secret-account-name:/root:/bin/bash = 1"));

    auto const verdict = WireGuardConfigPolicy::inspect(profile(lines));

    QVERIFY(!verdict.isAccepted());
    QCOMPARE(verdict.key, QStringLiteral("withheld"));
    QVERIFY2(!verdict.reason.contains(QStringLiteral("root")), qPrintable(verdict.reason));
    QVERIFY2(!verdict.reason.contains(QStringLiteral("secret")), qPrintable(verdict.reason));
    // The line number is always safe, and it is what locates the problem.
    QCOMPARE(verdict.lineNumber, 5);
}

void TestWireGuardConfigPolicy::binary_and_oversized_profiles_are_refused()
{
    QByteArray binary = profile(baseline());
    binary.append('\0');
    auto const fromBinary = WireGuardConfigPolicy::inspect(binary);
    QVERIFY(!fromBinary.isAccepted());
    QCOMPARE(fromBinary.outcome, WireGuardConfigPolicy::Outcome::Malformed);

    QByteArray huge(WireGuardConfigPolicy::kMaxConfigBytes + 1, 'A');
    auto const fromHuge = WireGuardConfigPolicy::inspect(huge);
    QVERIFY(!fromHuge.isAccepted());
    QCOMPARE(fromHuge.outcome, WireGuardConfigPolicy::Outcome::Malformed);
}

void TestWireGuardConfigPolicy::inspectFile_reports_an_unreadable_profile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    auto const missing = WireGuardConfigPolicy::inspectFile(dir.filePath("nope.conf"));
    QVERIFY(!missing.isAccepted());
    QCOMPARE(missing.outcome, WireGuardConfigPolicy::Outcome::Malformed);

    // A directory is not a profile either.
    auto const notAFile = WireGuardConfigPolicy::inspectFile(dir.path());
    QVERIFY(!notAFile.isAccepted());
    QCOMPARE(notAFile.outcome, WireGuardConfigPolicy::Outcome::Malformed);
}

void TestWireGuardConfigPolicy::inspectFile_accepts_a_real_profile_on_disk()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString const path = dir.filePath("wg0.conf");

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(profile(baseline())) > 0);
    file.close();

    auto const verdict = WireGuardConfigPolicy::inspectFile(path);
    QVERIFY2(verdict.isAccepted(), qPrintable(verdict.reason));
}

void TestWireGuardConfigPolicy::helper_script_lists_match()
{
    bool              ok         = false;
    QStringList const interfaces = helperList(QStringLiteral("WG_INTERFACE_KEYS"), &ok);
    QVERIFY2(ok, "could not read WG_INTERFACE_KEYS out of the helper script");
    QCOMPARE(interfaces, WireGuardConfigPolicy::interfaceKeys());

    ok                    = false;
    QStringList const peers = helperList(QStringLiteral("WG_PEER_KEYS"), &ok);
    QVERIFY2(ok, "could not read WG_PEER_KEYS out of the helper script");
    QCOMPARE(peers, WireGuardConfigPolicy::peerKeys());
}

void TestWireGuardConfigPolicy::windows_installer_lists_match()
{
    bool              ok         = false;
    QStringList const interfaces = powershellList(QStringLiteral("WgInterfaceKeys"), &ok);
    QVERIFY2(ok, "could not read $WgInterfaceKeys out of install-wg-tunnel.ps1");
    QCOMPARE(interfaces, WireGuardConfigPolicy::interfaceKeys());

    ok                      = false;
    QStringList const peers = powershellList(QStringLiteral("WgPeerKeys"), &ok);
    QVERIFY2(ok, "could not read $WgPeerKeys out of install-wg-tunnel.ps1");
    QCOMPARE(peers, WireGuardConfigPolicy::peerKeys());

    ok                          = false;
    QStringList const dangerous = powershellList(QStringLiteral("WgDangerousKeys"), &ok);
    QVERIFY2(ok, "could not read $WgDangerousKeys out of install-wg-tunnel.ps1");
    QCOMPARE(dangerous, WireGuardConfigPolicy::dangerousKeys());
}

void TestWireGuardConfigPolicy::dangerous_keys_are_known_but_never_accepted()
{
    QStringList const dangerous = WireGuardConfigPolicy::dangerousKeys();
    QCOMPARE(dangerous, (QStringList{ QStringLiteral("postdown"), QStringLiteral("postup"),
                                      QStringLiteral("predown"), QStringLiteral("preup") }));

    // Recognised, so the refusal can name them -- and so the list stays equal to
    // the helper's, which accepts them because it strips them.
    QStringList const known = WireGuardConfigPolicy::interfaceKeys();
    for (QString const &key : dangerous)
        QVERIFY2(known.contains(key), qPrintable(key));
}

QTEST_APPLESS_MAIN(TestWireGuardConfigPolicy)
#include "tst_WireGuardConfigPolicy.moc"
