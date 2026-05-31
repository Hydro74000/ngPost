# Third-party software licences

ngPost optionally depends on or bundles the following third-party programs.
Each is distributed under its own licence as described below.

---

## OpenVPN Community

- **How used**: bundled inside the Linux AppImage; chain-installed via its official
  MSI on Windows (optional, user-selected at install time)
- **Licence**: GNU General Public Licence, version 2 (with OpenSSL exception)
- **Copyright**: © 2002–2024 OpenVPN Inc. and contributors
- **Source code**: <https://github.com/OpenVPN/openvpn>
- **Full licence text**: <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>

**OpenSSL exception** – In addition, as a special exception, OpenVPN Inc. grants
permission to link the code of this release with the OpenSSL project's "OpenSSL"
library and to distribute the linked executables. All other code must comply with
the GNU GPL v2 in all respects.

---

## WireGuard for Windows

- **How used**: chain-installed via its official MSI on Windows (optional,
  user-selected at install time)
- **Licence**: GNU General Public Licence, version 2 or later
- **Copyright**: © 2015–2024 Jason A. Donenfeld `<Jason@zx2c4.com>`. All Rights Reserved.
- **Source code**: <https://git.zx2c4.com/wireguard-windows>
- **Mirror**: <https://github.com/WireGuard/wireguard-windows>
- **Full licence text**: <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>

---

## wireguard-tools (`wg`)

- **How used**: bundled inside the Linux AppImage
- **Licence**: GNU General Public Licence, version 2 only
- **Copyright**: © 2015–2024 Jason A. Donenfeld `<Jason@zx2c4.com>`. All Rights Reserved.
- **Source code**: <https://git.zx2c4.com/wireguard-tools>
- **Mirror**: <https://github.com/WireGuard/wireguard-tools>
- **Full licence text**: <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>

---

## par2cmdline (`par2` / `par2.exe`)

- **How used**: bundled inside the Linux AppImage (from the Debian/Ubuntu
  `par2` package) and shipped on Windows as `par2.exe` (par2cmdline 0.8.0), the
  always-installed PAR2 fallback used when ParPar is not present.
- **Licence**: GNU General Public Licence, version 2 or later
- **Copyright**: © 2003 Peter Brian Clements; © 2019–2024 par2cmdline contributors
- **Source code**: <https://github.com/Parchive/par2cmdline>
- **Full licence text**: <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>

---

## ParPar (`parpar.exe`)

- **How used**: shipped in the Windows ZIP and offered as an optional installer
  task on Windows. ngPost prefers ParPar over par2cmdline on Windows because
  QProcess invokes CreateProcess directly (no shell), so file-list wildcards
  must be expanded by the par2 binary itself — ParPar's `-R <folder>` flow
  avoids the issue entirely.
- **Licence**: Public Domain / CC0 1.0 Universal
- **Copyright**: released into the public domain by Anime Tosho
- **Source code**: <https://github.com/animetosho/ParPar>
- **Full licence text**: <https://creativecommons.org/publicdomain/zero/1.0/legalcode>

---

## MultiPar (`par2j64.exe`)

- **How used**: shipped on Windows as `par2j64.exe` (MultiPar 1.3.3.6), bundled
  as an alternative PAR2 backend selectable via the `PAR2_PATH` config option.
  ngPost passes par2j its native `/`-prefixed switches (e.g. `/rr` for redundancy).
- **Licence**: GNU General Public Licence
- **Copyright**: © Yutaka Sawada
- **Source code**: <https://github.com/Yutaka-Sawada/MultiPar>
- **Full licence text**: <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>

---

## wireguard-go

- **How used**: bundled inside the Linux AppImage as a fallback userspace WireGuard
  implementation when the kernel module is unavailable
- **Licence**: MIT
- **Copyright**: © 2017–2024 WireGuard LLC. All Rights Reserved.
- **Source code**: <https://git.zx2c4.com/wireguard-go>
- **Mirror**: <https://github.com/WireGuard/wireguard-go>

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## GNU General Public Licence v2 — reference

OpenVPN, WireGuard for Windows, wireguard-tools, par2cmdline, and MultiPar are
distributed under the GNU GPL v2. The full licence text is available at:
<https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>

As required by the GPL, source code for these programs is available from their
respective upstream repositories (links above). ngPost does not modify these
programs; it invokes them as independent system processes.
