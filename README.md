<div align="center">
	<img width="80" height="80" src="https://raw.githubusercontent.com/Hydro74000/ngPost/master/src/resources/icons/ngPost.png" alt="ngPost"/>
	<h3 align="center">ngPost - 5.3.0</h3>
	<img alt="Codacy grade" src="https://img.shields.io/codacy/grade/e790647f2eae44898d760b68ee6f5b78?style=for-the-badge">
	<img alt="GitHub Repo stars" src="https://img.shields.io/github/stars/Hydro74000/ngPost?style=for-the-badge">
	<img alt="GitHub top language" src="https://img.shields.io/github/languages/top/Hydro74000/ngPost?style=for-the-badge">
	<img alt="GitHub License" src="https://img.shields.io/github/license/Hydro74000/ngPost?style=for-the-badge&color=%23c20083">
	<img alt="GitHub Issues or Pull Requests" src="https://img.shields.io/github/issues-pr-raw/Hydro74000/ngPost?style=for-the-badge&color=%238575a8">
	<img alt="GitHub Issues or Pull Requests" src="https://img.shields.io/github/issues-raw/Hydro74000/ngPost?style=for-the-badge">

</div>

# About The Project

This application is a high-speed command line and GUI Usenet poster for binaries, designed for secure and efficient data posting. Developed with C++17 and [Qt 6.8.2](https://www.qt.io/blog/qt-6.8.2-released), it features file compression, par2 file generation, and a posting queue for managing multiple uploads. The tool automates tasks by scanning folders and posting files, with options for executing commands post-upload and shutting down the computer upon completion.


![ngPost_v5.3.0](https://github.com/Hydro74000/ngPost/blob/master/pics/ngPost_v5.3.0.png?raw=true)

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
- Multithreading.
- And many more.

[Releases will be available](https://github.com/Hydro74000/ngPost/releases) for, Linux 64 Bit and Windows 64 Bit. Support for MacOS and Raspbian will be considered in the future.

For building the project yourself, please refer to [the wiki](https://github.com/Hydro74000/ngPost/wiki/Build)

## VPN Tunnel Support

ngPost can route selected NNTP servers through an embedded VPN tunnel without changing the system default route. In the GUI, open **VPN...**, choose **OpenVPN** or **WireGuard**, select the VPN configuration file, then tick **Use VPN** on the servers that must use the tunnel.

When a server is marked **Use VPN**, ngPost binds that server's NNTP sockets to the tunnel IP and refuses to post or check through that server if the VPN is unavailable. Other applications and the normal system routing table are left untouched.

**Linux**: A privileged helper script is installed once via `pkexec`/Polkit. The AppImage bundles the VPN runtime assets; source builds install helper resources under `/var/lib/ngpost`.

**Windows**: ngPost uses the **OpenVPN Interactive Service** or the **WireGuard for Windows** installation to bind NNTP sockets to the tunnel interface via `IP_UNICAST_IF`. No extra helper is required.

The related configuration keys are:

```ini
VPN_AUTO_CONNECT = false
VPN_BACKEND = openvpn
VPN_CONFIG_PATH = /path/to/config.ovpn

[server]
useVpn = true
```

`VPN_AUTO_CONNECT` starts the tunnel automatically when a job starts and disconnects it after the queue has been empty for a short grace period. The Linux AppImage/release packaging bundles the VPN runtime assets and helper scripts; source builds can install the runtime helper resources under `/var/lib/ngpost`.

## Structured History And Resume

LevelUp adds a SQLite history database used by both the GUI and CLI. By default
it is stored at the application config path as `ngPost_history.sqlite`; set
`POST_DB = /path/to/ngPost_history.sqlite` to move it. Archive passwords are
stored in clear text for v1 so ngPost can regenerate NZB metadata; set
`HISTORY_STORE_PASSWORDS = false` to disable that. Passwords are masked in GUI,
CLI history output and CSV exports unless an explicit password action is used.

Every post, file, article and NNTP attempt is tracked. Articles are marked
`posted` only after a server `240` response. If the connection is lost before
confirmation, the article becomes `unknown`; resume reposts it with a new
Message-ID and the old ID stays as technical history. Final NZB files are
regenerated from the consolidated history so partial posts cannot silently look
complete.

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

## Command Line Usage

[Please visit the wiki](https://github.com/Hydro74000/ngPost/wiki/Command-Line-Usage)

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
