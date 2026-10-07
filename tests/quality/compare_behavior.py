#!/usr/bin/env python3
"""Compare pre/post extraction binaries in isolated homes, byte for byte.

Build config_snapshot.pro against each source tree first. This manual audit
needs both revisions; the normal Qt suites remain the ongoing regression gate.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def run(binary, arguments, env, cwd):
    result = subprocess.run(
        ["ngPost", *map(str, arguments)], executable=str(binary), env=env,
        cwd=cwd, capture_output=True, timeout=30,
    )
    # Only the log clock varies between sequential invocations. Help and
    # serialized configuration have no such prefix and are compared verbatim.
    def without_clock(data):
        return re.sub(rb"(?m)^\[\d{2}:\d{2}:\d{2}\.\d{3}\] ", b"[TIME] ", data)
    return result.returncode, without_clock(result.stdout), without_clock(result.stderr)


def cli_cases():
    for language in ("en", "fr", "de", "es", "nl", "pt", "zh"):
        yield f"help-{language}", ["--lang", language, "--help"]
    yield "version", ["--version"]
    yield "unknown", ["--nonexistent-lot5-option"]
    yield "vpn-conflict", ["--vpn", "--no_vpn"]
    yield "vpn-profile-missing", ["--vpn_profile", "absent"]
    for option in ("thread", "article_size", "retry", "rar_max", "post_cmd_timeout",
                   "nzb_upload_timeout", "port", "connection", "length_name", "length_pass"):
        for value in ("0", "-1", "invalid", "2147483648"):
            yield f"{option}-{value}", ["--history", "--json", "-h", "127.0.0.1",
                                        "--" + option, value]
    yield "server-list", ["--history", "--json", "-S", "127.0.0.1:119:1:nossl"]
    yield "server-and-host", ["--history", "--json", "-S", "127.0.0.1:119:1:nossl",
                              "-h", "127.0.0.2"]
    yield "server-invalid", ["--history", "-S", "invalid"]
    yield "metadata-equals", ["--history", "--json", "-m", "name=a=b"]
    yield "metadata-conflict", ["--history", "-m", "name=public", "--post-meta", "name=private"]
    yield "post-info-order", ["--history", "--json", "--post-info-only-on-success",
                              "--no-post-info-only-on-success"]
    yield "post-cmd-order", ["--history", "--json", "--post-cmd-fail-is-error",
                             "--no-post-cmd-fail-is-error"]


def config_cases(root):
    for name in ("ngPost.conf.example", "ngPost_fr.conf", "ngPost.docker.conf"):
        yield name, (root / name).read_bytes()
    for language in ("en", "fr", "de", "es", "nl", "pt", "zh"):
        yield f"language-{language}", f"FROM = test@example.invalid\nLANG = {language}\n".encode()
    # Boundary/invalid values across each extracted domain. Each process starts
    # fresh: static defaults and previous parse state cannot mask a difference.
    keys = ("THREAD ARTICLE_SIZE RETRY RAR_MAX RAR_SIZE PAR2_PCT TMP_RAM_RATIO "
            "SOCK_TIMEOUT RESUME_WAIT PREPARE_PACKING LENGTH_NAME LENGTH_PASS "
            "POST_CMD_TIMEOUT NZB_UPLOAD_TIMEOUT VPN_LEASE_WAIT_MINUTES "
            "VPN_RECOVERY_MAX_ATTEMPTS NZB_RM_ACCENTS KEEP_NFO_EXTENSION "
            "NZB_COPY_NFO AUTO_INCLUDE_NFO RAR_NO_ROOT_FOLDER KEEP_RAR "
            "HISTORY_STORE_PASSWORDS POST_INFO_ONLY_ON_SUCCESS POST_CMD_FAIL_IS_ERROR "
            "POST_CMD_EXPOSE_PASSWORD NO_RESUME_AUTO MONITOR_IGNORE_DIR "
            "MONITOR_SEC_DELAY_SCAN FIELD_SEPARATOR").split()
    for key in keys:
        for value in ("0", "-1", "true", "invalid"):
            yield f"{key}-{value}", f"FROM = test@example.invalid\n{key} = {value}\n".encode()
    yield "contexts-and-repeated-keys", (
        b"FROM = test@example.invalid\nTHREAD = 2\nTHREAD = 4\n"
        b"OBFUSCATE = article,filename\nGROUPS = alt.test,alt.other\n"
        b"NZB_POST_CMD = echo first\nNZB_POST_CMD = echo second\n"
        b"[server]\nhost = first.example.invalid\nport = 563\nssl = true\n"
        b"user = test-user\npass = a=b=c\nconnection = 3\nenabled = false\n"
        b"[vpn_profile]\nname = test\nbackend = openvpn\nconfig_file = test.ovpn\n"
        b"has_auth = true\n[server]\nhost = second.example.invalid\nport = 119\n"
        b"ssl = false\nenabled = false\nnzbCheck = true\nuseVpn = false\n"
    )
    yield "legacy-server", b"HOST = legacy.example.invalid\nUSER = legacy\nPASS = a=b\n"


def compare(args, work):
    env = dict(os.environ, HOME=str(work), XDG_CONFIG_HOME=str(work / "xdg"),
               APPDATA=str(work), LOCALAPPDATA=str(work), USERPROFILE=str(work),
               NGPOST_TEST_HOME=str(work), NGPOST_TEST_CONFIG_DIR=str(work / "config"),
               QT_QPA_PLATFORM="offscreen", LC_ALL="C.UTF-8")
    config_dir = work / "config"
    fixture = work / "fixture.conf"
    snapshot = work / "saved.conf"
    counts = {"CLI": 0, "config": 0}
    cases = [("CLI", name, options) for name, options in cli_cases()]
    cases += [("config", name, contents) for name, contents in config_cases(args.root)]
    for kind, name, payload in cases:
        observed = []
        for side in ("before", "after"):
            shutil.rmtree(config_dir, ignore_errors=True)
            shutil.rmtree(work / "xdg", ignore_errors=True)
            config_dir.mkdir()
            if kind == "CLI":
                fixture.write_bytes(b"FROM = test@example.invalid\n[server]\n"
                                    b"host = localhost\nenabled = false\n")
                observed.append(run(getattr(args, side + "_app"),
                                    ["-c", fixture, *payload], env, work))
            else:
                fixture.write_bytes(payload)
                result = run(getattr(args, side + "_snapshot"), [fixture, snapshot], env, work)
                if result[0] != 0:
                    raise RuntimeError(f"{side} {name}: probe failed: {result!r}")
                observed.append((result, snapshot.read_bytes(),
                                 Path(str(snapshot) + ".errors").read_bytes()))
        if observed[0] != observed[1]:
            raise AssertionError(f"{kind} {name}: before/after differ:\n{observed!r}")
        counts[kind] += 1
    print(f"Identical: {counts['CLI']} CLI exit/stdout/stderr cases, "
          f"{counts['config']} saved configurations and parse diagnostics")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    for name in ("before-app", "after-app", "before-snapshot", "after-snapshot"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    for name in ("before_app", "after_app", "before_snapshot", "after_snapshot"):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="ngpost-compare-") as directory:
        compare(args, Path(directory))


if __name__ == "__main__":
    main()
