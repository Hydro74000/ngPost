#!/bin/bash
#
# Sync the working tree into the Qt test container, sources only.
#
# `docker cp src/. <container>:/workspace/src/` copies object files, the
# qmake-generated Makefile and the built binaries along with the sources. The
# container then links a mixture of host- and container-built objects compiled
# against different revisions of the headers, and what comes out crashes in
# ways that look exactly like code defects: one session produced 31 SIGSEGV and
# SIGABRT cores on commands as trivial as `--history`, plus a std::bad_alloc
# traced to a stale NgPost layout in main.o.
#
# Worse, the crashes are silent: QProcess reports the signal number as the exit
# code on Unix, and SIGSEGV is 11 -- the value of ERROR_CODE::ERR_ARTICLE_SIZE.
# A segfaulting binary made the CLI suite report success. The test harnesses now
# detect QProcess::CrashExit, but the build tree has to stay clean too.
#
# Only tracked files are copied, and the container's build artefacts are removed
# first so nothing survives from a previous revision.
#
# Usage: tests/sync-to-container.sh [container]   (default: ngpost-qt6-test)

set -euo pipefail

CONTAINER="${1:-ngpost-qt6-test}"
WORKSPACE=/workspace

cd "$(git rev-parse --show-toplevel)"

if ! docker inspect "$CONTAINER" >/dev/null 2>&1; then
    echo "container '$CONTAINER' not found" >&2
    exit 1
fi

echo "Removing build artefacts from $CONTAINER:$WORKSPACE"
docker exec "$CONTAINER" bash -c '
    set -e
    find '"$WORKSPACE"' \( -name "*.o" -o -name "*.so" -o -name "moc_*.cpp" \
                        -o -name "qrc_*.cpp" -o -name "ui_*.h" -o -name "Makefile" \
                        -o -name ".qmake.stash" \) -delete 2>/dev/null || true
    rm -f '"$WORKSPACE"'/src/ngPost
    find '"$WORKSPACE"'/tests -type f -executable -name "tst_*" -delete 2>/dev/null || true
'

echo "Copying tracked files into $CONTAINER:$WORKSPACE"
git ls-files -z | tar --null --files-from=- -cf - | docker exec -i "$CONTAINER" tar -xf - -C "$WORKSPACE"

echo "Done. Build inside the container, never copy binaries into it:"
echo "  docker exec $CONTAINER bash -lc 'cd $WORKSPACE/src && qmake6 ngPost.pro && make -j\$(nproc)'"
