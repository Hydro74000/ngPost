<div align="center">
	<img width="80" height="80" src="https://raw.githubusercontent.com/Hydro74000/ngPost/master/src/resources/icons/ngPost.png" alt="ngPost"/>
	<h3 align="center">ngPost - 5.5.1</h3>
	<img alt="Codacy grade" src="https://img.shields.io/codacy/grade/f3f7f3b992d14700affabf1a2eb7ae94?style=for-the-badge">
	<img alt="GitHub Repo stars" src="https://img.shields.io/github/stars/Hydro74000/ngPost?style=for-the-badge">
	<img alt="GitHub top language" src="https://img.shields.io/github/languages/top/Hydro74000/ngPost?style=for-the-badge">
	<img alt="GitHub License" src="https://img.shields.io/github/license/Hydro74000/ngPost?style=for-the-badge&color=%23c20083">
	<img alt="GitHub Issues or Pull Requests" src="https://img.shields.io/github/issues-pr-raw/Hydro74000/ngPost?style=for-the-badge&color=%238575a8">
	<img alt="GitHub Issues or Pull Requests" src="https://img.shields.io/github/issues-raw/Hydro74000/ngPost?style=for-the-badge">

</div>

# About The Project

This application is a high-speed command line and GUI Usenet poster for binaries, designed for secure and efficient data posting. Developed with C++17 and [Qt 6.8.2](https://www.qt.io/blog/qt-6.8.2-released), it features file compression, par2 file generation, and a posting queue for managing multiple uploads. The tool automates tasks by scanning folders and posting files, with options for executing commands post-upload and shutting down the computer upon completion.


![ngPost_v5.6](https://github.com/Hydro74000/ngPost/blob/master/pics/ngPost_v5.6.png?raw=true)

# Getting Started

All the features are [highlighted here](https://github.com/Hydro74000/ngPost/wiki/Features). Some of the prominent ones being:
- Full obfuscation of the post.
- Built in posting queue, allows for the addition of items to the queue whilst the queue is processing.
- Full posting automation.
- Compression using RAR or 7zip.
- Multiple server support.
- Optional VPN tunnel for ngPost NNTP traffic only (OpenVPN/WireGuard, per-server opt-in — Linux & Windows).
- Structured SQLite posting history with resume support, searchable GUI history,
  stats, legacy CSV import and NZB regeneration from history.
- Post info files: a small text file describing each post, written from a model
  you provide, so any Usenet index can be served without ngPost knowing its
  format ([guide](https://github.com/Hydro74000/ngPost/wiki/Post-Info-Files)).
- Multithreading.
- And many more.

[Releases are available](https://github.com/Hydro74000/ngPost/releases) for Linux 64 Bit, Windows 64 Bit and macOS. Raspbian packages are not currently produced.

For building the project yourself, see [Building from source](#building-from-source) or [the wiki](https://github.com/Hydro74000/ngPost/wiki/Build).

## Configuration

ngPost reads `ngPost.conf` from the per-user ngPost configuration folder:

| System | Configuration file |
| --- | --- |
| Windows | `%LOCALAPPDATA%\ngPost\ngPost.conf` |
| Linux | `$XDG_CONFIG_HOME/ngPost/ngPost.conf` (or `~/.config/ngPost/ngPost.conf`) |
| macOS | `~/Library/Application Support/ngPost/ngPost.conf` |

With the GUI, start ngPost, choose your language, add your servers, fill in the
fields and click **Save**.

To edit the file by hand, start from [ngPost.conf.example](ngPost.conf.example)
and set at least:
- `nzbPath`: default destination folder for NZB files
- `inputDir`: default folder of the file selectors (mostly for the GUI; comment it out otherwise)
- `POST_HISTORY`: legacy CSV posting history
- `POST_DB`: structured SQLite history, in the configuration folder by default
- `HISTORY_STORE_PASSWORDS`: `true` by default; stores archive passwords in clear text so NZB files can be regenerated
- `GROUPS`: the newsgroups you post to
- `TMP_DIR`: temporary folder for archives and par2 files
- `RAR_PATH`: full path of the RAR or 7-Zip executable
- `VPN_AUTO_CONNECT`, `VPN_ACTIVE_PROFILE` and a `[vpn_profile]` section if you use the built-in VPN tunnel
- `useVpn` in each `[server]` section that must go through the VPN
- one or more `[server]` sections

Optional, if a Usenet index asks for a record sheet with each post:
- `POST_INFO_TEMPLATE`: your model of the sheet. ngPost knows no index format;
  you give it the model and it fills in the blanks.
- `POST_INFO_OUTPUT`: where to write the sheet, next to the NZB by default.

Ready-to-copy models are in [templates/](templates/), and the beginner's guide is
on the wiki: [Post Info Files](https://github.com/Hydro74000/ngPost/wiki/Post-Info-Files).

## Command Line Usage

```
ngPost (options)* (-i <file or folder> | --auto <folder> | --monitor <folder>)+
```

`ngPost --help` lists every option (`ngPost --help -l fr` in French, and so on);
the [wiki](https://github.com/Hydro74000/ngPost/wiki/Command-Line-Usage) details
them. Started without any argument, ngPost opens its GUI.

There are three ways to post:
- **`--monitor <folder>`**: ngPost keeps running (it can be put in the
  background) and posts every file or folder you copy or move into the
  monitored folders, each as its own post. It requires `--compress`, or
  `--gen_par2` alone when `MONITOR_IGNORE_DIR` is enabled (`--pack` can be used
  too). In the configuration, `MONITOR_EXTENSIONS` limits the extensions posted
  and `MONITOR_IGNORE_DIR` skips folders.
- **`--auto <folder>`**: every file or folder already in the folder is posted
  separately. It requires `--compress` or `--gen_par2` (or `--pack` with
  `COMPRESS` or `GEN_PAR2`); without compression, the folder must not contain
  subfolders. `--auto` and `--monitor` can target the same folder: monitoring
  only posts what arrives afterwards.
- **`-i <file or folder>`** (repeatable): a classic post. Without `--compress`, a
  folder's content is posted without recursion. This is the mode to use when
  your own scripts already made the archives and par2 files and ngPost is only
  the poster.

With `--auto` or `--monitor`, `--rm_posted` deletes each file or folder once it
has been posted. That cannot be undone.

Examples:

```bash
# monitoring
ngPost --monitor /data/folder1 --monitor /data/folder2 --auto_compress --rm_posted --disp_progress files
# auto post
ngPost --auto /data/folder1 --auto /data/folder2 --compress --gen_par2 --gen_name --gen_pass --rar_size 42 --disp_progress files
# compression, file name obfuscation, random password and par2
ngPost -i /tmp/file1 -i /tmp/folder1 -o /nzb/myPost.nzb --compress --gen_name --gen_pass --gen_par2
# another configuration file
ngPost -c /path/to/other.conf -m "password=qwerty42" -f ngPost@nowhere.com -i /tmp/file1 -i /tmp/file2 -i /tmp/folderToPost1
# a single server given on the command line
ngPost -t 1 -m "password=qwerty42" -m "metaKey=someValue" -h news.newshosting.com -P 443 -s -u user -p pass -n 30 -f ngPost@nowhere.com -g "alt.binaries.test,alt.binaries.test2" -a 64000 -i /tmp/folderToPost -o /tmp/folderToPost.nzb
```

Without `-o`, the NZB is written to `nzbPath` and named after the first file or
folder of the command line: `file1.nzb` for the configuration file example.

## Graphical Interface

Once the servers and parameters are filled in, posts live in the tabs below them:
- **Quick Post**: post one or more files.
- **Auto Posting**: generate Quick Posts from a folder, or monitor folders.
- **History**: history, statistics and resume center (see [below](#gui--history-statistics-and-resume-center)).
- **New**: opens another Quick Post tab.

Right-click a tab for **Close All finished Tabs**, or for **Open this tab on
startup** to choose the tab ngPost opens on.

### Quick Post

Add files or folders to the list by:
- clicking **Select Files** or **Select Folder**,
- right-clicking the list, which opens the file selector,
- dragging them from your file manager,
- pasting them (Ctrl+V).

Del or Backspace removes the selected entries, **Remove All** empties the list.
Choose the compression and par2 options, then click **Post Files**.

### Auto Posting

1. Choose the **Auto Dir** (the configuration's `inputDir` by default).
2. Click **Scan**.
3. Remove what you do not want to post: select it in the list and press Del or Backspace.
4. Choose the compression and par2 options.
5. Tick **start all Posts** if the posts should start right away.
6. Click **Generate Posts**.

A Quick Post tab is created for each file or folder, and the GUI switches to the
current one.

### Monitoring

1. Choose the **Auto Dir**.
2. Set the **Monitor extension filter** (for example `mkv,mp4,avi`: comma separated, no dots, no spaces) and whether to **post Folders**.
3. Choose the compression and par2 options.
4. Click **Monitor Folder**.

ngPost then posts every file or folder copied or moved into that folder. Once
monitoring has started, the **+** button next to the Auto Dir adds more folders.
Quick Posts can still be made while monitoring.

## PAR2 tools and GPU support

Packages include ParPar 0.4.6 and par2cmdline 1.4.0; Windows also includes
MultiPar 1.3.3.6. ParPar is optional in the Windows installer. The command-line
component shipped with MultiPar 1.3.3.6 reports version 1.3.3.5.

| Package | GPU generation | Required runtime |
| --- | --- | --- |
| Linux x86_64 archive / AppImage | ParPar via OpenCL | System OpenCL ICD loader and compatible GPU driver |
| Windows x64 | ParPar or MultiPar via OpenCL | Compatible 64-bit OpenCL driver/runtime (`OpenCL.dll`) |
| macOS ARM64 / x86_64 | Unavailable in the bundled ParPar 0.4.6 builds | CPU generation remains available |

par2cmdline uses the CPU on all platforms. A GPU checkbox or a successful CPU
test does not prove GPU availability: use **Find GPUs** with ParPar to check the
selected executable and installed runtime. These tools use OpenCL, not CUDA.
See [runtime details and upstream references](THIRD_PARTY_LICENSES.md#gpu-runtime-dependencies).

Everything above is set in **PAR2 Settings...**, next to **Compression Settings**
in the main window:

![PAR2 Settings](https://github.com/Hydro74000/ngPost/blob/master/pics/ngPost_v5.6_par2.png?raw=true)
![Compression Settings](https://github.com/Hydro74000/ngPost/blob/master/pics/ngPost_v5.6_compression.png?raw=true)

## VPN Tunnel Support

ngPost can route selected NNTP servers through an embedded VPN tunnel without changing the system default route. In the GUI, click **VPN...**:

1. **Setup** (Linux, once): **Install...** installs the privileged helper.
2. **Profiles**: **New...**, choose the **OpenVPN** or **WireGuard** backend, select the configuration file (`.ovpn` or `.conf`) and, for OpenVPN, optional credentials. Then pick the **Active profile**.
3. Tick **Use VPN** on the servers that must use the tunnel, or tick **Route ALL ngPost connections through the VPN (override per-server)** to send every server through it.

When a server is marked **Use VPN**, ngPost binds that server's NNTP sockets to the tunnel IP and refuses to post or check through that server if the VPN is unavailable. Other applications and the normal system routing table are left untouched.

**Linux**: A privileged helper script is installed once via `pkexec`/Polkit, with a Polkit rule limited to the current user and to ngPost's VPN helper; **Connect** and **Disconnect** then no longer ask for a password. The AppImage bundles the VPN runtime assets; source builds install helper resources under `/var/lib/ngpost`.

**Windows**: ngPost uses the **OpenVPN Interactive Service** or the **WireGuard for Windows** installation to bind NNTP sockets to the tunnel interface via `IP_UNICAST_IF`. No helper installation is required. Adding or editing a WireGuard profile registers its tunnel service through an elevated PowerShell script, so Windows asks for administrator approval (UAC).

Only one ngPost VPN tunnel may own the machine-wide VPN resources at a time.
The CLI waits five minutes for that lease by default and reports the owner PID;
the GUI never steals it. On Linux, session ownership is kept under volatile
`/run` storage and ambiguous resources are never removed automatically.
If ngPost reports unattributed VPN resources, first verify that no other
instance (including an older ngPost) is posting. The explicit CLI cleanup is
`ngPost --vpn-cleanup-unattributed --yes`; it can interrupt a live legacy
tunnel. OpenVPN credentials are transferred to the privileged helper over its
stdin pipe and are materialised only in an owner-only `/run` session directory.

The related configuration keys are:

```ini
VPN_AUTO_CONNECT = false
VPN_ACTIVE_PROFILE = My VPN
VPN_LEASE_WAIT_MINUTES = 5
VPN_RECOVERY_MAX_ATTEMPTS = 0

[vpn_profile]
name = My VPN
backend = openvpn
config_file = profile.ovpn
has_auth = false

[server]
useVpn = true
```

`VPN_AUTO_CONNECT` is the master switch behind **Route ALL ngPost connections through the VPN**: every ngPost NNTP connection goes through the tunnel, which starts automatically when a job starts and disconnects after the queue has been empty for a short grace period. It is ignored, with a warning, while no helper is installed or no usable active profile is selected; a server's own **Use VPN** stays enforced either way. `VPN_LEASE_WAIT_MINUTES` is CLI-only (`0..1440`, `0` means fail immediately). `VPN_RECOVERY_MAX_ATTEMPTS` is `0` for unlimited recovery or `1..1000` for a bounded run. During recovery, ngPost suspends new articles and preserves any article without a definitive NNTP reply as `unknown`; a user-paused job is never resumed automatically. The Linux AppImage/release packaging bundles the VPN runtime assets and helper scripts; source builds can install helper resources under `/var/lib/ngpost`.

Containers using the integrated Linux VPN must mount `/run` as tmpfs, for
example `--tmpfs /run:rw,nosuid,nodev,mode=755`. ngPost fails closed before any
network mutation if the runtime is persistent or the lease cannot be created.

## Structured History And Resume

LevelUp adds a SQLite history database used by both the GUI and CLI. By default
it is stored at the application config path as `ngPost_history.sqlite`; set
`POST_DB = /path/to/ngPost_history.sqlite` to move it. Archive passwords are
stored in clear text when enabled so ngPost can regenerate NZB metadata; set
`HISTORY_STORE_PASSWORDS = false` to disable that. Passwords are masked in GUI,
CLI history output and CSV exports unless an explicit password action is used.

Every post, file, article and NNTP attempt is tracked. Articles are marked
`posted` only after a server `240` response. If the connection is lost before
confirmation, the article becomes `unknown`; resume reposts it with a new
Message-ID and the old ID stays as technical history. Final NZB files are
regenerated from the consolidated history so partial posts cannot silently look
complete.

A post can only be resumed from the sources it was made from: for a compressed
post, the temporary archives and par2 files must still exist; for uncompressed
files, path, size and modification time must still match.

### GUI — History, Statistics and Resume Center

The **History** tab gives full access to the posting database from the GUI:

**History sub-tab** (search, filter, detail)
- Search by name, NZB path or archive name.
- Filter by status, password presence, error count, date range and newsgroup.
- Select a row to see full details (files, articles, speed, archive name, NZB path).
- Actions: **Regenerate NZB** (optionally include stored password), **Copy password**,
  **Purge password**, **Open NZB location** (opens file manager), **Delete entry**.
- Export the full history to CSV at any time.

**Stats sub-tab** (timeline, by group, top posts)
- Period filter (last 7/30/90 days, this year, all time) and newsgroup filter.
- *Timeline*: volume (MB) and failed-article count per day as a bar chart.
- *By group*: number of posts per newsgroup as a bar chart.
- *Top posts*: the 20 largest posts by total uploaded size.

**Resume sub-tab** (resume center)
- Lists all posts that can be (fully or partially) resumed.
- Multi-select: apply actions to several posts at once.
- Per-row detail: shows whether the post is fully resumable, partially resumable
  or not resumable, with article counts (posted / pending / failed / unknown).
- Actions: **Resume** (re-send missing articles), **Abandon** (keep history but
  remove from resume list), **Purge resume data** (remove article tracking,
  post history entry kept), **Ignore (session)** (hide from this view until restart).
- A banner at the top of the History tab shows when resumable posts exist and
  links directly to the Resume sub-tab.

### CLI

Useful CLI commands:

```bash
ngPost --history
ngPost --history-show 12
ngPost --history-import-csv /path/to/ngPost_history.csv
ngPost --regenerate-nzb 12 > restored.nzb
ngPost --regenerate-nzb 12 -o restored.nzb --include-password
ngPost --resume-list --json
ngPost --resume-check 12
ngPost --resume-post 12 --dry-run
ngPost --resume-post 12 --yes
ngPost --resume-abandon 12 --yes
ngPost --resume-purge 12 --yes
```

Dash and underscore aliases are both accepted, for example
`--resume-list` and `--resume_list`. Legacy CSV import is explicit and creates
history-only entries: old CSV files do not contain article Message-IDs, so those
entries cannot be resumed or used to regenerate complete NZBs.

## Building from source

Dependencies:
- a C++17 compiler and `make`
- Qt 6 (releases use 6.8.2) with `qmake` and `lrelease`, and the modules core,
  network, sql, gui, widgets, charts and concurrent, plus dbus on Linux
- QtKeychain for Qt 6 (Fedora `qtkeychain-qt6`, Ubuntu `libqt6keychain1-dev`,
  Homebrew `qtkeychain`)
- OpenSSL 3 on Linux, which Qt uses for SSL connections (usually already installed)
- optional, for the Linux VPN tunnel: `openvpn`, `wireguard-tools` and `wireguard-go`

Build outside the source tree:

```bash
mkdir -p build && cd build
qmake6 ../src/ngPost.pro CONFIG+=release   # plain `qmake` with an official Qt install
make
```

That gives the `ngPost` executable, which you can copy anywhere in your `PATH`.
You can also open `src/ngPost.pro` in Qt Creator. Platform specifics are on
[the wiki](https://github.com/Hydro74000/ngPost/wiki/Build).

## Supported languages

ngPost is available in English, Chinese, Dutch, French, German, Portuguese and
Spanish: use the language selector of the GUI, or `-l`/`--lang` on the command
line. The catalogs are Qt Linguist files in [src/lang/](src/lang/); to add a
language, open an issue or a pull request.

## Alternatives

Other Usenet posters are listed on [Nyuu's wiki](https://github.com/animetosho/Nyuu/wiki/Usenet-Uploaders).

## License

ngPost is released under the [GNU General Public License v3](LICENSE).

## ☕ Support the project

ngPost is free, open-source, and ad-free software maintained with passion. If ngPost saves you time or makes your life easier, you can support its development:
- [Buy me a coffee via PayPal](https://paypal.me/ngpost) to encourage future versions and fuel active development.
- Star the repository ⭐ on GitHub to help more people discover it.
- Share your feedback, bug reports, or feature suggestions in the [issues](https://github.com/Hydro74000/ngPost/issues).

### Thanks
- Matthieu Bruel for the base project
- Uukrull for his intensive testing and feedbacks and for building all the MacOS packages.
- awsms for his testing on proper server with a 10Gb/s connection that helped to improve ngPost's upload speed and the multi-threading support
- animetosho for having developped ParPar, the fasted par2 generator ever!
- demanuel for the dev of NewsUP that was my first poster
- noobcoder1983, tensai then yuppie for the German translation
- tiriclote for the Spanish translation
- hunesco for the Portuguese translation
- Peng for the Chinese translation
- All the ngPost users
