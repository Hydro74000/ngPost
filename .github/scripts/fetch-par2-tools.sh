#!/usr/bin/env bash
#
# Fetch the ParPar and par2cmdline builds pinned in the release workflow's env
# block into a destination directory, verifying every download against its
# SHA-256.
#
# One script for the Linux archive, the AppImage and the macOS bundle: the three
# used to disagree (Windows shipped a vendored par2cmdline, the AppImage took
# whatever apt had, macOS shipped nothing at all), so a post behaved differently
# depending on the package it came from. Windows fetches the same pinned
# versions from the workflow itself -- PowerShell, and MultiPar on top.
#
# ngPost finds these by name next to its own binary (see par2Candidates in
# NgPost::NgPost), which is why they are written as plain "parpar" and "par2".
#
# Usage: fetch-par2-tools.sh <destination-directory>
set -euo pipefail

dest=${1:?usage: fetch-par2-tools.sh <destination-directory>}

# Versions and checksums live next to this script so the release packages and
# the test jobs cannot drift apart.
pins="$(dirname "$0")/par2-tools.env"
[ -f "$pins" ] || { echo "ERROR: missing $pins" >&2; exit 1; }
# shellcheck disable=SC1090
. "$pins"

# Under GitHub Actions, publish them to the rest of the job: the verification
# steps that check what was actually bundled read these, and a job that had to
# remember to load the file itself would eventually forget.
if [ -n "${GITHUB_ENV:-}" ] && [ -f "${GITHUB_ENV}" ]; then
  grep -E '^[A-Z0-9_]+=' "$pins" >> "$GITHUB_ENV"
fi

: "${PARPAR_VERSION:?missing PARPAR_VERSION}"
: "${PAR2CMDLINE_VERSION:?missing PAR2CMDLINE_VERSION}"

mkdir -p "$dest"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

verify() { # <file> <expected-sha256>
  local got
  if command -v sha256sum >/dev/null 2>&1; then
    got=$(sha256sum "$1" | cut -d' ' -f1)
  else
    got=$(shasum -a 256 "$1" | cut -d' ' -f1)
  fi
  if [ "$got" != "$2" ]; then
    echo "ERROR: $(basename "$1") SHA-256 mismatch: got $got, expected $2" >&2
    exit 1
  fi
}

case "$(uname -s)" in
  Darwin)
    # Follow the runner: an arm64 package must not ship an x86_64 helper.
    if [ "$(uname -m)" = "arm64" ]; then
      parpar_asset="parpar-v${PARPAR_VERSION}-macos-arm64.xz"
      parpar_sha=${PARPAR_SHA256_MACOS_ARM64:?missing PARPAR_SHA256_MACOS_ARM64}
    else
      parpar_asset="parpar-v${PARPAR_VERSION}-macos-x64.xz"
      parpar_sha=${PARPAR_SHA256_MACOS_X64:?missing PARPAR_SHA256_MACOS_X64}
    fi
    par2_asset="par2cmdline-${PAR2CMDLINE_VERSION}-macos-universal.zip"
    par2_sha=${PAR2CMDLINE_SHA256_MACOS_UNIVERSAL:?missing PAR2CMDLINE_SHA256_MACOS_UNIVERSAL}
    ;;
  Linux)
    # ParPar's fully static build cannot load OpenCL, even with working GPU
    # drivers. The glibc 2.31 build supports runtime loading of the host's ICD.
    # par2cmdline remains static; do not pass it to linuxdeploy/patchelf.
    parpar_asset="parpar-v${PARPAR_VERSION}-linux-glibc2.31-amd64.xz"
    parpar_sha=${PARPAR_SHA256_LINUX_AMD64:?missing PARPAR_SHA256_LINUX_AMD64}
    par2_asset="par2cmdline-${PAR2CMDLINE_VERSION}-linux-amd64.zip"
    par2_sha=${PAR2CMDLINE_SHA256_LINUX_AMD64:?missing PAR2CMDLINE_SHA256_LINUX_AMD64}
    ;;
  *)
    echo "ERROR: unsupported platform $(uname -s)" >&2
    exit 1
    ;;
esac

curl -fsSL -o "$tmp/$parpar_asset" \
  "https://github.com/animetosho/ParPar/releases/download/v${PARPAR_VERSION}/${parpar_asset}"
verify "$tmp/$parpar_asset" "$parpar_sha"
# The ParPar assets are .xz; macOS runners do not guarantee the xz CLI, and
# python3 is present on every runner image.
if command -v xz >/dev/null 2>&1; then
  xz -dc "$tmp/$parpar_asset" > "$dest/parpar"
else
  python3 -c 'import lzma,sys; sys.stdout.buffer.write(lzma.open(sys.argv[1]).read())' \
    "$tmp/$parpar_asset" > "$dest/parpar"
fi

curl -fsSL -o "$tmp/$par2_asset" \
  "https://github.com/Parchive/par2cmdline/releases/download/v${PAR2CMDLINE_VERSION}/${par2_asset}"
verify "$tmp/$par2_asset" "$par2_sha"
unzip -o -q -j "$tmp/$par2_asset" par2 -d "$dest"

chmod 755 "$dest/parpar" "$dest/par2"

if [ "$(uname -s)" = "Darwin" ]; then
  # arm64 macOS refuses to execute an unsigned Mach-O, and these come straight
  # out of a release archive. Ad-hoc signing is what lets them run at all.
  codesign --force --sign - "$dest/parpar"
  codesign --force --sign - "$dest/par2"
fi

# Being on disk proves nothing: run both the way ngPost will and check what they
# report, so a truncated download, a wrong asset or a missing runtime fails here
# and not at someone's first post. ParPar prints --version on stderr.
"$dest/parpar" --version 2>&1 | grep -qx "${PARPAR_VERSION}" \
  || { echo "ERROR: bundled ParPar does not report ${PARPAR_VERSION}" >&2; exit 1; }
# A fully static ParPar has no INTERP segment -- and no working OpenCL. Asking
# it to list devices would not tell the two apart: with no ICD installed, which
# is the case on every CI runner, both builds answer "Could not load OpenCL
# runtime". The linkage does tell them apart, on any machine.
if [ "$(uname -s)" = "Linux" ] && command -v readelf >/dev/null 2>&1; then
  readelf -l "$dest/parpar" | grep -q INTERP \
    || { echo "ERROR: bundled ParPar is fully static; OpenCL/GPU would be unavailable" >&2; exit 1; }
fi
"$dest/par2" -V | grep -q "par2cmdline version ${PAR2CMDLINE_VERSION}" \
  || { echo "ERROR: bundled par2cmdline does not report ${PAR2CMDLINE_VERSION}" >&2; exit 1; }
echo "Bundled ParPar ${PARPAR_VERSION} and par2cmdline ${PAR2CMDLINE_VERSION} into $dest"
