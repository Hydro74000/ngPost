#!/usr/bin/env bash
set -u

if [ "$#" -ne 2 ]; then
  echo "usage: $0 <test-root> <suite-name>" >&2
  exit 2
fi

test_root="$1"
suite_name="$2"
fail=0
found=0

while IFS= read -r test_bin; do
  [ -x "$test_bin" ] || continue

  found=$((found + 1))
  test_name="$(basename "$test_bin")"
  test_dir="$(dirname "$test_bin")"
  text_log="${test_dir}/${test_name}.log"
  stdout_log="${test_dir}/${test_name}.stdout.log"

  rm -f "$text_log" "$stdout_log"

  echo "::group::${test_name}"
  code=0
  "$test_bin" -o "$text_log,txt" >"$stdout_log" 2>&1 || code=$?

  if [ -s "$stdout_log" ]; then
    echo "----- ${test_name} stdout/stderr -----"
    cat "$stdout_log"
  fi

  if [ -s "$text_log" ]; then
    echo "----- ${test_name} QtTest log -----"
    cat "$text_log"
  else
    echo "::warning::${test_name} did not produce a QtTest text log"
  fi

  if [ "$code" -ne 0 ]; then
    echo "::error file=${text_log},title=${test_name} failed::${test_name} exited with code ${code}"
    fail=$((fail + 1))
  fi
  echo "::endgroup::"
done < <(find "$test_root" -type f \( -name 'tst_*' -o -name 'tst_*.exe' \) | sort)

if [ "$found" -eq 0 ]; then
  echo "::error::No ${suite_name} test binaries found"
  exit 1
fi

if [ "$fail" -ne 0 ]; then
  echo "::error::${fail} ${suite_name} test binary/binaries failed"
  exit 1
fi
