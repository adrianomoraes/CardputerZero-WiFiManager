/*
 * CardputerZero Wi-Fi USB Manager - LVGL 9.5 UI & Input Controller
 * Author: Adriano Dias Moraes <adriano.moraes@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * 320x170 LVGL application for M5Stack CardputerZero (APPLaunch) to manage
 * external USB Wi-Fi adapters and built-in wireless interfaces.
 */
#include "compat/input_keys.h"
#include "keyboard_input.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/libs/tiny_ttf/lv_tiny_ttf.h"
#include "wifi_backend.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

#if LV_USE_SDL
#include <SDL.h>
#include "lvgl/src/drivers/sdl/lv_sdl_keyboard.h"
#include "lvgl/src/drivers/sdl/lv_sdl_mouse.h"
#include "lvgl/src/drivers/sdl/lv_sdl_window.h"
#endif

namespace {

constexpr int kScreenWidth = 320;
constexpr int kScreenHeight = 170;

enum class ScreenMode {
    Networks,
    Adapters,
    PasswordInput,
    Connecting,
};

enum class AsyncOp {
    None,
    Scan,
    Connect,
    Disconnect,
    Forget,
};

volatile sig_atomic_t g_quit_requested = 0;
lv_obj_t *g_root = nullptr;
lv_indev_t *g_keyboard_indev = nullptr;
lv_group_t *g_group = nullptr;

lv_font_t *g_font_small = nullptr;
lv_font_t *g_font_body = nullptr;
lv_font_t *g_font_title = nullptr;
std::vector<unsigned char> g_font_data;

ScreenMode g_mode = ScreenMode::Networks;
std::vector<wifi_manager::WifiAdapter> g_adapters;
int g_selected_adapter_idx = 0;

std::vector<wifi_manager::WifiNetwork> g_networks;
int g_selected_network_idx = 0;

std::string g_password_input;
bool g_show_password = true;
std::string g_target_ssid;
std::string g_target_security;

std::string g_status_msg = "Press R to scan networks";
bool g_status_error = false;
uint32_t g_esc_down_tick = 0;

// Background worker thread state
pthread_t g_worker_thread;
pthread_mutex_t g_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
bool g_worker_running = false;
bool g_worker_done = false;
AsyncOp g_worker_op = AsyncOp::None;
std::string g_worker_ifname;
std::string g_worker_ssid;
std::string g_worker_password;
bool g_worker_force_rescan = false;

std::vector<wifi_manager::WifiAdapter> g_worker_adapters_out;
std::vector<wifi_manager::WifiNetwork> g_worker_networks_out;
std::string g_worker_status_out;
bool g_worker_ok_out = true;

const char *getenv_default(const char *name, const char *fallback)
{
    const char *value = std::getenv(name);
    return (value && value[0]) ? value : fallback;
}

void request_quit()
{
    g_quit_requested = 1;
    LVGL_RUN_FLAGE = 0;
}

void handle_signal(int)
{
    request_quit();
}

bool read_binary_file(const std::string &path, std::vector<unsigned char> *out)
{
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    if (std::fseek(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return false;
    }
    long size = std::ftell(fp);
    if (size <= 0) {
        std::fclose(fp);
        return false;
    }
    std::rewind(fp);
    out->assign(static_cast<size_t>(size), 0);
    bool ok = std::fread(out->data(), 1, out->size(), fp) == out->size();
    std::fclose(fp);
    if (!ok) out->clear();
    return ok;
}

void init_fonts()
{
#if LV_USE_TINY_TTF
    const char *candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/APPLaunch/share/font/NotoSansSC-Regular.ttf",
        "/usr/share/APPLaunch/fonts/NotoSansSC-Regular.ttf",
        "fonts/NotoSansSC-Regular.ttf",
    };
    for (const char *path : candidates) {
        std::vector<unsigned char> data;
        if (!read_binary_file(path, &data)) continue;
        lv_font_t *f10 = lv_tiny_ttf_create_data(data.data(), data.size(), 11);
        lv_font_t *f12 = lv_tiny_ttf_create_data(data.data(), data.size(), 12);
        lv_font_t *f14 = lv_tiny_ttf_create_data(data.data(), data.size(), 14);
        if (f10 && f12 && f14) {
            g_font_data.swap(data);
            g_font_small = f10;
            g_font_body = f12;
            g_font_title = f14;
            return;
        }
        if (f10) lv_tiny_ttf_destroy(f10);
        if (f12) lv_tiny_ttf_destroy(f12);
        if (f14) lv_tiny_ttf_destroy(f14);
    }
#endif
}

const lv_font_t *font_small()
{
    return g_font_small ? g_font_small : &lv_font_montserrat_12;
}

const lv_font_t *font_body()
{
    return g_font_body ? g_font_body : &lv_font_montserrat_12;
}

const lv_font_t *font_title()
{
    return g_font_title ? g_font_title : &lv_font_montserrat_14;
}

std::string clipped(const std::string &s, size_t max_len)
{
    if (s.size() <= max_len) return s;
    if (max_len <= 3) return s.substr(0, max_len);
    return s.substr(0, max_len - 2) + "..";
}

const wifi_manager::WifiAdapter *current_adapter()
{
    if (g_adapters.empty()) return nullptr;
    if (g_selected_adapter_idx < 0) g_selected_adapter_idx = 0;
    if (g_selected_adapter_idx >= static_cast<int>(g_adapters.size())) {
        g_selected_adapter_idx = static_cast<int>(g_adapters.size()) - 1;
    }
    return &g_adapters[g_selected_adapter_idx];
}

const wifi_manager::WifiNetwork *current_network()
{
    if (g_networks.empty()) return nullptr;
    if (g_selected_network_idx < 0) g_selected_network_idx = 0;
    if (g_selected_network_idx >= static_cast<int>(g_networks.size())) {
        g_selected_network_idx = static_cast<int>(g_networks.size()) - 1;
    }
    return &g_networks[g_selected_network_idx];
}

void render();

void *worker_thread_fn(void *)
{
    pthread_mutex_lock(&g_worker_mutex);
    AsyncOp op = g_worker_op;
    std::string ifname = g_worker_ifname;
    std::string ssid = g_worker_ssid;
    std::string password = g_worker_password;
    bool force_rescan = g_worker_force_rescan;
    pthread_mutex_unlock(&g_worker_mutex);

    std::string status;
    bool ok = true;

    if (op == AsyncOp::Connect) {
        ok = wifi_manager::connect_network(ifname, ssid, password, &status);
    } else if (op == AsyncOp::Disconnect) {
        ok = wifi_manager::disconnect_adapter(ifname, &status);
    } else if (op == AsyncOp::Forget) {
        ok = wifi_manager::forget_network(ssid, &status);
    }

    std::vector<wifi_manager::WifiAdapter> adapters = wifi_manager::list_adapters();
    std::string scan_ifname = ifname;
    if (scan_ifname.empty() && !adapters.empty()) {
        scan_ifname = adapters[0].ifname;
    }
    std::string scan_status;
    std::vector<wifi_manager::WifiNetwork> nets =
        wifi_manager::scan_networks(scan_ifname, (op == AsyncOp::Scan) ? force_rescan : false, &scan_status);
    if (op == AsyncOp::Scan) {
        status = scan_status;
    }

    pthread_mutex_lock(&g_worker_mutex);
    g_worker_adapters_out.swap(adapters);
    g_worker_networks_out.swap(nets);
    g_worker_status_out = status;
    g_worker_ok_out = ok;
    g_worker_done = true;
    pthread_mutex_unlock(&g_worker_mutex);
    return nullptr;
}

void start_async_op(AsyncOp op, const std::string &ifname, const std::string &ssid = "",
                    const std::string &password = "", bool force_rescan = false,
                    const std::string &busy_label = "Working...")
{
    if (g_worker_running) return;
    pthread_mutex_lock(&g_worker_mutex);
    g_worker_running = true;
    g_worker_done = false;
    g_worker_op = op;
    g_worker_ifname = ifname;
    g_worker_ssid = ssid;
    g_worker_password = password;
    g_worker_force_rescan = force_rescan;
    pthread_mutex_unlock(&g_worker_mutex);

    g_status_msg = busy_label;
    g_status_error = false;
    if (op == AsyncOp::Connect) {
        g_mode = ScreenMode::Connecting;
    }
    render();

    pthread_create(&g_worker_thread, nullptr, worker_thread_fn, nullptr);
}

void poll_worker()
{
    if (!g_worker_running) return;
    pthread_mutex_lock(&g_worker_mutex);
    bool done = g_worker_done;
    if (!done) {
        pthread_mutex_unlock(&g_worker_mutex);
        return;
    }
    AsyncOp op = g_worker_op;
    std::string prev_ifname = g_worker_ifname;
    g_adapters.swap(g_worker_adapters_out);
    g_networks.swap(g_worker_networks_out);
    g_status_msg = g_worker_status_out;
    g_status_error = !g_worker_ok_out;
    g_worker_running = false;
    g_worker_done = false;
    pthread_mutex_unlock(&g_worker_mutex);

    pthread_join(g_worker_thread, nullptr);

    // Keep same adapter selected by name if still present
    if (!prev_ifname.empty()) {
        for (size_t i = 0; i < g_adapters.size(); ++i) {
            if (g_adapters[i].ifname == prev_ifname) {
                g_selected_adapter_idx = static_cast<int>(i);
                break;
            }
        }
    }
    if (g_selected_network_idx >= static_cast<int>(g_networks.size())) {
        g_selected_network_idx = std::max(0, static_cast<int>(g_networks.size()) - 1);
    }
    if (op == AsyncOp::Connect) {
        g_mode = g_status_error ? ScreenMode::PasswordInput : ScreenMode::Networks;
    }
    render();
}

lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg_hex, uint32_t border_hex = 0x26354A)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 5, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(border_hex), 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

lv_obj_t *make_label(lv_obj_t *parent, int x, int y, const std::string &text,
                     const lv_font_t *font, uint32_t color_hex = 0xE2E8F0)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color_hex), 0);
    lv_label_set_text(lbl, text.c_str());
    return lbl;
}

lv_obj_t *make_marquee_label(lv_obj_t *parent, int x, int y, int width, const std::string &text,
                             const lv_font_t *font, uint32_t color_hex = 0xE2E8F0)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_width(lbl, width);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(lbl, 4500, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color_hex), 0);
    lv_label_set_text(lbl, text.c_str());
    return lbl;
}

void draw_signal_bars(lv_obj_t *parent, int x, int y, int signal, bool selected)
{
    int bars = 1;
    if (signal >= 75) bars = 4;
    else if (signal >= 50) bars = 3;
    else if (signal >= 25) bars = 2;

    uint32_t active_col = selected ? 0x38BDF8 : 0x22C55E;
    uint32_t dim_col = 0x334155;

    for (int i = 0; i < 4; ++i) {
        int bh = 3 + i * 3;
        int bx = x + i * 4;
        int by = y + (12 - bh);
        lv_obj_t *bar = lv_obj_create(parent);
        lv_obj_remove_style_all(bar);
        lv_obj_set_pos(bar, bx, by);
        lv_obj_set_size(bar, 3, bh);
        lv_obj_set_style_bg_color(bar, lv_color_hex(i < bars ? active_col : dim_col), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }
}

void render_header()
{
    lv_obj_t *hdr = make_panel(g_root, 3, 2, 314, 22, 0x0F172A, 0x1E293B);
    const wifi_manager::WifiAdapter *ad = current_adapter();
    if (!ad) {
        make_label(hdr, 6, 3, "WiFi Manager - No Adapter", font_title(), 0xF87171);
        return;
    }

    // Adapter badge (USB vs Built-in)
    uint32_t badge_bg = ad->is_usb ? 0xD97706 : 0x0284C7;
    lv_obj_t *badge = make_panel(hdr, 4, 3, ad->is_usb ? 36 : 40, 16, badge_bg, badge_bg);
    make_label(badge, 4, 1, ad->is_usb ? "USB" : "INT", font_small(), 0xFFFFFF);

    std::string title = ad->ifname + " (" + ad->driver + ")";
    make_label(hdr, ad->is_usb ? 44 : 48, 3, clipped(title, 20), font_title(), 0xF8FAFC);

    std::string right_info;
    uint32_t right_col = 0x94A3B8;
    if (ad->state == "connected") {
        right_info = ad->ipv4 != "--" ? ad->ipv4 : ad->active_ssid;
        right_col = 0x4ADE80;
    } else {
        right_info = ad->state;
        right_col = 0xFBBF24;
    }
    make_label(hdr, 195, 4, clipped(right_info, 18), font_small(), right_col);
}

void render_footer(const std::string &keys_hint)
{
    lv_obj_t *ftr = make_panel(g_root, 3, 148, 314, 20, 0x0F172A, 0x1E293B);
    uint32_t col = g_status_error ? 0xF87171 : (g_worker_running ? 0x38BDF8 : 0x94A3B8);
    std::string text = g_worker_running ? g_status_msg : (keys_hint.empty() ? g_status_msg : keys_hint);
    make_label(ftr, 6, 3, clipped(text, 50), font_small(), col);
}

void render_networks_screen()
{
    // Left pane: Network list (x=3, y=26, w=194, h=120)
    lv_obj_t *left = make_panel(g_root, 3, 26, 194, 120, 0x0B1120, 0x1E293B);

    if (g_networks.empty()) {
        make_label(left, 10, 42, g_worker_running ? "Scanning Wi-Fi..." : "No networks listed", font_body(), 0x94A3B8);
        make_label(left, 10, 62, "Press R to scan", font_small(), 0x64748B);
    } else {
        constexpr int kVisibleRows = 5;
        int total = static_cast<int>(g_networks.size());
        int start = std::max(0, g_selected_network_idx - kVisibleRows / 2);
        if (start + kVisibleRows > total) {
            start = std::max(0, total - kVisibleRows);
        }
        int end = std::min(total, start + kVisibleRows);

        for (int i = start; i < end; ++i) {
            int row_y = 4 + (i - start) * 23;
            bool sel = (i == g_selected_network_idx);
            const wifi_manager::WifiNetwork &net = g_networks[i];

            uint32_t row_bg = sel ? 0x1E3A5F : 0x0F172A;
            uint32_t row_border = sel ? 0x38BDF8 : 0x1E293B;
            lv_obj_t *row = make_panel(left, 4, row_y, 186, 21, row_bg, row_border);

            draw_signal_bars(row, 4, 4, net.signal, sel);

            std::string prefix = net.in_use ? "* " : (net.saved ? "+ " : "");
            uint32_t text_col = net.in_use ? 0x4ADE80 : (sel ? 0xFFFFFF : 0xE2E8F0);
            make_label(row, 24, 3, clipped(prefix + net.ssid, 18), font_body(), text_col);

            std::string sec_short = (net.security == "OPEN") ? "OPN" : "LCK";
            make_label(row, 156, 4, sec_short, font_small(), net.security == "OPEN" ? 0x4ADE80 : 0xFBBF24);
        }
    }

    // Right pane: Adapter & Selected Network Details (x=200, y=26, w=117, h=120)
    lv_obj_t *right = make_panel(g_root, 200, 26, 117, 120, 0x0B1120, 0x1E293B);
    const wifi_manager::WifiAdapter *ad = current_adapter();
    const wifi_manager::WifiNetwork *net = current_network();

    if (ad) {
        make_marquee_label(right, 5, 3, 107, ad->model, font_small(), 0x38BDF8);
        make_marquee_label(right, 5, 16, 107, "Cur: " + ad->active_ssid, font_small(),
                           ad->state == "connected" ? 0x4ADE80 : 0x94A3B8);
    }

    // Divider line
    lv_obj_t *div = make_panel(right, 4, 31, 109, 1, 0x1E293B, 0x1E293B);
    (void)div;

    if (net) {
        make_marquee_label(right, 5, 35, 107, net->ssid, font_body(), 0xF8FAFC);
        char sig_buf[64];
        std::snprintf(sig_buf, sizeof(sig_buf), "Sig: %d%% Ch:%d", net->signal, net->channel);
        make_label(right, 5, 51, sig_buf, font_small(), 0xCBD5E1);
        make_marquee_label(right, 5, 64, 107, "Sec: " + net->security, font_small(), 0xFBBF24);
        make_label(right, 5, 77, net->in_use ? "Status: Connected" : (net->saved ? "Profile: Saved" : "Profile: New"),
                   font_small(), net->in_use ? 0x4ADE80 : 0x94A3B8);
    } else {
        make_label(right, 5, 45, "No AP selected", font_small(), 0x64748B);
    }

    make_marquee_label(right, 5, 93, 107, g_status_msg, font_small(),
                       g_status_error ? 0xF87171 : 0x38BDF8);

    render_footer("F/X:Up/Dn Z/C:Adapter Tab:List Ent:Conn R:Scan");
}

void render_adapters_screen()
{
    lv_obj_t *panel = make_panel(g_root, 3, 26, 314, 120, 0x0B1120, 0x38BDF8);
    make_label(panel, 8, 4, "Select Wi-Fi Interface (USB / Built-in):", font_title(), 0x38BDF8);

    if (g_adapters.empty()) {
        make_label(panel, 10, 45, "No wireless interfaces detected!", font_body(), 0xF87171);
    } else {
        int max_rows = std::min(static_cast<int>(g_adapters.size()), 3);
        for (int i = 0; i < max_rows; ++i) {
            const wifi_manager::WifiAdapter &ad = g_adapters[i];
            bool sel = (i == g_selected_adapter_idx);
            int ry = 24 + i * 30;
            lv_obj_t *row = make_panel(panel, 6, ry, 302, 27, sel ? 0x1E3A5F : 0x0F172A,
                                       sel ? 0x38BDF8 : 0x1E293B);

            std::string tag = ad.is_usb ? "[USB] " : "[INT] ";
            std::string line1 = tag + ad.ifname + " - " + clipped(ad.model, 24);
            make_label(row, 6, 2, line1, font_body(), sel ? 0xFFFFFF : 0xE2E8F0);

            std::string line2 = "State: " + ad.state + " | SSID: " + clipped(ad.active_ssid, 12) + " | IP: " + ad.ipv4;
            make_label(row, 6, 14, clipped(line2, 48), font_small(),
                       ad.state == "connected" ? 0x4ADE80 : 0x94A3B8);
        }
    }

    render_footer("F/X/Up/Dn:Select Adapter  Enter:Use & Scan  Esc:Back");
}

void render_password_screen()
{
    lv_obj_t *panel = make_panel(g_root, 3, 26, 314, 120, 0x0B1120, 0x38BDF8);
    const wifi_manager::WifiAdapter *ad = current_adapter();
    std::string ifname = ad ? (ad->ifname + (ad->is_usb ? " [USB]" : " [INT]")) : "wlan";

    make_label(panel, 8, 5, "Connect " + ifname + " to:", font_small(), 0x94A3B8);
    make_label(panel, 8, 19, clipped(g_target_ssid, 34) + " (" + g_target_security + ")", font_title(), 0xF8FAFC);

    make_label(panel, 8, 40, "Password (leave empty if saved/open):", font_small(), 0xCBD5E1);

    lv_obj_t *input_box = make_panel(panel, 8, 55, 298, 26, 0x0F172A, 0x38BDF8);
    std::string shown;
    if (g_show_password) {
        shown = g_password_input + "_";
    } else {
        shown = std::string(g_password_input.size(), '*') + "_";
    }
    make_label(input_box, 6, 5, clipped(shown, 42), font_body(), 0x38BDF8);

    make_label(panel, 8, 86, g_show_password ? "Tab: Hide Password" : "Tab: Show Password", font_small(), 0x94A3B8);
    if (g_status_error && !g_status_msg.empty()) {
        make_label(panel, 8, 101, clipped(g_status_msg, 46), font_small(), 0xF87171);
    } else {
        make_label(panel, 8, 101, "Press Enter to connect, Backspace to edit", font_small(), 0x64748B);
    }

    render_footer("Enter:Connect  Tab:Show/Hide  Bksp:Del  Esc:Cancel");
}

void render_connecting_screen()
{
    lv_obj_t *panel = make_panel(g_root, 3, 26, 314, 120, 0x0B1120, 0x38BDF8);
    make_label(panel, 16, 32, "Configuring Wi-Fi Adapter...", font_title(), 0x38BDF8);
    make_label(panel, 16, 56, "Adapter: " + g_worker_ifname, font_body(), 0xF8FAFC);
    make_label(panel, 16, 74, "Network: " + clipped(g_worker_ssid, 32), font_body(), 0x4ADE80);
    make_label(panel, 16, 94, "Please wait (up to 20s for DHCP)...", font_small(), 0x94A3B8);
    render_footer("Connecting via NetworkManager...");
}

void render()
{
    if (!g_root) return;
    lv_obj_clean(g_root);
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0x020617), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);

    render_header();

    switch (g_mode) {
    case ScreenMode::Networks:
        render_networks_screen();
        break;
    case ScreenMode::Adapters:
        render_adapters_screen();
        break;
    case ScreenMode::PasswordInput:
        render_password_screen();
        break;
    case ScreenMode::Connecting:
        render_connecting_screen();
        break;
    }
}

bool is_press(const key_item *item)
{
    return item && (item->key_state == KBD_KEY_PRESSED || item->key_state == KBD_KEY_REPEATED);
}

char ascii_lower(const key_item *item)
{
    if (!item || !item->utf8[0] || item->utf8[1] != '\0') return '\0';
    char c = item->utf8[0];
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return c;
}

void handle_esc_key(const key_item *item)
{
    if (!item) return;
    if (item->key_state == KBD_KEY_PRESSED) {
        g_esc_down_tick = lv_tick_get();
        return;
    }
    if (item->key_state == KBD_KEY_REPEATED) {
        if (g_esc_down_tick && lv_tick_elaps(g_esc_down_tick) > 700) {
            request_quit();
        }
        return;
    }
    if (item->key_state == KBD_KEY_RELEASED) {
        uint32_t held = g_esc_down_tick ? lv_tick_elaps(g_esc_down_tick) : 0;
        g_esc_down_tick = 0;
        if (held > 700) {
            request_quit();
            return;
        }
        if (g_mode == ScreenMode::PasswordInput || g_mode == ScreenMode::Adapters) {
            g_mode = ScreenMode::Networks;
            g_status_error = false;
            render();
        } else if (g_mode == ScreenMode::Networks && !g_worker_running) {
            request_quit();
        }
    }
}

void handle_networks_key(const key_item *item)
{
    if (!is_press(item) || g_worker_running) return;
    char ch = ascii_lower(item);

    if (item->key_code == KEY_UP || ch == 'f' || ch == ';') {
        if (!g_networks.empty()) {
            g_selected_network_idx = (g_selected_network_idx - 1 + static_cast<int>(g_networks.size())) %
                                     static_cast<int>(g_networks.size());
            render();
        }
        return;
    }
    if (item->key_code == KEY_DOWN || ch == 'x' || ch == '.') {
        if (!g_networks.empty()) {
            g_selected_network_idx = (g_selected_network_idx + 1) % static_cast<int>(g_networks.size());
            render();
        }
        return;
    }
    if (item->key_code == KEY_LEFT || item->key_code == KEY_RIGHT || ch == 'z' || ch == 'c') {
        if (g_adapters.size() > 1) {
            int delta = (item->key_code == KEY_RIGHT || ch == 'c') ? 1 : -1;
            g_selected_adapter_idx = (g_selected_adapter_idx + delta + static_cast<int>(g_adapters.size())) %
                                     static_cast<int>(g_adapters.size());
            const wifi_manager::WifiAdapter *ad = current_adapter();
            if (ad) {
                g_selected_network_idx = 0;
                start_async_op(AsyncOp::Scan, ad->ifname, "", "", false, "Switching to " + ad->ifname + "...");
            }
        }
        return;
    }
    if (item->key_code == KEY_TAB || ch == 'a' || ch == 'i') {
        g_adapters = wifi_manager::list_adapters();
        g_mode = ScreenMode::Adapters;
        render();
        return;
    }
    if (ch == 'r') {
        const wifi_manager::WifiAdapter *ad = current_adapter();
        if (ad) {
            start_async_op(AsyncOp::Scan, ad->ifname, "", "", true, "Scanning on " + ad->ifname + "...");
        }
        return;
    }
    if (ch == 'd') {
        const wifi_manager::WifiAdapter *ad = current_adapter();
        if (ad) {
            start_async_op(AsyncOp::Disconnect, ad->ifname, "", "", false, "Disconnecting " + ad->ifname + "...");
        }
        return;
    }
    if (ch == 'k' || item->key_code == KEY_BACKSPACE || item->key_code == KEY_DELETE) {
        const wifi_manager::WifiNetwork *net = current_network();
        const wifi_manager::WifiAdapter *ad = current_adapter();
        if (net && ad) {
            start_async_op(AsyncOp::Forget, ad->ifname, net->ssid, "", false, "Forgetting " + net->ssid + "...");
        }
        return;
    }
    if (item->key_code == KEY_ENTER || item->key_code == KEY_KPENTER) {
        const wifi_manager::WifiNetwork *net = current_network();
        const wifi_manager::WifiAdapter *ad = current_adapter();
        if (!net || !ad) return;
        g_target_ssid = net->ssid;
        g_target_security = net->security;
        g_password_input.clear();
        g_status_error = false;
        if (net->security == "OPEN") {
            start_async_op(AsyncOp::Connect, ad->ifname, net->ssid, "", false,
                           "Connecting " + ad->ifname + " to " + net->ssid + "...");
        } else {
            g_mode = ScreenMode::PasswordInput;
            render();
        }
        return;
    }
}

void handle_adapters_key(const key_item *item)
{
    if (!is_press(item) || g_worker_running) return;
    char ch = ascii_lower(item);

    if (item->key_code == KEY_UP || item->key_code == KEY_LEFT || ch == 'f' || ch == 'z' || ch == ';') {
        if (!g_adapters.empty()) {
            g_selected_adapter_idx = (g_selected_adapter_idx - 1 + static_cast<int>(g_adapters.size())) %
                                     static_cast<int>(g_adapters.size());
            render();
        }
        return;
    }
    if (item->key_code == KEY_DOWN || item->key_code == KEY_RIGHT || ch == 'x' || ch == 'c' || ch == '.') {
        if (!g_adapters.empty()) {
            g_selected_adapter_idx = (g_selected_adapter_idx + 1) % static_cast<int>(g_adapters.size());
            render();
        }
        return;
    }
    if (ch == 'r') {
        g_adapters = wifi_manager::list_adapters();
        render();
        return;
    }
    if (item->key_code == KEY_ENTER || item->key_code == KEY_KPENTER || item->key_code == KEY_TAB) {
        g_mode = ScreenMode::Networks;
        const wifi_manager::WifiAdapter *ad = current_adapter();
        if (ad) {
            g_selected_network_idx = 0;
            start_async_op(AsyncOp::Scan, ad->ifname, "", "", false, "Loading networks on " + ad->ifname + "...");
        } else {
            render();
        }
        return;
    }
}

void handle_password_key(const key_item *item)
{
    if (!is_press(item) || g_worker_running) return;

    if (item->key_code == KEY_TAB) {
        g_show_password = !g_show_password;
        render();
        return;
    }
    if (item->key_code == KEY_ENTER || item->key_code == KEY_KPENTER) {
        const wifi_manager::WifiAdapter *ad = current_adapter();
        if (!ad) return;
        start_async_op(AsyncOp::Connect, ad->ifname, g_target_ssid, g_password_input, false,
                       "Connecting " + ad->ifname + " to " + g_target_ssid + "...");
        return;
    }
    if (item->key_code == KEY_BACKSPACE || item->key_code == KEY_DELETE) {
        if (!g_password_input.empty()) {
            g_password_input.pop_back();
            render();
        }
        return;
    }
    // Printable ASCII characters
    if (item->utf8[0] >= 32 && item->utf8[0] < 127 && item->utf8[1] == '\0') {
        g_password_input.push_back(item->utf8[0]);
        render();
    }
}

void keyboard_event_cb(lv_event_t *event)
{
    key_item *item = static_cast<key_item *>(lv_event_get_param(event));
    if (!item) return;
    if (item->key_code == KEY_ESC) {
        handle_esc_key(item);
        return;
    }
    if (g_mode == ScreenMode::Networks) handle_networks_key(item);
    else if (g_mode == ScreenMode::Adapters) handle_adapters_key(item);
    else if (g_mode == ScreenMode::PasswordInput) handle_password_key(item);
}

int get_lcd_fbdev(char *dev_path, size_t buf_size)
{
    if (!dev_path || buf_size == 0) return -1;
    // Prefer /dev/fb_lcd symlink if present on CardputerZero
    if (access("/dev/fb_lcd", R_OK | W_OK) == 0) {
        std::snprintf(dev_path, buf_size, "/dev/fb_lcd");
        return 0;
    }
    FILE *fp = std::fopen("/proc/fb", "r");
    if (!fp) return -1;
    char line[256];
    int fb_num = -1;
    while (std::fgets(line, sizeof(line), fp)) {
        if ((std::strstr(line, "fb_st7789v") || std::strstr(line, "panel-mipi-dbi")) &&
            std::sscanf(line, "%d", &fb_num) == 1) {
            break;
        }
    }
    std::fclose(fp);
    if (fb_num < 0) return -1;
    std::snprintf(dev_path, buf_size, "/dev/fb%d", fb_num);
    return 0;
}

#if LV_USE_EVDEV
int evdev_to_lv_key(uint16_t code)
{
    switch (code) {
    case KEY_UP: return LV_KEY_UP;
    case KEY_DOWN: return LV_KEY_DOWN;
    case KEY_RIGHT: return LV_KEY_RIGHT;
    case KEY_LEFT: return LV_KEY_LEFT;
    case KEY_ESC: return LV_KEY_ESC;
    case KEY_DELETE: return LV_KEY_DEL;
    case KEY_BACKSPACE: return LV_KEY_BACKSPACE;
    case KEY_ENTER: return LV_KEY_ENTER;
    case KEY_TAB: return KEY_TAB;
    case KEY_HOME: return LV_KEY_HOME;
    case KEY_END: return LV_KEY_END;
    default: return code;
    }
}

void keypad_read_cb(lv_indev_t *, lv_indev_data_t *data)
{
    data->state = LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
    pthread_mutex_lock(&keyboard_mutex);
    if (!STAILQ_EMPTY(&keyboard_queue)) {
        key_item *elm = STAILQ_FIRST(&keyboard_queue);
        STAILQ_REMOVE_HEAD(&keyboard_queue, entries);
        if (g_root) lv_obj_send_event(g_root, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), elm);
        data->key = evdev_to_lv_key(elm->key_code);
        data->state = static_cast<lv_indev_state_t>(elm->key_state);
        data->continue_reading = !STAILQ_EMPTY(&keyboard_queue);
        std::free(elm);
    }
    pthread_mutex_unlock(&keyboard_mutex);
}

void lv_linux_indev_init()
{
    const char *keyboard_device = getenv_default(
        "LV_LINUX_KEYBOARD_DEVICE",
        getenv_default("APPLAUNCH_LINUX_KEYBOARD_DEVICE",
                       "/dev/input/by-path/platform-3f804000.i2c-event"));
    pthread_t thread_id;
    pthread_create(&thread_id, nullptr, keyboard_read_thread, const_cast<char *>(keyboard_device));
    pthread_detach(thread_id);
    g_keyboard_indev = lv_indev_create();
    lv_indev_set_type(g_keyboard_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(g_keyboard_indev, keypad_read_cb);
}
#endif

#if LV_USE_LINUX_FBDEV
void lv_linux_disp_init()
{
    char fbdev[64] = {};
    const char *device = getenv_default("LV_LINUX_FBDEV_DEVICE",
                                        getenv_default("APPLAUNCH_LINUX_FBDEV_DEVICE", nullptr));
    if (!device && get_lcd_fbdev(fbdev, sizeof(fbdev)) == 0) device = fbdev;
    if (!device) device = "/dev/fb0";
    lv_display_t *disp = lv_linux_fbdev_create();
    if (disp) lv_linux_fbdev_set_file(disp, device);
}
#elif LV_USE_SDL
void lv_linux_disp_init()
{
    lv_display_t *disp = lv_sdl_window_create(kScreenWidth, kScreenHeight);
    lv_sdl_window_set_title(disp, "WiFi USB Manager");
}

void lv_linux_indev_init()
{
    lv_sdl_mouse_create();
    g_keyboard_indev = lv_sdl_keyboard_create();
}
#endif

void build_ui()
{
    g_root = lv_screen_active();
    lv_obj_remove_style_all(g_root);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_root, keyboard_event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), nullptr);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_CLICKABLE);

    g_group = lv_group_create();
    lv_group_add_obj(g_group, g_root);
    lv_group_focus_obj(g_root);
    if (g_keyboard_indev) lv_indev_set_group(g_keyboard_indev, g_group);
    render();
}

}  // namespace

int main()
{
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGPIPE, SIG_IGN);

    g_adapters = wifi_manager::list_adapters();
    g_selected_adapter_idx = 0;

    lv_init();
    init_fonts();
    lv_linux_disp_init();
    LV_EVENT_KEYBOARD = lv_event_register_id();
    lv_linux_indev_init();
    build_ui();

    // Perform initial network list load on the default (USB-first) adapter
    if (!g_adapters.empty()) {
        start_async_op(AsyncOp::Scan, g_adapters[0].ifname, "", "", false,
                       "Loading networks on " + g_adapters[0].ifname + "...");
    }

    while (!g_quit_requested) {
        lv_timer_handler();
        poll_worker();
        usleep(2000);
    }

    if (g_worker_running) {
        pthread_join(g_worker_thread, nullptr);
    }
    return 0;
}
