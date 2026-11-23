// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// NGPOST_TEST_MAIN(TestObject): QTEST_MAIN, but the test binary first answers
// as the supervisor of a preparation tool, as ngPost does (main.cpp): queue
// runtimes run rar/7z/par2 under QCoreApplication::applicationFilePath()
// --ngpost-tool-supervisor (plan_queue.md, lot C5.6). Dispatched before any
// Qt application exists.
//
// NGPOST_TEST_MAIN_WITH(TestObject, before): the same, then before(argc, argv)
// may answer for the binary (fake tools): any value but -1 is its exit code.
//
// QTEST_MAIN itself is kept, whatever it sets up for this Qt version: it is
// expanded in a namespace, where a function named main is an ordinary one.
//
//========================================================================

#ifndef TESTS_COMMON_TESTMAIN_H
#define TESTS_COMMON_TESTMAIN_H

#include "tools/ToolSupervisor.h"

#include <QtTest>

#include <cstring>

namespace ngpost_test_main
{
inline int nothingBefore(int, char **)
{
    return -1;
}
} // namespace ngpost_test_main

#define NGPOST_TEST_MAIN_WITH(TestObject, Before)                                                  \
    namespace ngpost_test_main                                                                     \
    {                                                                                              \
    QTEST_MAIN(TestObject)                                                                         \
    }                                                                                              \
    int main(int argc, char *argv[])                                                               \
    {                                                                                              \
        if (argc > 1 && std::strcmp(argv[1], toolsupervisor::kSwitch) == 0)                        \
            return toolsupervisor::run(argc, argv);                                                \
        const int answered = Before(argc, argv);                                                   \
        if (answered != -1)                                                                        \
            return answered;                                                                       \
        return ngpost_test_main::main(argc, argv);                                                 \
    }

#define NGPOST_TEST_MAIN(TestObject)                                                               \
    NGPOST_TEST_MAIN_WITH(TestObject, ngpost_test_main::nothingBefore)

#endif // TESTS_COMMON_TESTMAIN_H
