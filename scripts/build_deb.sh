#!/usr/bin/env bash
# CardputerZero Wi-Fi USB Manager - Debian (.deb) Package Builder
# Author: Adriano Dias Moraes <adriano.moraes@gmail.com>
# SPDX-License-Identifier: MIT
#
# Builds the arm64 .deb package locally using Python (tarfile + lzma + GNU ar format)
# from the prebuilt dist/M5CardputerZero-WiFiManager binary, so no SSH connection or
# dpkg-deb binary is required on macOS/Linux.
set -euo pipefail

cd "$(dirname "$0")/.."
VERSION="$(tr -d ' \t\r\n' < VERSION)"
DEB_NAME="wifi-manager_${VERSION}-m5stack1_arm64.deb"

mkdir -p artifacts

if [ ! -f "dist/M5CardputerZero-WiFiManager" ]; then
    echo "Error: dist/M5CardputerZero-WiFiManager not found." >&2
    exit 1
fi

python3 - "${VERSION}" "artifacts/${DEB_NAME}" <<'PYEOF'
import io
import os
import stat
import sys
import tarfile
import time

version = sys.argv[1]
out_deb = sys.argv[2]
mtime = int(time.time())

def reset_tarinfo(ti, mode, is_dir=False):
    ti.uid = 0
    ti.gid = 0
    ti.uname = "root"
    ti.gname = "root"
    ti.mtime = mtime
    ti.mode = mode
    if is_dir:
        ti.type = tarfile.DIRTYPE

def add_dir(tf, path, mode=0o755):
    ti = tarfile.TarInfo(path)
    reset_tarinfo(ti, mode, is_dir=True)
    tf.addfile(ti)

def add_bytes(tf, path, data_bytes, mode=0o644):
    ti = tarfile.TarInfo(path)
    reset_tarinfo(ti, mode, is_dir=False)
    ti.size = len(data_bytes)
    tf.addfile(ti, io.BytesIO(data_bytes))

def add_disk_file(tf, arcname, src_path, mode):
    with open(src_path, "rb") as f:
        data = f.read()
    add_bytes(tf, arcname, data, mode=mode)

# 1. Build control.tar.xz (NO postinst that restarts APPLaunch.service, so AppStore never crashes!)
control_text = f"""Package: wifi-manager
Version: {version}-m5stack1
Section: utils
Priority: optional
Architecture: arm64
Maintainer: Adriano Dias Moraes <adriano.moraes@gmail.com>
Homepage: https://github.com/adrianomoraes/CardputerZero-WiFiManager
Description: CardputerZero USB & Built-in Wi-Fi Adapter Manager
 A 320x170 LVGL 9.5 application for M5Stack CardputerZero (APPLaunch)
 to select external USB Wi-Fi adapters or built-in wireless interfaces,
 scan Wi-Fi networks, and connect with passwords.
"""

postinst_text = """#!/bin/sh
set -e
chmod 0755 /usr/share/APPLaunch/bin/M5CardputerZero-WiFiManager 2>/dev/null || true
exit 0
"""

control_buf = io.BytesIO()
with tarfile.open(fileobj=control_buf, mode="w:xz", format=tarfile.GNU_FORMAT) as tf:
    add_dir(tf, "./", 0o755)
    add_bytes(tf, "./control", control_text.encode("utf-8"), 0o644)
    add_bytes(tf, "./postinst", postinst_text.encode("utf-8"), 0o755)
control_tar_xz = control_buf.getvalue()

# 2. Build data.tar.xz
data_buf = io.BytesIO()
with tarfile.open(fileobj=data_buf, mode="w:xz", format=tarfile.GNU_FORMAT) as tf:
    for d in [
        "./",
        "./usr/",
        "./usr/share/",
        "./usr/share/APPLaunch/",
        "./usr/share/APPLaunch/applications/",
        "./usr/share/APPLaunch/bin/",
        "./usr/share/APPLaunch/share/",
        "./usr/share/APPLaunch/share/images/",
    ]:
        add_dir(tf, d, 0o755)
    add_disk_file(
        tf,
        "./usr/share/APPLaunch/applications/wifi-manager.desktop",
        "applications/wifi-manager.desktop",
        0o644,
    )
    add_disk_file(
        tf,
        "./usr/share/APPLaunch/bin/M5CardputerZero-WiFiManager",
        "dist/M5CardputerZero-WiFiManager",
        0o755,
    )
    add_disk_file(
        tf,
        "./usr/share/APPLaunch/share/images/wifi-manager.png",
        "share/images/wifi-manager.png",
        0o644,
    )
data_tar_xz = data_buf.getvalue()

# 3. Write standard GNU ar archive (.deb)
def ar_member(name, content):
    hdr = f"{name:<16}{mtime:<12}{0:<6}{0:<6}{100644:<8}{len(content):<10}`\n".encode("ascii")
    pad = b"\n" if (len(content) % 2 == 1) else b""
    return hdr + content + pad

deb_bytes = (
    b"!<arch>\n"
    + ar_member("debian-binary", b"2.0\n")
    + ar_member("control.tar.xz", control_tar_xz)
    + ar_member("data.tar.xz", data_tar_xz)
)

with open(out_deb, "wb") as f:
    f.write(deb_bytes)

print(f"Successfully packaged {out_deb} ({len(deb_bytes)} bytes)")
PYEOF

# Keep a stable symlink/copy at artifacts/wifi-manager_latest_arm64.deb as well
cp -f "artifacts/${DEB_NAME}" "artifacts/wifi-manager_latest_arm64.deb"
rm -f artifacts/wifi-manager_0.1.0-m5stack1_arm64.deb
echo "Updated artifacts/${DEB_NAME} and artifacts/wifi-manager_latest_arm64.deb"
