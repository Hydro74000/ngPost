#!/bin/bash
git pull
make clean
rm Makefile
$MB_QMAKE -o Makefile ngPost.pro CONFIG+=release
make -j 2
cp ngPost ../release/ngPost/

# Bundle VPN binaries if available in ../vpn-bundle/linux-x86_64/
# Expected files: openvpn, wireguard-go, wg
VPN_SRC="../vpn-bundle/linux-x86_64"
if [ -d "$VPN_SRC" ]; then
    mkdir -p ../release/ngPost/vpn
    for bin in openvpn wireguard-go wg; do
        if [ -x "$VPN_SRC/$bin" ]; then
            cp "$VPN_SRC/$bin" "../release/ngPost/vpn/"
            chmod 755 "../release/ngPost/vpn/$bin"
        else
            echo "WARNING: $VPN_SRC/$bin missing or not executable - skipping"
        fi
    done
else
    echo "INFO: no $VPN_SRC directory - VPN binaries not bundled (ngPost will fall back to PATH)"
fi

# VPN runtime: helper, install/uninstall scripts and the polkit rule template
# all live next to the ngPost binary inside the AppImage. The user clicks
# "Install..." in the VPN dialog which calls pkexec with the install script.
cp vpn/scripts/ngpost-vpn-helper.sh    ../release/ngPost/
cp vpn/scripts/ngpost-vpn-install.sh   ../release/ngPost/
cp vpn/scripts/ngpost-vpn-uninstall.sh ../release/ngPost/
cp vpn/polkit/49-ngpost-vpn.rules.in   ../release/ngPost/
chmod 755 ../release/ngPost/ngpost-vpn-*.sh
chmod 644 ../release/ngPost/49-ngpost-vpn.rules.in

cd ..
cp  ngPost.conf release_notes.txt README* release/ngPost/
cd ..
/home/mb/apps/bin/linuxdeployqt-6-x86_64.AppImage ngPost/ngPost.desktop -appimage -qmake=$MB_QMAKE
ls -alrt

