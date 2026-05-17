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


![ngPost_v5.0.0](https://github.com/Hydro74000/ngPost/blob/rc-4.17/pics/ngPost_v4.17.png?raw=true)

# Getting Started

All the features are [highlighted here](https://github.com/Hydro74000/ngPost/wiki/Features). Some of the prominent ones being:
- Full obfuscation of the post.
- Built in posting queue, allows for the addition of items to the queue whilst the queue is processing.
- Full posting automation.
- Compression using RAR or 7zip.
- Multiple server support.
- Optional Linux VPN tunnel for ngPost NNTP traffic only (OpenVPN/WireGuard, per-server opt-in).
- Multithreading.
- And many more.

[Releases will be available](https://github.com/Hydro74000/ngPost/releases) for, Linux 64 Bit and Windows 64 Bit. Support for MacOS and Raspbian will be considered in the future.

For building the project yourself, please refer to [the wiki](https://github.com/Hydro74000/ngPost/wiki/Build)

## Linux VPN Tunnel

ngPost can route selected NNTP servers through an embedded VPN tunnel on Linux without changing the system default route. In the GUI, open **VPN...**, install the privileged helper once, choose **OpenVPN** or **WireGuard**, select the VPN configuration file, then tick **Use VPN** on the servers that must use the tunnel.

The helper is installed through `pkexec`/Polkit and is limited to ngPost's VPN helper for the current user. When a server is marked **Use VPN**, ngPost binds that server's NNTP sockets to the tunnel IP and refuses to post or check through that server if the VPN is unavailable. Other applications and the normal system routing table are left untouched.

The related configuration keys are:

```ini
VPN_AUTO_CONNECT = false
VPN_BACKEND = openvpn
VPN_CONFIG_PATH = /path/to/config.ovpn

[server]
useVpn = true
```

`VPN_AUTO_CONNECT` starts the tunnel automatically when a job starts and disconnects it after the queue has been empty for a short grace period. The Linux AppImage/release packaging bundles the VPN runtime assets and helper scripts; source builds can install the runtime helper resources under `/var/lib/ngpost`.

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
