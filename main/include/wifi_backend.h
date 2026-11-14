/*
 * CardputerZero Wi-Fi USB Manager
 * Author: Adriano Dias Moraes <adriano.moraes@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Wireless interface discovery, USB adapter metadata parsing, and NetworkManager
 * (nmcli) backend operations for M5Stack CardputerZero.
 */
#pragma once

#include <string>
#include <vector>

namespace wifi_manager {

struct WifiAdapter {
    std::string ifname;        // e.g. "wlan1", "wlan0"
    bool is_usb = false;       // true if backed by USB bus
    std::string driver;        // e.g. "mt7601u", "brcmfmac"
    std::string model;         // e.g. "MediaTek 802.11 n WLAN" or "Built-in Broadcom"
    std::string mac;           // MAC address
    std::string state;         // "connected", "disconnected", "unavailable", etc.
    std::string active_ssid;   // Currently connected SSID (or "--")
    std::string ipv4;          // e.g. "192.168.1.251/24"
};

struct WifiNetwork {
    std::string ssid;
    std::string bssid;
    int signal = 0;            // 0..100
    std::string security;      // e.g. "WPA2", "WPA3", "OPEN"
    int channel = 0;
    bool in_use = false;
    bool saved = false;        // Whether a saved connection profile exists
};

// Enumerate all wireless network interfaces (USB adapters sorted first).
std::vector<WifiAdapter> list_adapters();

// Scan and list visible Wi-Fi networks on a specific adapter interface.
// If force_rescan is true, requests a fresh scan from NetworkManager first.
std::vector<WifiNetwork> scan_networks(const std::string &ifname, bool force_rescan, std::string *status_out);

// Connect the specified adapter to an SSID (with optional password; if empty and saved/open, connects without password).
bool connect_network(const std::string &ifname, const std::string &ssid, const std::string &password, std::string *status_out);

// Disconnect the specified adapter from its current network.
bool disconnect_adapter(const std::string &ifname, std::string *status_out);

// Forget saved connection profile(s) matching the given SSID.
bool forget_network(const std::string &ssid, std::string *status_out);

}  // namespace wifi_manager
