#!/usr/bin/env python3
"""Mock TCP DNS server for ngPost's VpnDnsResolver tests.

ngPost resolves NNTP hostnames over DNS-over-TCP so the query exits via
the VPN tunnel (see src/vpn/VpnDnsResolver.cpp). This mock serves
just enough of RFC 1035 / 7766 to answer A queries with a configurable
fixed-IP map.

Wire format (per RFC 1035 section 4.2.2): every message on the TCP
connection is preceded by a 2-byte big-endian length field.

Run standalone:
    python3 tcp_dns.py --port 0 --port-file /tmp/p \\
        --answer example.test=10.42.0.99 --answer other.invalid=192.0.2.7

Tests drive this via QProcess and pass --port 0 --port-file <tmp>, then
read the chosen ephemeral port from disk.
"""

from __future__ import annotations

import argparse
import asyncio
import struct
import sys
from pathlib import Path
from typing import Dict, Optional


def _log(log_path: Optional[Path], msg: str) -> None:
    print(msg, flush=True)
    if log_path is not None:
        with log_path.open("a", encoding="utf-8") as f:
            f.write(msg + "\n")


def _parse_qname(buf: bytes, off: int) -> tuple[str, int]:
    labels = []
    while True:
        if off >= len(buf):
            raise ValueError("qname overruns packet")
        n = buf[off]
        if n == 0:
            return ".".join(labels), off + 1
        if n & 0xc0:
            # message compression — not used in queries, but fail safe.
            raise ValueError("compression in query qname is unexpected")
        off += 1
        labels.append(buf[off : off + n].decode("ascii", errors="strict"))
        off += n


def _build_answer(query: bytes, ipv4: str) -> bytes:
    if len(query) < 12:
        return b""
    qid = struct.unpack(">H", query[:2])[0]
    flags = struct.unpack(">H", query[2:4])[0]
    qd = struct.unpack(">H", query[4:6])[0]
    end_qname = _parse_qname(query, 12)[1]
    qtype, qclass = struct.unpack(">HH", query[end_qname : end_qname + 4])

    # Build a minimal response: QR=1, RD copied, RA=0, AA=1, RCODE=0.
    rflags = 0x8400 | (flags & 0x0100)
    header = struct.pack(">HHHHHH", qid, rflags, qd, 1, 0, 0)
    question = query[12 : end_qname + 4]
    if qtype != 1 or qclass != 1:
        # Not an A IN query: NXDOMAIN-ish for simplicity (RCODE 3).
        header = struct.pack(">HHHHHH", qid, 0x8403, qd, 0, 0, 0)
        return header + question

    # Answer: name (compression pointer 0xC00C → start of question name), type
    # A, class IN, TTL 30, rdlength 4, rdata = ipv4.
    rdata = bytes(int(o) for o in ipv4.split("."))
    answer = struct.pack(">HHHIH", 0xC00C, 1, 1, 30, 4) + rdata
    return header + question + answer


class DnsSession:
    def __init__(self, reader, writer, answers, log_path, truncate_after):
        self.reader = reader
        self.writer = writer
        self.answers = answers
        self.log_path = log_path
        self.truncate_after = truncate_after
        peer = writer.get_extra_info("peername")
        self.peer = f"{peer[0]}:{peer[1]}" if peer else "?"

    def log(self, msg: str) -> None:
        _log(self.log_path, f"[{self.peer}] {msg}")

    async def serve(self) -> None:
        self.log("connect")
        try:
            while True:
                hdr = await self.reader.readexactly(2)
                if not hdr:
                    break
                msg_len = struct.unpack(">H", hdr)[0]
                query = await self.reader.readexactly(msg_len)
                try:
                    qname, _ = _parse_qname(query, 12)
                except Exception as exc:
                    self.log(f"malformed query: {exc}")
                    break
                ip = self.answers.get(qname.lower())
                if ip is None:
                    self.log(f"NXDOMAIN for {qname}")
                    response = _build_answer(query, "0.0.0.0")
                    # Force NXDOMAIN flags
                    response = response[:2] + struct.pack(">H", 0x8403) + response[4:12] + response[12:]
                else:
                    self.log(f"answer {qname} = {ip}")
                    response = _build_answer(query, ip)

                length = struct.pack(">H", len(response))
                payload = length + response
                if self.truncate_after > 0:
                    payload = payload[: self.truncate_after]
                    self.log(f"truncating to {self.truncate_after} bytes (--truncate-after)")
                self.writer.write(payload)
                await self.writer.drain()
                if self.truncate_after > 0:
                    # After delivering a partial reply, close to simulate a
                    # peer that crashed mid-response.
                    break
        except asyncio.IncompleteReadError:
            pass
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            try:
                self.writer.close()
                await self.writer.wait_closed()
            except Exception:
                pass
            self.log("disconnect")


async def _run(opts: argparse.Namespace) -> int:
    log_path: Optional[Path] = Path(opts.log_file) if opts.log_file else None
    if log_path is not None:
        log_path.parent.mkdir(parents=True, exist_ok=True)

    answers: Dict[str, str] = {}
    for entry in opts.answer:
        if "=" not in entry:
            print(f"--answer must be name=ip, got {entry!r}", file=sys.stderr)
            return 2
        name, ip = entry.split("=", 1)
        answers[name.strip().lower()] = ip.strip()

    def make_handler():
        async def handler(reader, writer):
            await DnsSession(
                reader, writer, answers, log_path, opts.truncate_after
            ).serve()
        return handler

    server = await asyncio.start_server(make_handler(), opts.listen, opts.port)
    sock = server.sockets[0]
    actual_port = sock.getsockname()[1]
    _log(log_path, f"listening on {opts.listen}:{actual_port}")

    if opts.port_file:
        Path(opts.port_file).write_text(str(actual_port), encoding="utf-8")

    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in ("SIGTERM", "SIGINT"):
        try:
            import signal as _sig
            loop.add_signal_handler(getattr(_sig, sig), stop.set)
        except (NotImplementedError, AttributeError):
            pass

    async with server:
        done_task = asyncio.create_task(stop.wait())
        serve_task = asyncio.create_task(server.serve_forever())
        await asyncio.wait([done_task, serve_task], return_when=asyncio.FIRST_COMPLETED)
        if stop.is_set():
            server.close()
            await server.wait_closed()
    return 0


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--listen", default="127.0.0.1")
    p.add_argument("--port", type=int, default=0)
    p.add_argument("--port-file", default=None)
    p.add_argument("--log-file", default=None)
    p.add_argument("--answer", action="append", default=[],
                   help="name=ip pair (may be repeated)")
    p.add_argument("--truncate-after", type=int, default=0,
                   help="If >0, send only the first N bytes of the reply and "
                        "drop the connection. Tests use this to exercise the "
                        "resolver's truncation path.")
    opts = p.parse_args(argv)
    return asyncio.run(_run(opts))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
