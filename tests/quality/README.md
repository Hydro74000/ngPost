# Function length and extraction checks

The `function-length` job in `.github/workflows/quality.yml` installs lizard
1.24.0 and its dependencies with pinned versions and wheel hashes. Reproduce it
from the repository root:

```sh
python3 -m venv /tmp/ngpost-function-length
/tmp/ngpost-function-length/bin/pip install --require-hashes --only-binary=:all: \
    -r .github/scripts/function-length-requirements.txt
/tmp/ngpost-function-length/bin/python -m unittest discover -s tests/quality \
    -p test_function_length.py -v
/tmp/ngpost-function-length/bin/python .github/scripts/check-function-length.py
```

The measure includes comments and blank lines, from the signature to the closing
brace. Only Git-tracked C/C++ files under `src/` count. A key consists of the path
and whitespace-normalized lizard signature: overloads remain distinct, moving a
function within its file keeps its allowance, and alternative platform bodies
use their maximum length. Generated, untracked build files are excluded.

Every existing signature has its own ceiling in
[`../function-length-baseline.txt`](../function-length-baseline.txt). A new
signature may have at most 100 physical lines; nothing may exceed 200. Baseline
decreases and removals are allowed. An increase requires an explicit review,
just like a change to a test-count floor. Renaming a function or moving it to
another file makes it new to the gate.

After an intentional reduction, regenerate with `--write-baseline` and review
the diff before committing it. This command refuses any function over 200;
it is not used by CI. The ten tests exercise real lizard parsing and CLI exit
statuses, including 100/101 lines, growth below 100, 201 lines, duplicate
platform definitions, overloads, blank lines, malformed baselines and missing
sources.

## Comparing two application revisions

`compare_behavior.py` is a manual differential check. It needs two application
binaries and two copies of `config_snapshot`, each compiled against its own
source revision. Both applications must have the same Qt version, features and
translation resources. Build the applications normally, then build the probe
in two separate directories, replacing the absolute paths below:

```sh
qmake6 /path/to/current/tests/quality/config_snapshot.pro \
    SOURCE_ROOT=/path/to/before CONFIG+=release
make -j2
```

Repeat in another build directory with `SOURCE_ROOT=/path/to/current`. Generate
the seven `.qm` files in each source tree as for a normal application build if
they are absent. The probe links the real translation resources: localized
`saveConfig()` comments are part of the comparison.

```sh
python3 tests/quality/compare_behavior.py \
    --before-app /path/to/before/ngPost --after-app /path/to/after/ngPost \
    --before-snapshot /path/to/probe-before/config_snapshot \
    --after-snapshot /path/to/probe-after/config_snapshot
```

The script creates disposable configuration/home directories. It compares 58
CLI cases (status, stdout, stderr) and 132 saved configurations with their
diagnostics, including all three shipped configurations and all seven
languages. Only `[HH:MM:SS.mmm]` log prefixes are normalized. Help text and
serialized configurations are compared byte for byte. It does not start a
post or contact a news server. Network posting, GUI behavior and lifetimes are
covered by the regular Qt suites and their ASan/UBSan run.

The probe refuses to run without `NGPOST_TEST_CONFIG_DIR`; normally let the
comparison script provide that disposable directory. Results and the review
scope for lot 5 are recorded in
[`../../docs/code-quality-lot5.md`](../../docs/code-quality-lot5.md).
