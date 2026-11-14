#!/usr/bin/env bash
# CardputerZero Wi-Fi USB Manager - Build & Deploy Script
# Author: Adriano Dias Moraes <adriano.moraes@gmail.com>
# SPDX-License-Identifier: MIT
set -euo pipefail

HOST="${1:-${CARDPUTER_ZERO_HOST:-192.168.1.210}}"
SSH_USER="${2:-${CARDPUTER_ZERO_USER:-adrianomoraes}}"
TARGET="${SSH_USER}@${HOST}"
REMOTE_BUILD_DIR="/home/${SSH_USER}/CardputerZero-WiFiManager-build"

cd "$(dirname "$0")"

if [ ! -f share/images/wifi-manager.png ]; then
    echo "Generating fallback icon..."
    python3 scripts/generate_icon.py
fi

echo "Syncing project to ${TARGET}:${REMOTE_BUILD_DIR}, building natively on CardputerZero, and installing into APPLaunch..."

tar czf /tmp/cpzero_wifi_src.tar.gz \
    SConstruct config_defaults.mk app-builder.json \
    applications main scripts share

python3 - "${TARGET}" "${REMOTE_BUILD_DIR}" "${CARDPUTER_ZERO_PASSWORD:-}" <<'PYEOF'
import os, pty, select, signal, sys, time

target = sys.argv[1]
remote_dir = sys.argv[2]
password = sys.argv[3]

def run_interactive(cmd_args, timeout=900):
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

run_interactive(["scp", "-o", "StrictHostKeyChecking=accept-new", "/tmp/cpzero_wifi_src.tar.gz", f"{target}:/tmp/cpzero_wifi_src.tar.gz"], timeout=60)

sdk_dir = os.path.dirname(remote_dir) + "/SDK"
remote_script = f"""
set -e
if ! command -v scons >/dev/null 2>&1 || ! dpkg -s libinput-dev >/dev/null 2>&1 || ! dpkg -s python3-parse >/dev/null 2>&1; then
    echo "Installing build dependencies (scons, python3-parse, libinput-dev, libxkbcommon-dev, libudev-dev)..."
    sudo -n apt-get update && sudo -n apt-get install -y scons python3-parse libinput-dev libxkbcommon-dev libudev-dev
fi
mkdir -p "{remote_dir}"
if [ -d "{remote_dir}/SDK" ] && [ ! -d "{sdk_dir}" ]; then
    mv "{remote_dir}/SDK" "{sdk_dir}"
fi
rm -rf "{remote_dir}/SDK"
if [ ! -d "{sdk_dir}" ]; then
    echo "Cloning M5Stack_Linux_Libs SDK..."
    git clone --depth 1 https://github.com/m5stack/M5Stack_Linux_Libs.git "{sdk_dir}"
fi
tar xzf /tmp/cpzero_wifi_src.tar.gz -C "{remote_dir}"
cd "{remote_dir}"
rm -rf dist/M5CardputerZero-WiFiManager
SDK_PATH="{sdk_dir}" CONFIG_REPO_AUTOMATION=1 scons -j$(nproc)
file dist/M5CardputerZero-WiFiManager
sudo -n mkdir -p /usr/share/APPLaunch/bin /usr/share/APPLaunch/applications /usr/share/APPLaunch/share/images
sudo -n install -m 0755 dist/M5CardputerZero-WiFiManager /usr/share/APPLaunch/bin/M5CardputerZero-WiFiManager
sudo -n install -m 0644 applications/wifi-manager.desktop /usr/share/APPLaunch/applications/wifi-manager.desktop
sudo -n install -m 0644 share/images/wifi-manager.png /usr/share/APPLaunch/share/images/wifi-manager.png
systemctl --user restart APPLaunch.service
echo "SUCCESS: Deployed WiFi USB Manager to APPLaunch!"
"""

run_interactive(["ssh", "-o", "StrictHostKeyChecking=accept-new", target, remote_script], timeout=900)
PYEOF
