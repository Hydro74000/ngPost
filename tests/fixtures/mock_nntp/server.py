#!/usr/bin/env python3
"""Mock NNTP server for ngPost tests.

Implements a small RFC 3977 subset (MODE READER, AUTHINFO, GROUP, IHAVE,
POST, QUIT, STAT) suitable for end-to-end testing of ngPost posting and
NZB-check flows. Every accepted article is dumped to disk so tests can
verify yEnc bodies, headers and Message-IDs without parsing the wire
protocol themselves.

The server also logs each accepted TCP connection along with its peer
address — this is the hook the Phase 5 VPN E2E uses to confirm the
posting socket actually exited via the tunnel (peer IP == tunnel client
IP) rather than the loopback interface.

Run standalone for manual exploration:
    python3 server.py --port 11119 --dump-dir /tmp/dump --log-file /tmp/m.log

Tests drive it via the C++ MockNntpServer wrapper, which always passes
--port 0 --port-file <tmp> and reads the chosen ephemeral port from disk.
"""

from __future__ import annotations

import argparse
import asyncio
import ssl
import sys
from pathlib import Path
from typing import Optional


def _log(log_path: Optional[Path], msg: str) -> None:
    print(msg, flush=True)
    if log_path is not None:
        with log_path.open("a", encoding="utf-8") as f:
            f.write(msg + "\n")


class Session:
    """Per-connection state machine."""

    def __init__(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
        opts: argparse.Namespace,
        log_path: Optional[Path],
        dump_dir: Optional[Path],
    ) -> None:
        self.reader = reader
        self.writer = writer
        self.opts = opts
        self.log_path = log_path
        self.dump_dir = dump_dir
        self.bytes_read = 0
        self.authed = not opts.require_auth
        self.user: Optional[str] = None

        peer = writer.get_extra_info("peername")
        self.peer = f"{peer[0]}:{peer[1]}" if peer else "?"

    def log(self, msg: str) -> None:
        _log(self.log_path, f"[{self.peer}] {msg}")

    async def write_line(self, line: bytes) -> None:
        if self.opts.slow_mode_ms > 0:
            await asyncio.sleep(self.opts.slow_mode_ms / 1000.0)
        self.writer.write(line)
        if not line.endswith(b"\r\n"):
            self.writer.write(b"\r\n")
        await self.writer.drain()

    async def read_line(self) -> Optional[bytes]:
        # Honor --drop-after-bytes: once N bytes have been read on this
        # connection, the server closes abruptly without an error reply.
        if self.opts.drop_after_bytes > 0 and self.bytes_read >= self.opts.drop_after_bytes:
            self.log(f"closing after {self.bytes_read} bytes (drop-after-bytes)")
            return None
        line = await self.reader.readline()
        if not line:
            return None
        self.bytes_read += len(line)
        return line

    async def read_until_dot(self) -> bytes:
        """Read article body, terminated by a single '.' line per RFC 3977."""
        chunks: list[bytes] = []
        while True:
            line = await self.reader.readline()
            if not line:
                # connection closed mid-post; bail
                return b"".join(chunks)
            self.bytes_read += len(line)
            stripped = line.rstrip(b"\r\n")
            if stripped == b".":
                break
            # Dot-stuffing: a leading '..' represents a literal '.' per RFC.
            if stripped.startswith(b".."):
                stripped = stripped[1:]
            chunks.append(stripped + b"\r\n")
        return b"".join(chunks)

    async def serve(self) -> None:
        self.log(f"connect (require_auth={self.opts.require_auth})")
        await self.write_line(b"200 ngPost mock NNTP server ready (POSTING OK)")

        try:
            while True:
                line = await self.read_line()
                if not line:
                    break
                cmd_line = line.rstrip(b"\r\n").decode("latin1", errors="replace")
                if not cmd_line:
                    continue

                upper = cmd_line.upper()
                if upper == "QUIT":
                    await self.write_line(b"205 closing")
                    break

                if upper == "MODE READER":
                    await self.write_line(b"200 Reader mode, posting allowed")
                    continue

                if upper.startswith("AUTHINFO USER"):
                    self.user = cmd_line[len("AUTHINFO USER ") :].strip()
                    await self.write_line(b"381 password required")
                    continue

                if upper.startswith("AUTHINFO PASS"):
                    pw = cmd_line[len("AUTHINFO PASS ") :].strip()
                    if self.opts.fail_auth:
                        await self.write_line(b"481 auth rejected (test injection)")
                        continue
                    expected_user, expected_pass = "", ""
                    if self.opts.require_auth:
                        expected_user, expected_pass = self.opts.require_auth.split(":", 1)
                    if (not self.opts.require_auth) or (
                        self.user == expected_user and pw == expected_pass
                    ):
                        self.authed = True
                        await self.write_line(b"281 authentication accepted")
                    else:
                        await self.write_line(b"481 authentication rejected")
                    continue

                if upper.startswith("GROUP "):
                    group = cmd_line[len("GROUP ") :].strip()
                    await self.write_line(f"211 0 0 0 {group}".encode("latin1"))
                    continue

                if not self.authed:
                    await self.write_line(b"480 authentication required")
                    continue

                if upper.startswith("IHAVE"):
                    parts = cmd_line.split(None, 1)
                    msgid = parts[1].strip("<>") if len(parts) > 1 else "no-id"
                    await self.write_line(b"335 send the article")
                    body = await self.read_until_dot()
                    self._dump_article(msgid, body)
                    await self.write_line(b"235 article transferred ok")
                    continue

                if upper == "POST":
                    await self.write_line(b"340 send the article; end with <CR-LF>.<CR-LF>")
                    body = await self.read_until_dot()
                    msgid = self._extract_msgid(body)
                    self._dump_article(msgid, body)
                    await self.write_line(b"240 article received ok")
                    continue

                if upper.startswith("STAT"):
                    # Used by --check. Default: report the article exists.
                    parts = cmd_line.split(None, 1)
                    msgid = parts[1].strip("<>") if len(parts) > 1 else "1"
                    if self.opts.stat_missing:
                        await self.write_line(
                            f"430 No article with message-id <{msgid}>".encode("latin1")
                        )
                    else:
                        await self.write_line(
                            f"223 0 <{msgid}> status".encode("latin1")
                        )
                    continue

                await self.write_line(f"500 unknown command: {cmd_line}".encode("latin1"))

        except (BrokenPipeError, ConnectionResetError):
            self.log("client reset connection")
        finally:
            try:
                self.writer.close()
                await self.writer.wait_closed()
            except Exception:
                pass
            self.log(f"disconnect (bytes_read={self.bytes_read})")

    @staticmethod
    def _extract_msgid(body: bytes) -> str:
        # Look for "Message-ID: <foo@bar>" in the headers portion (everything
        # before the first blank line).
        head_end = body.find(b"\r\n\r\n")
        head = body[:head_end] if head_end >= 0 else body[:4096]
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"message-id:"):
                rest = line[len(b"message-id:") :].strip()
                return rest.decode("latin1", errors="replace").strip("<>")
        return f"noid-{abs(hash(body)) & 0xFFFFFF:x}"

    def _dump_article(self, msgid: str, body: bytes) -> None:
        if self.dump_dir is None:
            return
        # Avoid weird characters in filenames.
        safe = "".join(c if c.isalnum() or c in "-_." else "_" for c in msgid)[:200]
        path = self.dump_dir / f"{safe}.eml"
        # If the same Message-ID arrives twice, append rather than overwrite —
        # tests can spot resend / retry behavior that way.
        existed = path.exists()
        mode = "ab" if existed else "wb"
        with path.open(mode) as f:
            if existed:
                f.write(b"\n=== RESEND ===\n")
            f.write(body)
        self.log(f"article dumped: {safe}.eml ({len(body)} bytes)")


async def _run(opts: argparse.Namespace) -> int:
    log_path: Optional[Path] = Path(opts.log_file) if opts.log_file else None
    dump_dir: Optional[Path] = Path(opts.dump_dir) if opts.dump_dir else None
    if dump_dir is not None:
        dump_dir.mkdir(parents=True, exist_ok=True)
    if log_path is not None:
        log_path.parent.mkdir(parents=True, exist_ok=True)

    def make_handler():
        async def handler(reader, writer):
            await Session(reader, writer, opts, log_path, dump_dir).serve()
        return handler

    server = await asyncio.start_server(make_handler(), opts.listen, opts.port)
    sock = server.sockets[0]
    actual_port = sock.getsockname()[1]
    _log(log_path, f"listening on {opts.listen}:{actual_port}")

    ssl_server = None
    ssl_actual_port = 0
    if opts.ssl_port is not None and opts.ssl_cert and opts.ssl_key:
        ssl_ctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
        ssl_ctx.load_cert_chain(opts.ssl_cert, opts.ssl_key)
        ssl_server = await asyncio.start_server(
            make_handler(), opts.listen, opts.ssl_port, ssl=ssl_ctx
        )
        ssl_sock = ssl_server.sockets[0]
        ssl_actual_port = ssl_sock.getsockname()[1]
        _log(log_path, f"listening (TLS) on {opts.listen}:{ssl_actual_port}")

    if opts.port_file:
        Path(opts.port_file).write_text(str(actual_port), encoding="utf-8")
    if opts.ssl_port_file and ssl_actual_port:
        Path(opts.ssl_port_file).write_text(str(ssl_actual_port), encoding="utf-8")

    # Cooperative shutdown: SIGTERM/SIGINT → close server cleanly.
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in ("SIGTERM", "SIGINT"):
        try:
            import signal as _sig
            loop.add_signal_handler(getattr(_sig, sig), stop.set)
        except (NotImplementedError, AttributeError):
            pass

    servers = [server]
    if ssl_server is not None:
        servers.append(ssl_server)

    try:
        done_task = asyncio.create_task(stop.wait())
        serve_tasks = [asyncio.create_task(s.serve_forever()) for s in servers]
        await asyncio.wait([done_task, *serve_tasks], return_when=asyncio.FIRST_COMPLETED)
    finally:
        for s in servers:
            s.close()
            await s.wait_closed()
    return 0


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--listen", default="127.0.0.1",
                   help="Bind address (default 127.0.0.1; use 0.0.0.0 for VPN E2E)")
    p.add_argument("--port", type=int, default=0,
                   help="TCP port; 0 = ephemeral (recommended for tests, paired with --port-file)")
    p.add_argument("--port-file", default=None,
                   help="If set, the chosen port number is written to this path once listening")
    p.add_argument("--log-file", default=None,
                   help="Append connection events here (default stdout only)")
    p.add_argument("--dump-dir", default=None,
                   help="Write each received article to <dir>/<msgid>.eml")
    p.add_argument("--require-auth", default="",
                   help="If set to 'user:pass', reject AUTHINFO PASS that does not match")
    p.add_argument("--fail-auth", action="store_true",
                   help="Always reject AUTHINFO PASS with 481")
    p.add_argument("--drop-after-bytes", type=int, default=0,
                   help="Close the connection after N bytes have been received (0 = never)")
    p.add_argument("--slow-mode-ms", type=int, default=0,
                   help="Sleep N ms before each server reply (default 0)")
    p.add_argument("--stat-missing", action="store_true",
                   help="Reply 430 to STAT (simulates missing articles for --check)")
    p.add_argument("--ssl-port", type=int, default=None,
                   help="If set, also listen on this TCP port wrapped in TLS")
    p.add_argument("--ssl-port-file", default=None,
                   help="Write the chosen SSL port number to this path")
    p.add_argument("--ssl-cert", default=None,
                   help="Path to the TLS certificate (PEM)")
    p.add_argument("--ssl-key", default=None,
                   help="Path to the TLS private key (PEM)")
    opts = p.parse_args(argv)

    return asyncio.run(_run(opts))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
