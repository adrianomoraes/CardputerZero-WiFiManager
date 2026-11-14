#!/usr/bin/env bash
# CardputerZero Wi-Fi USB Manager - Zero-Compile Binary Installer
# Author: Adriano Dias Moraes <adriano.moraes@gmail.com>
# SPDX-License-Identifier: MIT
#
# Installs the precompiled .deb package (or precompiled dist/ binary) locally on
# a CardputerZero or remotely via SSH without requiring SCons or compilation.
set -euo pipefail

cd "$(dirname "$0")"
DEB_FILE="$(ls -1 artifacts/wifi-manager_*_arm64.deb 2>/dev/null | tail -n 1 || true)"

# If running directly on the CardputerZero (Linux aarch64) and no remote host arg is given:
if [ $# -eq 0 ] && [ "$(uname -s)" = "Linux" ] && [ "$(uname -m)" = "aarch64" ]; then
    if [ -n "$DEB_FILE" ] && [ -f "$DEB_FILE" ]; then
        echo "Installing precompiled package ${DEB_FILE}..."
        sudo dpkg -i "$DEB_FILE"
    elif [ -f "dist/M5CardputerZero-WiFiManager" ]; then
        echo "Installing precompiled binary and assets into /usr/share/APPLaunch..."
        sudo mkdir -p /usr/share/APPLaunch/bin /usr/share/APPLaunch/applications /usr/share/APPLaunch/share/images
        sudo install -m 0755 dist/M5CardputerZero-WiFiManager /usr/share/APPLaunch/bin/M5CardputerZero-WiFiManager
        sudo install -m 0644 applications/wifi-manager.desktop /usr/share/APPLaunch/applications/wifi-manager.desktop
        sudo install -m 0644 share/images/wifi-manager.png /usr/share/APPLaunch/share/images/wifi-manager.png
        systemctl --user restart APPLaunch.service || true
    else
        echo "Error: No precompiled package or binary found." >&2
        exit 1
    fi
    echo "Done! WiFi USB Manager is installed."
    exit 0
fi

# Otherwise install remotely over SSH to a CardputerZero:
HOST="${1:-${CARDPUTER_ZERO_HOST:-}}"
SSH_USER="${2:-${CARDPUTER_ZERO_USER:-adrianomoraes}}"
if [ -z "$HOST" ]; then
    echo "Usage:"
    echo "  On CardputerZero:   ./install_prebuilt.sh"
    echo "  From Mac/PC (SSH):  ./install_prebuilt.sh <cardputer-ip> [ssh-user]"
    exit 2
fi

if [ -z "$DEB_FILE" ] || [ ! -f "$DEB_FILE" ]; then
    echo "Error: Precompiled package not found in artifacts/." >&2
    exit 1
fi

TARGET="${SSH_USER}@${HOST}"
echo "Uploading ${DEB_FILE} to ${TARGET} and installing without compilation..."
scp -o StrictHostKeyChecking=accept-new "$DEB_FILE" "${TARGET}:/tmp/wifi-manager_arm64.deb"
ssh -o StrictHostKeyChecking=accept-new "${TARGET}" "sudo dpkg -i /tmp/wifi-manager_arm64.deb && systemctl --user restart APPLaunch.service"
echo "Successfully installed ${DEB_FILE} on ${TARGET}!"
