#!/usr/bin/env bash
# CardputerZero Wi-Fi USB Manager - Zero-Compile Binary Installer
# Author: Adriano Dias Moraes <adriano.moraes@gmail.com>
# SPDX-License-Identifier: MIT
#
# Installs the precompiled .deb package (or precompiled dist/ binary) locally on
# a CardputerZero or remotely via SSH without requiring SCons or compilation.
set -euo pipefail

cd "$(dirname "$0")"
VERSION="$(tr -d ' \t\r\n' < VERSION 2>/dev/null || echo '0.1.1')"
DEB_FILE="artifacts/wifi-manager_${VERSION}-m5stack1_arm64.deb"
if [ ! -f "$DEB_FILE" ]; then
    DEB_FILE="$(ls -1 artifacts/wifi-manager_*_arm64.deb 2>/dev/null | tail -n 1 || true)"
fi

# 1. Running directly on the CardputerZero (Linux aarch64) with no remote host argument:
if [ $# -eq 0 ] && [ "$(uname -s)" = "Linux" ] && [ "$(uname -m)" = "aarch64" ]; then
    # Clean up any broken postinst from v0.1.0 before installing
    if [ -f /var/lib/dpkg/info/wifi-manager.postinst ]; then
        sudo rm -f /var/lib/dpkg/info/wifi-manager.postinst || true
        sudo dpkg --configure -a || true
    fi
    if [ -n "$DEB_FILE" ] && [ -f "$DEB_FILE" ]; then
        echo "Installing precompiled package ${DEB_FILE}..."
        sudo dpkg -i "$DEB_FILE"
    elif [ -f "dist/M5CardputerZero-WiFiManager" ]; then
        echo "Installing precompiled binary and assets into /usr/share/APPLaunch..."
        sudo mkdir -p /usr/share/APPLaunch/bin /usr/share/APPLaunch/applications /usr/share/APPLaunch/share/images
        sudo install -m 0755 dist/M5CardputerZero-WiFiManager /usr/share/APPLaunch/bin/M5CardputerZero-WiFiManager
        sudo install -m 0644 applications/wifi-manager.desktop /usr/share/APPLaunch/applications/wifi-manager.desktop
        sudo install -m 0644 share/images/wifi-manager.png /usr/share/APPLaunch/share/images/wifi-manager.png
    else
        echo "Error: No precompiled package or binary found." >&2
        exit 1
    fi
    # Only restart APPLaunch if we are in an interactive SSH/TTY session (never inside AppStore)
    if [ -t 1 ]; then
        systemctl --user restart APPLaunch.service 2>/dev/null || true
    fi
    echo "Done! WiFi USB Manager v${VERSION} is installed."
    exit 0
fi

# 2. Running on a PC/Mac (or with explicit remote arguments) to deploy over SSH:
HOST="${1:-${CARDPUTER_ZERO_HOST:-}}"
SSH_USER="${2:-${CARDPUTER_ZERO_USER:-pi}}"
if [ -z "$HOST" ]; then
    echo "Usage:"
    echo "  Directly on CardputerZero:  ./install_prebuilt.sh"
    echo "  From PC/Mac over SSH:       ./install_prebuilt.sh <cardputer-ip> [ssh-user]"
    exit 2
fi

if [ -z "$DEB_FILE" ] || [ ! -f "$DEB_FILE" ]; then
    echo "Error: Precompiled package not found in artifacts/." >&2
    exit 1
fi

TARGET="${SSH_USER}@${HOST}"
echo "Uploading ${DEB_FILE} to ${TARGET} and installing without compilation..."
scp -o StrictHostKeyChecking=accept-new "$DEB_FILE" "${TARGET}:/tmp/wifi-manager_arm64.deb"
ssh -o StrictHostKeyChecking=accept-new "${TARGET}" "sudo rm -f /var/lib/dpkg/info/wifi-manager.postinst && sudo dpkg --configure -a || true; sudo dpkg -i /tmp/wifi-manager_arm64.deb && (systemctl --user restart APPLaunch.service 2>/dev/null || true)"
echo "Successfully installed ${DEB_FILE} on ${TARGET}!"
