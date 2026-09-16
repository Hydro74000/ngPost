// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_WindowsSecurity.cpp — Windows-only ACL hardening for ephemeral VPN
// state.
//
// WindowsSecurity::protectOwnerAndSystem() is what stops the OpenVPN
// management password file and the machine-wide owner manifest -- both under
// ProgramData, which is world-readable by default -- from being readable by
// every account on the machine. It applies a PROTECTED DACL granting only
// SYSTEM and the owner.
//
// The failure is silent: a descriptor that fails to apply, or one that keeps
// inheriting ProgramData's permissive ACEs, leaves the file exactly as exposed
// as it was while the function reports success. So the assertions read the ACL
// back rather than trusting the return value.
//
// SID policy tests run on every platform; NTFS enforcement tests run on Windows.
//
//========================================================================

#include <QtTest>

// Not inside the guard below: isPrivilegedTrusteeSid() is portable, and the
// tests that pin it run on every platform.
#include "vpn/WindowsSecurity.h"

#ifdef Q_OS_WIN
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#endif

class TestWindowsSecurity : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    //! Which principals may hold write access to a script ngPost is about to
    //! launch through `Start-Process -Verb RunAs`. The enforcement needs a
    //! DACL and therefore Windows, but the policy itself is a decision about
    //! SIDs, so it is settled here on every platform -- the rule is the part
    //! worth pinning, and the portable zip makes getting it wrong an
    //! administrator shell.
    void privileged_trustees_are_the_three_administrative_sids();
    void ordinary_and_nearly_privileged_sids_are_not_trusted();
    void privileged_sid_matching_ignores_case_and_padding();

#ifdef Q_OS_WIN
    //! A file it can reach comes back secured, and the ACL actually says so.
    void protect_applies_a_protected_dacl_to_a_file();

    //! Same for a directory: the runtime folder is secured before anything is
    //! written into it.
    void protect_applies_a_protected_dacl_to_a_directory();

    //! The DACL must be PROTECTED. Without that bit the object keeps
    //! inheriting ProgramData's ACEs and stays readable by everyone, while
    //! the call still reports success.
    void protected_dacl_blocks_inheritance();

    //! A path that does not exist must be reported as a failure rather than
    //! silently treated as secured.
    void protect_reports_failure_on_a_missing_path();
    void elevated_script_path_policy_data();
    void elevated_script_path_policy();
    void elevated_script_rejects_junction_ancestors();
    void elevated_script_accepts_wof_compression();
#endif
};

void TestWindowsSecurity::initTestCase()
{
    // No QSKIP here any more. The DACL tests below are #ifdef'd out off
    // Windows, but isPrivilegedTrusteeSid() is portable, and skipping the whole
    // suite would have left that policy unexercised on two CI platforms out of
    // three.
}

void TestWindowsSecurity::privileged_trustees_are_the_three_administrative_sids()
{
    // LocalSystem, BUILTIN\Administrators, and the TrustedInstaller service SID
    // that owns the Program Files tree. Write access held by any of these
    // grants nothing an administrator could not already take.
    QVERIFY(WindowsSecurity::isPrivilegedTrusteeSid(QStringLiteral("S-1-5-18")));
    QVERIFY(WindowsSecurity::isPrivilegedTrusteeSid(QStringLiteral("S-1-5-32-544")));
    QVERIFY(WindowsSecurity::isPrivilegedTrusteeSid(QStringLiteral(
        "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464")));
}

void TestWindowsSecurity::ordinary_and_nearly_privileged_sids_are_not_trusted()
{
    // A normal user account: the case the portable zip creates, and the whole
    // reason the check exists.
    QVERIFY(!WindowsSecurity::isPrivilegedTrusteeSid(
        QStringLiteral("S-1-5-21-1004336348-1177238915-682003330-1001")));

    // Everyone, Authenticated Users, Users, INTERACTIVE.
    for (QString const &sid : { QStringLiteral("S-1-1-0"), QStringLiteral("S-1-5-11"),
                                QStringLiteral("S-1-5-32-545"), QStringLiteral("S-1-5-4") })
        QVERIFY2(!WindowsSecurity::isPrivilegedTrusteeSid(sid), qPrintable(sid));

    // Administrative in practice, but not the default owner of an install
    // directory: finding one of them with write access is worth reporting, not
    // waving through. Backup, Print and Server Operators.
    for (QString const &sid : { QStringLiteral("S-1-5-32-551"), QStringLiteral("S-1-5-32-550"),
                                QStringLiteral("S-1-5-32-549") })
        QVERIFY2(!WindowsSecurity::isPrivilegedTrusteeSid(sid), qPrintable(sid));

    // CREATOR OWNER appears in the default Program Files DACL, but as an
    // inherit-only entry: it describes what children get, never this object.
    QVERIFY(!WindowsSecurity::isPrivilegedTrusteeSid(QStringLiteral("S-1-3-0")));

    // A prefix of a trusted SID is a different account.
    QVERIFY(!WindowsSecurity::isPrivilegedTrusteeSid(QStringLiteral("S-1-5-32-5440")));
    QVERIFY(!WindowsSecurity::isPrivilegedTrusteeSid(QStringLiteral("S-1-5-1")));
    QVERIFY(!WindowsSecurity::isPrivilegedTrusteeSid(QString()));
}

void TestWindowsSecurity::privileged_sid_matching_ignores_case_and_padding()
{
    // ConvertSidToStringSidW yields upper case, but nothing guarantees the
    // spelling of a SID that reached us another way.
    QVERIFY(WindowsSecurity::isPrivilegedTrusteeSid(QStringLiteral("s-1-5-32-544")));
    QVERIFY(WindowsSecurity::isPrivilegedTrusteeSid(QStringLiteral("  S-1-5-18  ")));
}

#ifdef Q_OS_WIN

namespace
{
//! Read the object's DACL back. Returns false if it cannot be read at all.
//! \a descriptor must be LocalFree'd by the caller when this returns true.
bool readDacl(QString const &path, PSECURITY_DESCRIPTOR *descriptor,
              PACL *dacl, SECURITY_DESCRIPTOR_CONTROL *control)
{
    *descriptor = nullptr;
    *dacl       = nullptr;
    DWORD const rc =
        GetNamedSecurityInfoW(reinterpret_cast<LPCWSTR>(path.utf16()),
                              SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, dacl, nullptr, descriptor);
    if (rc != ERROR_SUCCESS)
        return false;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(*descriptor, control, &revision)) {
        LocalFree(*descriptor);
        *descriptor = nullptr;
        return false;
    }
    return true;
}

QString makeFile(QTemporaryDir const &dir, QString const &name)
{
    QString const path = QDir(dir.path()).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    file.write("secret");
    file.close();
    return path;
}
} // namespace

void TestWindowsSecurity::protect_applies_a_protected_dacl_to_a_file()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString const path = makeFile(dir, QStringLiteral("management-token"));
    QVERIFY(!path.isEmpty());

    QVERIFY2(WindowsSecurity::protectOwnerAndSystem(path),
             "securing a file the test just created must succeed");

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    SECURITY_DESCRIPTOR_CONTROL control{};
    QVERIFY2(readDacl(path, &descriptor, &dacl, &control), "could not read the DACL back");
    QVERIFY2(dacl != nullptr, "a null DACL means no access control at all, i.e. everyone");
    // The SDDL grants SYSTEM and the owner, and nothing else.
    QVERIFY2(dacl->AceCount == 2,
             qPrintable(QStringLiteral("expected exactly SYSTEM and owner, got %1 ACE(s)")
                            .arg(dacl->AceCount)));
    LocalFree(descriptor);
}

void TestWindowsSecurity::protect_applies_a_protected_dacl_to_a_directory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString const sub = QDir(dir.path()).filePath(QStringLiteral("vpn-runtime"));
    QVERIFY(QDir().mkpath(sub));

    QVERIFY2(WindowsSecurity::protectOwnerAndSystem(sub),
             "securing the runtime directory must succeed");

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    SECURITY_DESCRIPTOR_CONTROL control{};
    QVERIFY(readDacl(sub, &descriptor, &dacl, &control));
    QVERIFY(dacl != nullptr);
    LocalFree(descriptor);
}

void TestWindowsSecurity::protected_dacl_blocks_inheritance()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString const path = makeFile(dir, QStringLiteral("owner-v1"));
    QVERIFY(!path.isEmpty());

    QVERIFY(WindowsSecurity::protectOwnerAndSystem(path));

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    SECURITY_DESCRIPTOR_CONTROL control{};
    QVERIFY(readDacl(path, &descriptor, &dacl, &control));
    // This is the whole point of PROTECTED_DACL_SECURITY_INFORMATION: without
    // it the object keeps ProgramData's inherited ACEs and stays readable by
    // every account, while protectOwnerAndSystem() still returns true.
    QVERIFY2((control & SE_DACL_PROTECTED) != 0,
             "the DACL is not protected, so inherited permissions still apply");
    LocalFree(descriptor);
}

void TestWindowsSecurity::protect_reports_failure_on_a_missing_path()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString const missing = QDir(dir.path()).filePath(QStringLiteral("no-such-file"));
    QVERIFY(!QFile::exists(missing));

    QVERIFY2(!WindowsSecurity::protectOwnerAndSystem(missing),
             "a path that cannot be secured must not be reported as secured");
}

namespace {
bool setTestSecurity(QString const &path, QString const &sddl)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &descriptor, nullptr))
        return false;
    const QString native = QDir::toNativeSeparators(path);
    const bool ok = SetFileSecurityW(reinterpret_cast<LPCWSTR>(native.utf16()),
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION
            | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    return ok;
}
}

void TestWindowsSecurity::elevated_script_path_policy_data()
{
    QTest::addColumn<QString>("target");
    QTest::addColumn<QString>("security");
    QTest::addColumn<bool>("accepted");
    const QString admin = QStringLiteral("O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)");
    QTest::newRow("protected-tree") << QStringLiteral("parent") << admin << true;
    QTest::newRow("create-siblings-only") << QStringLiteral("parent")
        << admin + "(A;;0x6;;;AU)" << true;
    QTest::newRow("delete-children") << QStringLiteral("parent")
        << admin + "(A;;0x40;;;AU)" << false;
    QTest::newRow("change-parent-dacl") << QStringLiteral("parent")
        << admin + "(A;;WD;;;AU)" << false;
    QTest::newRow("writable-script") << QStringLiteral("script")
        << admin + "(A;;FW;;;AU)" << false;
    QTest::newRow("append-script") << QStringLiteral("script")
        << admin + "(A;;0x4;;;AU)" << false;
    QTest::newRow("ordinary-owner") << QStringLiteral("parent")
        << QStringLiteral("O:%1D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)")
               .arg(WindowsSecurity::currentUserSid()) << false;
    QTest::newRow("null-dacl") << QStringLiteral("script") << QStringLiteral("O:BAD:NO_ACCESS_CONTROL") << false;
}

void TestWindowsSecurity::elevated_script_path_policy()
{
    QFETCH(QString, target);
    QFETCH(QString, security);
    QFETCH(bool, accepted);
    // A fresh tree directly under the volume root avoids user-owned TEMP
    // ancestors. No existing machine directory has its permissions changed.
    QTemporaryDir root(QDir::rootPath() + QStringLiteral("ngpost-acl-test-XXXXXX"));
    QVERIFY(root.isValid());
    const QString admin = QStringLiteral("O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)");
    if (!setTestSecurity(root.path(), admin))
        QSKIP("NTFS ownership fixtures require an elevated Windows test process");
    QVERIFY(QDir().mkdir(root.filePath("parent")));
    QVERIFY(QDir().mkdir(root.filePath("parent/child")));
    const QString script = makeFile(root, QStringLiteral("parent/child/install.ps1"));
    QVERIFY(!script.isEmpty());
    QVERIFY(setTestSecurity(script, admin));
    QVERIFY(setTestSecurity(root.filePath("parent"), admin));
    QVERIFY(setTestSecurity(root.filePath("parent/child"), admin));
    const QString changed = target == "parent" ? root.filePath("parent") : script;
    QVERIFY(setTestSecurity(changed, security));
    QString detail;
    const bool actual = WindowsSecurity::onlyPrivilegedPrincipalsCanWrite(script, &detail);
    QCOMPARE(actual, accepted);
    if (!accepted) QVERIFY(!detail.isEmpty());
}

void TestWindowsSecurity::elevated_script_rejects_junction_ancestors()
{
    QTemporaryDir root(QDir::rootPath() + QStringLiteral("ngpost-junction-test-XXXXXX"));
    QVERIFY(root.isValid());
    const QString admin = QStringLiteral("O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)");
    if (!setTestSecurity(root.path(), admin))
        QSKIP("NTFS ownership fixtures require an elevated Windows test process");
    QVERIFY(QDir().mkdir(root.filePath("target")));
    const QString script = makeFile(root, QStringLiteral("target/install.ps1"));
    QVERIFY(!script.isEmpty());
    // DACL inheritance does not inherit ownership. CI may create children
    // owned by the individual account even when the parent is owned by BA.
    QVERIFY(setTestSecurity(root.filePath("target"), admin));
    QVERIFY(setTestSecurity(script, admin));
    QString detail;
    QVERIFY2(WindowsSecurity::onlyPrivilegedPrincipalsCanWrite(script, &detail),
             qPrintable(detail));
    QProcess cmd;
    cmd.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"),
        QStringLiteral("/J"), QDir::toNativeSeparators(root.filePath("link")),
        QDir::toNativeSeparators(root.filePath("target"))});
    QVERIFY(cmd.waitForFinished());
    QCOMPARE(cmd.exitCode(), 0);
    QVERIFY(!WindowsSecurity::onlyPrivilegedPrincipalsCanWrite(root.filePath("link/install.ps1"), &detail));
    QVERIFY2(detail.contains(QStringLiteral("reparse")), qPrintable(detail));
    QVERIFY(QDir().rmdir(root.filePath("link")));
}

void TestWindowsSecurity::elevated_script_accepts_wof_compression()
{
    QTemporaryDir root(QDir::rootPath() + QStringLiteral("ngpost-wof-test-XXXXXX"));
    QVERIFY(root.isValid());
    const QString admin = QStringLiteral("O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)");
    if (!setTestSecurity(root.path(), admin))
        QSKIP("NTFS ownership fixtures require an elevated Windows test process");
    const QString path = root.filePath(QStringLiteral("install.ps1"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(QByteArray(131072, '#')), qint64(131072));
    file.close();
    // Set the leaf's owner explicitly, independently of the process token's
    // default owner. Compression must be the only changing condition.
    QVERIFY(setTestSecurity(path, admin));
    QString detail;
    QVERIFY2(WindowsSecurity::onlyPrivilegedPrincipalsCanWrite(path, &detail), qPrintable(detail));
    QProcess compact;
    compact.start(QStringLiteral("compact.exe"), {QStringLiteral("/C"),
        QStringLiteral("/EXE:XPRESS4K"), QStringLiteral("/F"), QDir::toNativeSeparators(path)});
    QVERIFY(compact.waitForFinished());
    QCOMPARE(compact.exitCode(), 0);
    const QString native = QDir::toNativeSeparators(path);
    // WOF can hide its tag/attribute through its file-system filter. Confirm
    // actual compression rather than assuming the tag is visible to callers.
    DWORD high = 0;
    DWORD const stored = GetCompressedFileSizeW(reinterpret_cast<LPCWSTR>(native.utf16()), &high);
    QVERIFY(high == 0 && stored > 0 && stored < 131072);
    QVERIFY2(WindowsSecurity::onlyPrivilegedPrincipalsCanWrite(path, &detail), qPrintable(detail));
    // Compression is no exemption from checking the file's actual permissions.
    QVERIFY(setTestSecurity(path, admin + QStringLiteral("(A;;FW;;;AU)")));
    QVERIFY(!WindowsSecurity::onlyPrivilegedPrincipalsCanWrite(path, &detail));
}

#endif // Q_OS_WIN

QTEST_MAIN(TestWindowsSecurity)
#include "tst_WindowsSecurity.moc"
