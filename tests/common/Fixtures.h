//========================================================================
//
// tests/common/Fixtures.h — paths to test fixture files.
//
//========================================================================

#ifndef TESTS_FIXTURES_H
#define TESTS_FIXTURES_H

#include <QString>

namespace ngpost::tests
{

//! Absolute path to <repo>/tests/fixtures/. NGPOST_TESTS_ROOT is injected at
//! compile time by tests/common/common.pri (`-DNGPOST_TESTS_ROOT="..."`) so
//! the result is the same regardless of the test's working directory.
inline QString fixturesDir()
{
    return QString::fromLatin1(NGPOST_TESTS_ROOT) + QStringLiteral("/fixtures");
}

} // namespace ngpost::tests

#endif // TESTS_FIXTURES_H
