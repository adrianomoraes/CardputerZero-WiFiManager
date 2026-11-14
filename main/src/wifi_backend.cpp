/*
 * CardputerZero Wi-Fi USB Manager - Network Backend
 * Author: Adriano Dias Moraes <adriano.moraes@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#include "wifi_backend.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace wifi_manager {
namespace {

std::string trim(const std::string &s)
{
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string read_first_line(const std::string &path)
{
    std::ifstream in(path);
    if (!in.is_open()) return "";
    std::string line;
    std::getline(in, line);
    return trim(line);
}

std::string realpath_str(const std::string &path)
{
    char resolved[4096] = {};
    if (realpath(path.c_str(), resolved)) {
        return std::string(resolved);
    }
    return "";
}

std::string basename_str(const std::string &path)
{
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

std::string dirname_str(const std::string &path)
{
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

std::string shell_quote(const std::string &arg)
{
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

int run_cmd(const std::string &cmd, std::string *output_out = nullptr)
{
    std::string full_cmd = "LC_ALL=C " + cmd + " 2>&1";
    FILE *pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) {
        if (output_out) *output_out = "popen failed";
        return -1;
    }
    std::string result;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        result += buf;
    }
    int status = pclose(pipe);
    if (output_out) *output_out = trim(result);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return status;
}

// Parse colon-separated nmcli -t line where '\:' is an escaped colon and '\\' is an escaped backslash.
std::vector<std::string> split_nmcli_terse(const std::string &line)
{
    std::vector<std::string> fields;
    std::string current;
    bool escaped = false;
    for (char c : line) {
        if (escaped) {
            current.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == ':') {
            fields.push_back(current);
            current.clear();
        } else if (c != '\r' && c != '\n') {
            current.push_back(c);
        }
    }
    fields.push_back(current);
    return fields;
}

std::vector<std::string> split_lines(const std::string &text)
{
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::set<std::string> get_saved_wifi_ssids()
{
    std::set<std::string> saved;
    std::string out;
    if (run_cmd("nmcli -t -f NAME,TYPE connection show", &out) == 0) {
        for (const std::string &line : split_lines(out)) {
            std::vector<std::string> f = split_nmcli_terse(line);
            if (f.size() >= 2 && f[1].find("wireless") != std::string::npos) {
                saved.insert(f[0]);
            }
        }
    }
    return saved;
}

}  // namespace

std::vector<WifiAdapter> list_adapters()
{
    std::vector<WifiAdapter> adapters;

    // Gather nmcli device status + active connection
    std::map<std::string, std::pair<std::string, std::string>> nm_state;
    std::string nm_out;
    if (run_cmd("nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status", &nm_out) == 0) {
        for (const std::string &line : split_lines(nm_out)) {
            std::vector<std::string> f = split_nmcli_terse(line);
            if (f.size() >= 4 && f[1] == "wifi") {
                nm_state[f[0]] = {f[2], f[3]};
            }
        }
    }

    // Gather IPv4 addresses via ip -4 -br addr
    std::map<std::string, std::string> ip_map;
    std::string ip_out;
    if (run_cmd("ip -4 -br addr", &ip_out) == 0) {
        for (const std::string &line : split_lines(ip_out)) {
            std::istringstream iss(line);
            std::string ifname, state, addr;
            if (iss >> ifname >> state) {
                if (iss >> addr) {
                    ip_map[ifname] = addr;
                }
            }
        }
    }

    DIR *dir = opendir("/sys/class/net");
    if (!dir) return adapters;

    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == ".." || name.rfind("p2p-", 0) == 0) continue;

        std::string base = "/sys/class/net/" + name;
        struct stat st = {};
        if (stat((base + "/wireless").c_str(), &st) != 0 &&
            stat((base + "/phy80211").c_str(), &st) != 0) {
            continue;
        }

        WifiAdapter adapter;
        adapter.ifname = name;
        adapter.mac = read_first_line(base + "/address");

        std::string dev_real = realpath_str(base + "/device");
        adapter.is_usb = (dev_real.find("/usb") != std::string::npos);

        std::string drv_real = realpath_str(base + "/device/driver");
        adapter.driver = drv_real.empty() ? "unknown" : basename_str(drv_real);

        if (adapter.is_usb) {
            // Walk up one or two directories from the USB interface to read USB product/manufacturer
            std::string usb_dir = dirname_str(dev_real);
            std::string mfg = read_first_line(usb_dir + "/manufacturer");
            std::string prod = read_first_line(usb_dir + "/product");
            if (prod.empty()) {
                std::string parent_dir = dirname_str(usb_dir);
                mfg = read_first_line(parent_dir + "/manufacturer");
                prod = read_first_line(parent_dir + "/product");
            }
            if (!mfg.empty() && !prod.empty()) {
                adapter.model = mfg + " " + prod;
            } else if (!prod.empty()) {
                adapter.model = prod;
            } else {
                adapter.model = "USB Wi-Fi (" + adapter.driver + ")";
            }
        } else {
            adapter.model = "Built-in Wi-Fi (" + adapter.driver + ")";
        }

        auto it = nm_state.find(name);
        if (it != nm_state.end()) {
            adapter.state = it->second.first;
            adapter.active_ssid = it->second.second.empty() ? "--" : it->second.second;
        } else {
            adapter.state = read_first_line(base + "/operstate");
            adapter.active_ssid = "--";
        }

        // Query exact active SSID for this interface if connected
        if (adapter.state == "connected") {
            std::string ssid_out;
            if (run_cmd("iwgetid " + shell_quote(name) + " -r", &ssid_out) == 0 && !ssid_out.empty()) {
                adapter.active_ssid = ssid_out;
            }
        }

        auto ip_it = ip_map.find(name);
        adapter.ipv4 = (ip_it != ip_map.end()) ? ip_it->second : "--";

        adapters.push_back(adapter);
    }
    closedir(dir);

    // Sort USB adapters first so external USB adapters are immediately selected by default,
    // then sort by interface name (e.g. wlan1 before wlan0 if wlan1 is USB).
    std::sort(adapters.begin(), adapters.end(), [](const WifiAdapter &a, const WifiAdapter &b) {
        if (a.is_usb != b.is_usb) return a.is_usb > b.is_usb;
        return a.ifname < b.ifname;
    });

    return adapters;
}

std::vector<WifiNetwork> scan_networks(const std::string &ifname, bool force_rescan, std::string *status_out)
{
    std::vector<WifiNetwork> networks;
    if (ifname.empty()) {
        if (status_out) *status_out = "No Wi-Fi adapter selected";
        return networks;
    }

    if (force_rescan) {
        std::string rescan_out;
        run_cmd("sudo -n nmcli device wifi rescan ifname " + shell_quote(ifname) +
                " || nmcli device wifi rescan ifname " + shell_quote(ifname),
                &rescan_out);
    }

    std::set<std::string> saved_ssids = get_saved_wifi_ssids();
    std::string list_out;
    std::string cmd = "nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY,CHAN,BSSID device wifi list ifname " +
                      shell_quote(ifname) +
                      (force_rescan ? " --rescan yes" : " --rescan no");
    int rc = run_cmd(cmd, &list_out);
    if (rc != 0) {
        if (status_out) *status_out = list_out.empty() ? "Scan failed" : list_out;
        return networks;
    }

    std::map<std::string, size_t> by_ssid;
    for (const std::string &line : split_lines(list_out)) {
        std::vector<std::string> f = split_nmcli_terse(line);
        if (f.size() < 6) continue;

        std::string ssid = f[1];
        if (ssid.empty()) continue;  // Skip hidden unnamed beacons

        WifiNetwork net;
        net.in_use = (f[0] == "*");
        net.ssid = ssid;
        net.signal = std::atoi(f[2].c_str());
        net.security = f[3].empty() ? "OPEN" : f[3];
        net.channel = std::atoi(f[4].c_str());
        net.bssid = f[5];
        net.saved = (saved_ssids.count(ssid) > 0) ||
                    (saved_ssids.count("netplan-" + ifname + "-" + ssid) > 0);

        auto it = by_ssid.find(ssid);
        if (it == by_ssid.end()) {
            by_ssid[ssid] = networks.size();
            networks.push_back(net);
        } else {
            // Merge duplicate BSSIDs for the same SSID: keep strongest signal & active flag
            WifiNetwork &existing = networks[it->second];
            if (net.in_use) existing.in_use = true;
            if (net.signal > existing.signal) {
                existing.signal = net.signal;
                existing.channel = net.channel;
                existing.bssid = net.bssid;
                existing.security = net.security;
            }
        }
    }

    std::sort(networks.begin(), networks.end(), [](const WifiNetwork &a, const WifiNetwork &b) {
        if (a.in_use != b.in_use) return a.in_use > b.in_use;
        if (a.signal != b.signal) return a.signal > b.signal;
        return a.ssid < b.ssid;
    });

    if (status_out) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s: %zu network(s) found", ifname.c_str(), networks.size());
        *status_out = buf;
    }
    return networks;
}

bool connect_network(const std::string &ifname, const std::string &ssid, const std::string &password, std::string *status_out)
{
    if (ifname.empty() || ssid.empty()) {
        if (status_out) *status_out = "Adapter or SSID missing";
        return false;
    }

    // Use a per-adapter connection profile name so wlan0 and wlan1 can each maintain independent bindings
    std::string con_name = ifname + "-" + ssid;
    std::string out;

    if (!password.empty()) {
        // Remove any stale half-created connection with our custom per-interface name first
        run_cmd("sudo -n nmcli connection delete " + shell_quote(con_name) + " >/dev/null 2>&1 || true");

        std::string cmd = "sudo -n nmcli -w 20 device wifi connect " + shell_quote(ssid) +
                          " password " + shell_quote(password) +
                          " ifname " + shell_quote(ifname) +
                          " name " + shell_quote(con_name);
        int rc = run_cmd(cmd, &out);
        if (rc == 0) {
            if (status_out) *status_out = "Connected " + ifname + " to " + ssid;
            return true;
        }
        // Fallback: if nmcli device wifi connect failed to set key-mgmt on older profiles, try explicit add + up
        run_cmd("sudo -n nmcli connection delete " + shell_quote(con_name) + " >/dev/null 2>&1 || true");
        std::string add_cmd = "sudo -n nmcli connection add type wifi con-name " + shell_quote(con_name) +
                              " ifname " + shell_quote(ifname) +
                              " ssid " + shell_quote(ssid) +
                              " wifi-sec.key-mgmt wpa-psk wifi-sec.psk " + shell_quote(password);
        if (run_cmd(add_cmd, &out) == 0) {
            if (run_cmd("sudo -n nmcli -w 20 connection up " + shell_quote(con_name) +
                        " ifname " + shell_quote(ifname), &out) == 0) {
                if (status_out) *status_out = "Connected " + ifname + " to " + ssid;
                return true;
            }
        }
        if (status_out) *status_out = out.empty() ? "Connection failed" : out;
        return false;
    } else {
        // Connect without password (open network or existing saved profile)
        std::string cmd = "sudo -n nmcli -w 20 device wifi connect " + shell_quote(ssid) +
                          " ifname " + shell_quote(ifname);
        int rc = run_cmd(cmd, &out);
        if (rc == 0) {
            if (status_out) *status_out = "Connected " + ifname + " to " + ssid;
            return true;
        }
        if (status_out) *status_out = out.empty() ? "Connection failed" : out;
        return false;
    }
}

bool disconnect_adapter(const std::string &ifname, std::string *status_out)
{
    if (ifname.empty()) return false;
    std::string out;
    int rc = run_cmd("sudo -n nmcli device disconnect " + shell_quote(ifname), &out);
    if (rc == 0) {
        if (status_out) *status_out = "Disconnected " + ifname;
        return true;
    }
    if (status_out) *status_out = out.empty() ? "Disconnect failed" : out;
    return false;
}

bool forget_network(const std::string &ssid, std::string *status_out)
{
    if (ssid.empty()) return false;
    std::string out;
    bool deleted_any = false;
    if (run_cmd("nmcli -t -f NAME,UUID,TYPE connection show", &out) == 0) {
        for (const std::string &line : split_lines(out)) {
            std::vector<std::string> f = split_nmcli_terse(line);
            if (f.size() >= 3 && f[2].find("wireless") != std::string::npos) {
                const std::string &name = f[0];
                const std::string &uuid = f[1];
                if (name == ssid || name.find(ssid) != std::string::npos) {
                    std::string del_out;
                    if (run_cmd("sudo -n nmcli connection delete uuid " + shell_quote(uuid), &del_out) == 0) {
                        deleted_any = true;
                    }
                }
            }
        }
    }
    if (deleted_any) {
        if (status_out) *status_out = "Forgot saved profile for " + ssid;
        return true;
    }
    if (status_out) *status_out = "No saved profile found for " + ssid;
    return false;
}

}  // namespace wifi_manager
