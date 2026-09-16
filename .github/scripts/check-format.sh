#!/usr/bin/env bash
# check-format.sh -- fail when the C++ lines changed since a base commit do not
# follow .clang-format.
#
# Only changed lines are checked: the tree predates .clang-format and is
# deliberately not reformatted in bulk. To fix what this reports, run
# `git clang-format <base>` with clang-format 18, the version CI pins -- another
# major version formats some constructs differently and the two never agree.
#
# Usage: check-format.sh [<base-commit>]
#
# Compares the working tree with <base-commit>. With no base, an all-zero SHA
# (the first push of a branch) or a commit missing from the clone (a force
# push), only the last commit is checked: the merge base with master would drag
# in every line devel wrote before this file existed.
#
# CLANG_FORMAT and GIT_CLANG_FORMAT override the tool names.

set -euo pipefail

clang_format=${CLANG_FORMAT:-clang-format}
git_clang_format=${GIT_CLANG_FORMAT:-git-clang-format}

# git-clang-format's default list also covers .js, .json, .proto...; the style
# file only describes C++, and clang-format refuses the other languages.
extensions=c,cc,cpp,cxx,h,hh,hpp,hxx

base=${1:-}
if [[ -z $base || $base =~ ^0+$ ]] || ! git cat-file -e "${base}^{commit}" 2>/dev/null; then
    if base=$(git rev-parse --verify -q 'HEAD^'); then
        echo "check-format: no usable base commit; checking the last commit only."
    else
        echo "check-format: no base commit to compare with; nothing checked."
        exit 0
    fi
fi

echo "check-format: $("$clang_format" --version)"
echo "check-format: checking lines changed since $(git rev-parse --short "$base")"

# With --diff, git-clang-format exits 1 when it would change something, 0 when
# not; anything else is a failure of the tool itself.
set +e
diff=$("$git_clang_format" --quiet --binary "$clang_format" \
                           --extensions "$extensions" --diff "$base")
status=$?
set -e

case $status in
0)
    echo "check-format: OK"
    ;;
1)
    printf '%s\n' "$diff"
    message="changed lines do not follow .clang-format; run: git clang-format $base"
    if [[ ${GITHUB_ACTIONS:-} == true ]]; then
        echo "::error title=clang-format::$message"
    else
        echo "check-format: $message" >&2
    fi
    ;;
*)
    printf '%s\n' "$diff" >&2
    echo "check-format: git-clang-format failed (exit $status)" >&2
    ;;
esac
exit "$status"
