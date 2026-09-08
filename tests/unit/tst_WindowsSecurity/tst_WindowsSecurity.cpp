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
// On non-Windows the source is entirely #ifdef'd out, so the suite is a single
// QSKIP. It stays enrolled everywhere, like tst_WindowsBindHelper, so an
// accidental Linux/macOS break still surfaces.
//
//========================================================================

#include <QtTest>

#ifdef Q_OS_WIN
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "vpn/WindowsSecurity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#endif

class TestWindowsSecurity : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

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
#endif
};

void TestWindowsSecurity::initTestCase()
{
#ifndef Q_OS_WIN
    QSKIP("WindowsSecurity is Windows-only (SDDL / SetNamedSecurityInfoW). "
          "On Linux/macOS the same role is filled by 0600 file modes, "
          "exercised by tst_VpnProfile and tst_PostHistory.");
#endif
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

#endif // Q_OS_WIN

QTEST_MAIN(TestWindowsSecurity)
#include "tst_WindowsSecurity.moc"
