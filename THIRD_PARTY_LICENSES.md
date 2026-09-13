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

- **How used**: bundled in every package — Windows (`par2.exe`), the Linux
  archive, the AppImage and the macOS bundle — from the same pinned upstream
  release (par2cmdline 1.4.0), as the always-installed PAR2 fallback used when
  ParPar is not present. The Windows build is MSVC/OpenMP, so Microsoft's
  redistributable `vcomp140.dll` is shipped next to it (see below).
- **Licence**: GNU General Public Licence, version 2 or later
- **Copyright**: © 2003 Peter Brian Clements; © 2019–2024 par2cmdline contributors
- **Source code**: <https://github.com/Parchive/par2cmdline>
- **Full licence text**: <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>

---

## ParPar (`parpar` / `parpar.exe`)

- **How used**: bundled in every package (ParPar 0.4.6) — in the Windows ZIP and
  offered as an optional installer task on Windows, and shipped next to the
  binary in the Linux archive, the AppImage and the macOS bundle. ngPost prefers
  ParPar because QProcess invokes the tool directly (no shell), so file-list
  wildcards must be expanded by the par2 binary itself — ParPar's `-R <folder>`
  flow avoids the question entirely.
- **Licence**: Public Domain / CC0 1.0 Universal
- **Copyright**: released into the public domain by Anime Tosho
- **Source code**: <https://github.com/animetosho/ParPar>
- **Full licence text**: <https://creativecommons.org/publicdomain/zero/1.0/legalcode>

### GPU runtime dependencies

ParPar and MultiPar include their OpenCL processing code in the executable;
the upstream archives contain no separate GPU plugin to copy. These packages
do not include vendor GPU drivers or CUDA modules. par2cmdline uses CPU/OpenMP
and has no GPU backend.

- **Linux x86_64**: ParPar's glibc build loads the system OpenCL ICD loader
  (`libOpenCL.so`, `libOpenCL.so.1` or `libOpenCL.so.1.0.0`). The host needs both
  that loader and an OpenCL implementation for its GPU. The AppImage uses the
  host driver as well.
- **Windows x64**: ParPar and MultiPar load `OpenCL.dll` from the installed
  OpenCL runtime. A compatible 64-bit GPU driver/runtime is required.
  `vcomp140.dll` below is a CPU threading dependency, not a GPU module.
- **macOS ARM64 and x86_64**: the pinned upstream ParPar 0.4.6 executables
  have their dynamic OpenCL loader compiled out. They support CPU generation;
  installing an OpenCL runtime alone will not enable GPU processing in those
  binaries. A different build with a working macOS OpenCL loader would need
  native validation before being shipped. MultiPar is Windows-only.

Upstream references: [ParPar OpenCL requirements](https://github.com/animetosho/ParPar/blob/v0.4.6/README.md#opencl-support),
[ParPar executable build](https://github.com/animetosho/ParPar/blob/v0.4.6/nexe/build.js),
[ParPar loader](https://github.com/animetosho/ParPar/blob/v0.4.6/gf16/opencl-include/cl.c),
[MultiPar loader](https://github.com/Yutaka-Sawada/MultiPar/blob/v1.3.3.6/source/par2j/lib_opencl.c).

---

## Microsoft Visual C++ OpenMP runtime (`vcomp140.dll`)

- **How used**: shipped in the Windows package next to `par2.exe`, which is an
  MSVC/OpenMP build of par2cmdline and does not start without it. It is not
  deployed by `windeployqt --compiler-runtime`, and a machine without the VC++
  redistributable would otherwise fail at the par2 step of a post.
- **Licence**: Microsoft Visual C++ redistributable terms (Distributable Code)
- **Copyright**: © Microsoft Corporation
- **Source of the binary**: the Visual Studio redistributable directory on the
  build machine (`vcomp140.dll`, x64)

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
