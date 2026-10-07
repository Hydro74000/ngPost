#!/usr/bin/env bash
# static-analysis.sh -- clang-tidy and cppcheck over one or more builds.
#
#   [JOBS=n] static-analysis.sh <build-dir>...
#
# Each build directory must hold the compile_commands.json of a build of
# src/ngPost.pro (`bear -- make`). quality.yml runs it on the GUI and on the
# headless build, whose #ifdef branches differ. Rules: .clang-tidy, and the
# cppcheck options below. Any finding fails the run.
set -euo pipefail

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <build-dir>..." >&2
    exit 2
fi

repo=$(cd "$(dirname "$0")/../.." && pwd)
src="$repo/src"
# JOBS caps the parallelism: clang-tidy holds a full AST per job.
jobs=${JOBS:-$(nproc)}
status=0

for build in "$@"; do
    build=$(cd "$build" && pwd)
    db="$build/compile_commands.json"
    if [ ! -s "$db" ]; then
        echo "no compile_commands.json in $build" >&2
        exit 2
    fi

    echo "::group::clang-tidy -- $build"
    # Our sources only: moc_* and qrc_* are generated. -quiet keeps the log to
    # the findings; any warning is an error (.clang-tidy lists the rules).
    if ! (cd "$repo" && run-clang-tidy -p "$build" -j "$jobs" -quiet \
            -warnings-as-errors='*' "$src/(?!.*(moc_|qrc_)).*\\.cpp\$"); then
        status=1
    fi
    echo "::endgroup::"

    echo "::group::cppcheck -- $build"
    python3 "$repo/.github/scripts/cppcheck-compile-db.py" "$db" "$build/cppcheck.json" --root "$src"
    # --library=qt stands in for the Qt headers the database no longer lists.
    # QT_CHARTS_USE_NAMESPACE is QtCharts', which that description lacks.
    # returnByReference: a QString member returned by value is one atomic
    # increment, and the copy stays valid while another thread changes the
    # object -- NntpFile getters are read from the upload threads.
    if ! cppcheck --project="$build/cppcheck.json" --library=qt \
            -DQT_CHARTS_USE_NAMESPACE= \
            --enable=warning,performance,portability --inline-suppr \
            --suppress=missingInclude --suppress=missingIncludeSystem \
            --suppress=returnByReference \
            --error-exitcode=1 -j "$jobs" --quiet \
            --template='{file}:{line}: {severity}: {message} [{id}]'; then
        status=1
    fi
    echo "::endgroup::"
done

exit "$status"
