# Changelog

All notable changes to **CardputerZero Wi-Fi USB Manager** will be documented in this file.

## [0.1.1] - 2026-11-23

### Fixed
- **Debian Package (`postinst` AppStore crash & `dpkg` lockup)**: Removed `systemctl --user restart APPLaunch.service` from `DEBIAN/postinst`. Previously, when installing from the on-device CardputerZero AppStore (`M5CardputerZero-AppStore`), restarting `APPLaunch.service` inside `postinst` terminated the AppStore process tree mid-installation, killing `dpkg` before package configuration completed and leaving `dpkg` in a broken state (`dpkg --configure -a` required).
- **Debian Package (`DEBIAN/control` dependencies)**: Removed strict library version dependencies in `DEBIAN/control` that could cause `dpkg -i` to fail on different firmware builds.
- **Installation Scripts & Documentation**: Removed any remote SSH host assumptions when running directly on the CardputerZero, added one-line direct `.deb` installation & repair commands to `README.md`, and added local pure-Python `.deb` packaging support so `.deb` packages can be built on macOS or Linux without requiring an active SSH connection.

### Added
- Version tracking via `VERSION` (`0.1.1`) and `CHANGELOG.md`.

## [0.1.0] - 2026-11-14

### Added
- Initial release of **CardputerZero Wi-Fi USB Manager** (`M5CardputerZero-WiFiManager`).
- Auto-detection and prioritization of external USB Wi-Fi adapters (`wlan1`, etc.) and built-in wireless interfaces (`wlan0`) via `sysfs` and `nmcli`.
- 320 × 170 LVGL 9.5 UI with single-key navigation (`F`/`X` for Up/Down, `Z`/`C` for Left/Right) and circular marquee scrolling for long SSIDs and adapter details.
- Per-interface NetworkManager connection profile creation (`<ifname>-<ssid>`).
- Custom 100 × 100 RGBA Cyber-Hologram Radar Wi-Fi launcher icon.
