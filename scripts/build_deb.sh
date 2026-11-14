#!/usr/bin/env bash
# CardputerZero Wi-Fi USB Manager - Debian (.deb) Package Builder
# Author: Adriano Dias Moraes <adriano.moraes@gmail.com>
# SPDX-License-Identifier: MIT
set -euo pipefail

HOST="${1:-${CARDPUTER_ZERO_HOST:-192.168.1.210}}"
SSH_USER="${2:-${CARDPUTER_ZERO_USER:-adrianomoraes}}"
TARGET="${SSH_USER}@${HOST}"
REMOTE_BUILD_DIR="/home/${SSH_USER}/CardputerZero-WiFiManager-build"
VERSION="0.1.0"
DEB_NAME="wifi-manager_${VERSION}-m5stack1_arm64.deb"

cd "$(dirname "$0")"
mkdir -p artifacts dist

echo "Packaging ${DEB_NAME} on ${TARGET}..."

python3 - "${TARGET}" "${REMOTE_BUILD_DIR}" "${VERSION}" "${DEB_NAME}" "${CARDPUTER_ZERO_PASSWORD:-}" <<'PYEOF'
import os, pty, select, signal, sys, time

target, remote_dir, version, deb_name, password = sys.argv[1:6]

def run_interactive(cmd_args, timeout=300):
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(cmd_args[0], cmd_args)
    output = b""
    sent_pw = False
    start = time.time()
    timed_out = False
    while True:
        if not timed_out and time.time() - start > timeout:
            timed_out = True
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass
        r, _, _ = select.select([fd], [], [], 1.0)
        if r:
            try:
                data = os.read(fd, 4096)
                if not data:
                    break
                sys.stdout.buffer.write(data)
                sys.stdout.flush()
                if not sent_pw:
                    output += data
                    if password and b"assword:" in output:
                        os.write(fd, (password + "\n").encode("utf-8"))
                        sent_pw = True
            except OSError:
                break
    os.close(fd)
    _, status = os.waitpid(pid, 0)
    if timed_out:
        sys.exit(124)
    if os.WIFEXITED(status) and os.WEXITSTATUS(status) != 0:
        sys.exit(os.WEXITSTATUS(status))

remote_script = f"""
set -e
cd "{remote_dir}"
PKG_ROOT="/tmp/wifi-manager-deb-stage"
rm -rf "$PKG_ROOT"
mkdir -p "$PKG_ROOT/DEBIAN" \
         "$PKG_ROOT/usr/share/APPLaunch/bin" \
         "$PKG_ROOT/usr/share/APPLaunch/applications" \
         "$PKG_ROOT/usr/share/APPLaunch/share/images"

install -m 0755 /usr/share/APPLaunch/bin/M5CardputerZero-WiFiManager "$PKG_ROOT/usr/share/APPLaunch/bin/M5CardputerZero-WiFiManager"
install -m 0644 /usr/share/APPLaunch/applications/wifi-manager.desktop "$PKG_ROOT/usr/share/APPLaunch/applications/wifi-manager.desktop"
install -m 0644 /usr/share/APPLaunch/share/images/wifi-manager.png "$PKG_ROOT/usr/share/APPLaunch/share/images/wifi-manager.png"

cat > "$PKG_ROOT/DEBIAN/control" <<'EOF'
Package: wifi-manager
Version: {version}-m5stack1
Section: net
Priority: optional
Architecture: arm64
Depends: libc6, libstdc++6, libinput10, libxkbcommon0, libudev1, network-manager
Maintainer: Adriano Dias Moraes <adriano.moraes@gmail.com>
Homepage: https://github.com/adrianomoraes/CardputerZero-WiFiManager
Description: CardputerZero USB & Built-in Wi-Fi Adapter Manager
 A 320x170 LVGL 9.5 application for M5Stack CardputerZero (APPLaunch)
 to select external USB Wi-Fi adapters or built-in wireless interfaces,
 scan Wi-Fi networks, and connect with passwords.
EOF

cat > "$PKG_ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
chmod 0755 /usr/share/APPLaunch/bin/M5CardputerZero-WiFiManager || true
for uid_dir in /run/user/*; do
    if [ -d "$uid_dir" ]; then
        uid=$(basename "$uid_dir")
        user=$(id -nu "$uid" 2>/dev/null || true)
        if [ -n "$user" ]; then
            su - "$user" -c "XDG_RUNTIME_DIR=$uid_dir systemctl --user restart APPLaunch.service" >/dev/null 2>&1 || true
        fi
    fi
done
exit 0
EOF
chmod 0755 "$PKG_ROOT/DEBIAN/postinst"

dpkg-deb --root-owner-group --build "$PKG_ROOT" "/tmp/{deb_name}"
dpkg-deb --info "/tmp/{deb_name}"
dpkg-deb --contents "/tmp/{deb_name}"
"""

run_interactive(["ssh", "-o", "StrictHostKeyChecking=accept-new", target, remote_script], timeout=120)
run_interactive(["scp", "-o", "StrictHostKeyChecking=accept-new", f"{target}:/tmp/{deb_name}", f"artifacts/{deb_name}"], timeout=60)
run_interactive(["scp", "-o", "StrictHostKeyChecking=accept-new", f"{target}:/usr/share/APPLaunch/bin/M5CardputerZero-WiFiManager", "dist/M5CardputerZero-WiFiManager"], timeout=60)
PYEOF

chmod 0755 dist/M5CardputerZero-WiFiManager
echo "Built precompiled Debian package: artifacts/${DEB_NAME}"
echo "Saved precompiled ARM64 binary:   dist/M5CardputerZero-WiFiManager"
