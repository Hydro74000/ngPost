//========================================================================
//
// tests/common/TestEnv.h — sandboxed HOME, port allocation, and a
// narrow #ifdef NGPOST_TESTING accessor to NgPost private members.
//
//========================================================================

#ifndef TESTS_TESTENV_H
#define TESTS_TESTENV_H

#include <QString>
#include <QTemporaryDir>

class NgPost;

namespace ngpost::tests
{

//! RAII sandbox: redirects HOME, XDG_CONFIG_HOME, APPDATA, USERPROFILE to a
//! temporary directory and restores the previous values on destruction.
//!
//! Use this in every test that touches PathHelper or constructs NgPost so the
//! user's real ~/.config/ngPost is never read or written.
class HomeSandbox
{
public:
    HomeSandbox();
    ~HomeSandbox();

    //! Absolute path to the sandbox root directory.
    QString rootPath() const;

    //! Convenience: <root>/.config (Linux XDG_CONFIG_HOME).
    QString xdgConfigHome() const;

private:
    QTemporaryDir _root;
    QString       _prevHome;
    QString       _prevXdg;
    QString       _prevAppData;
    QString       _prevUserProfile;
    QString       _prevTestHome;
    QString       _prevTestConfigDir;
    bool          _hadHome;
    bool          _hadXdg;
    bool          _hadAppData;
    bool          _hadUserProfile;
    bool          _hadTestHome;
    bool          _hadTestConfigDir;
};

//! Allocate a free TCP port on 127.0.0.1 for a test. Returns 0 on failure.
quint16 allocLoopbackPort();

//! Locate the built ngPost executable. Honors $NGPOST_BIN, otherwise falls
//! back to <repo>/src/ngPost (or src/ngPost.app/Contents/MacOS/ngPost on
//! macOS). Returns an empty string if the binary cannot be found.
QString locateNgPostBinary();

} // namespace ngpost::tests

#endif // TESTS_TESTENV_H
