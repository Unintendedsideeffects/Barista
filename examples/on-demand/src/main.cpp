#include "api/app_hook.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
XErrorHandler previous_x_error_handler = nullptr;

int tolerate_disappearing_windows(Display* display, XErrorEvent* event) {
    if (event->error_code == BadWindow || event->error_code == BadDrawable) return 0;
    return previous_x_error_handler ? previous_x_error_handler(display, event) : 0;
}

using Clock = std::chrono::steady_clock;
constexpr unsigned kOutputWidth = 864;
constexpr unsigned kOutputHeight = 480;
constexpr uint32_t kA = 0x8000;
constexpr uint32_t kB = 0x4000;
constexpr uint32_t kLeft = 0x0800;
constexpr uint32_t kRight = 0x0400;
constexpr uint32_t kUp = 0x0200;
constexpr uint32_t kDown = 0x0100;

struct TargetWindow {
    Window id{};
    int x{};
    int y{};
    unsigned width{};
    unsigned height{};
    std::string title;
};

std::string get_title(Display* dpy, Window window) {
    const Atom name = XInternAtom(dpy, "_NET_WM_NAME", True);
    const Atom utf8 = XInternAtom(dpy, "UTF8_STRING", True);
    if (name != None && utf8 != None) {
        Atom type{};
        int format{};
        unsigned long count{};
        unsigned long remaining{};
        unsigned char* value = nullptr;
        if (XGetWindowProperty(dpy, window, name, 0, 1024, False, utf8,
                               &type, &format, &count, &remaining, &value) == Success &&
            value && format == 8) {
            std::string result(reinterpret_cast<char*>(value), count);
            XFree(value);
            if (!result.empty()) return result;
        } else if (value) {
            XFree(value);
        }
    }

    char* legacy = nullptr;
    if (XFetchName(dpy, window, &legacy) && legacy) {
        std::string result(legacy);
        XFree(legacy);
        return result;
    }
    return {};
}

void collect_windows(Display* dpy, Window parent, unsigned depth,
                     std::vector<Window>& windows) {
    if (depth > 5) return;
    Window root{};
    Window parent_return{};
    Window* children = nullptr;
    unsigned count{};
    if (!XQueryTree(dpy, parent, &root, &parent_return, &children, &count)) return;
    for (unsigned i = 0; i < count; ++i) {
        windows.push_back(children[i]);
        collect_windows(dpy, children[i], depth + 1, windows);
    }
    if (children) XFree(children);
}

std::optional<TargetWindow> find_dashboard_window(Display* dpy, Window root) {
    std::vector<Window> windows;
    collect_windows(dpy, root, 0, windows);
    std::optional<TargetWindow> best;
    int best_score = 0;
    for (Window window : windows) {
        XWindowAttributes attr{};
        if (!XGetWindowAttributes(dpy, window, &attr) || attr.map_state != IsViewable ||
            attr.width < 320 || attr.height < 200 || attr.width > 8192 || attr.height > 8192)
            continue;
        std::string title = get_title(dpy, window);
        std::string lower = title;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        int score = 0;
        if (lower.find("home assistant") != std::string::npos) score = 3;
        else if (lower.find("chromium") != std::string::npos) score = 1;
        if (score <= best_score) continue;
        int root_x{};
        int root_y{};
        Window child{};
        if (!XTranslateCoordinates(dpy, window, root, 0, 0, &root_x, &root_y, &child)) continue;
        best = TargetWindow{window, root_x, root_y,
                            static_cast<unsigned>(attr.width),
                            static_cast<unsigned>(attr.height), title};
        best_score = score;
    }
    return best;
}

unsigned channel(unsigned long pixel, unsigned long mask) {
    if (!mask) return 0;
    unsigned shift = 0;
    while (((mask >> shift) & 1UL) == 0UL) ++shift;
    const unsigned long maximum = mask >> shift;
    return static_cast<unsigned>(((pixel & mask) >> shift) * 255UL / maximum);
}

bool capture_rgb(Display* dpy, const TargetWindow& target, std::vector<uint8_t>& rgb) {
    XImage* image = XGetImage(dpy, target.id, 0, 0, target.width, target.height,
                              AllPlanes, ZPixmap);
    if (!image) return false;
    rgb.resize(static_cast<size_t>(kOutputWidth) * kOutputHeight * 3);
    for (unsigned y = 0; y < kOutputHeight; ++y) {
        const unsigned source_y = std::min(target.height - 1,
            static_cast<unsigned>((static_cast<uint64_t>(y) * target.height) / kOutputHeight));
        for (unsigned x = 0; x < kOutputWidth; ++x) {
            const unsigned source_x = std::min(target.width - 1,
                static_cast<unsigned>((static_cast<uint64_t>(x) * target.width) / kOutputWidth));
            const unsigned long pixel = XGetPixel(image, source_x, source_y);
            const size_t offset = (static_cast<size_t>(y) * kOutputWidth + x) * 3;
            rgb[offset] = static_cast<uint8_t>(channel(pixel, image->red_mask));
            rgb[offset + 1] = static_cast<uint8_t>(channel(pixel, image->green_mask));
            rgb[offset + 2] = static_cast<uint8_t>(channel(pixel, image->blue_mask));
        }
    }
    XDestroyImage(image);
    return true;
}

uint32_t read_buttons(const std::array<uint8_t, 128>& report) {
    return (uint32_t(report[80]) << 16) | (uint32_t(report[2]) << 8) | report[3];
}

bool read_touch(const std::array<uint8_t, 128>& report, int& x, int& y) {
    int raw_x = 0;
    int raw_y = 0;
    for (size_t point = 0; point < 10; ++point) {
        const size_t base = 36 + point * 4;
        raw_x += ((report[base + 1] & 0x0f) << 8) | report[base];
        raw_y += ((report[base + 3] & 0x0f) << 8) | report[base + 2];
    }
    raw_x /= 10;
    raw_y /= 10;
    int pressure = 0;
    for (size_t point = 0; point < 4; ++point)
        pressure |= ((report[37 + point * 4] >> 4) & 7) << (point * 3);
    if (pressure == 0) return false;

    // Barista's GamePad calibration maps raw touch samples onto 854x480.
    x = std::clamp(20 + (raw_x - 195) * (834 - 20) / (3877 - 195), 0, 853);
    y = std::clamp(20 + (raw_y - 3818) * (460 - 20) / (373 - 3818), 0, 479);
    return true;
}

void set_key(Display* dpy, KeySym symbol, bool before, bool after) {
    if (before == after) return;
    const KeyCode code = XKeysymToKeycode(dpy, symbol);
    if (code != 0) XTestFakeKeyEvent(dpy, code, after ? True : False, CurrentTime);
}

void apply_input(Display* dpy, const TargetWindow& target,
                 const std::array<uint8_t, 128>& report,
                 uint32_t& previous_buttons, bool& previous_touch) {
    const uint32_t buttons = read_buttons(report);
    set_key(dpy, XK_Return, previous_buttons & kA, buttons & kA);
    set_key(dpy, XK_Escape, previous_buttons & kB, buttons & kB);
    set_key(dpy, XK_Left, previous_buttons & kLeft, buttons & kLeft);
    set_key(dpy, XK_Right, previous_buttons & kRight, buttons & kRight);
    set_key(dpy, XK_Up, previous_buttons & kUp, buttons & kUp);
    set_key(dpy, XK_Down, previous_buttons & kDown, buttons & kDown);
    previous_buttons = buttons;

    int touch_x{};
    int touch_y{};
    const bool touching = read_touch(report, touch_x, touch_y);
    if (touching) {
        const int px = target.x + std::clamp(touch_x * static_cast<int>(target.width) / 854,
                                             0, static_cast<int>(target.width) - 1);
        const int py = target.y + std::clamp(touch_y * static_cast<int>(target.height) / 480,
                                             0, static_cast<int>(target.height) - 1);
        XTestFakeMotionEvent(dpy, -1, px, py, CurrentTime);
    }
    if (touching != previous_touch)
        XTestFakeButtonEvent(dpy, 1, touching ? True : False, CurrentTime);
    previous_touch = touching;
    XFlush(dpy);
}

void release_inputs(Display* dpy, uint32_t& previous_buttons, bool& previous_touch) {
    set_key(dpy, XK_Return, previous_buttons & kA, false);
    set_key(dpy, XK_Escape, previous_buttons & kB, false);
    set_key(dpy, XK_Left, previous_buttons & kLeft, false);
    set_key(dpy, XK_Right, previous_buttons & kRight, false);
    set_key(dpy, XK_Up, previous_buttons & kUp, false);
    set_key(dpy, XK_Down, previous_buttons & kDown, false);
    if (previous_touch) XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    previous_buttons = 0;
    previous_touch = false;
    XFlush(dpy);
}

std::string endpoint() {
    if (const char* configured = std::getenv("BARISTA_MUG_SOCKET")) return configured;
    return "/run/barista/media-" + std::to_string(getuid()) + ".sock";
}
} // namespace

int main() {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::cerr << "barista-dashboard-bridge: cannot connect to the X11 display\n";
        return 1;
    }
    previous_x_error_handler = XSetErrorHandler(tolerate_disappearing_windows);
    int event_base{}, error_base{}, major{}, minor{};
    if (!XTestQueryExtension(dpy, &event_base, &error_base, &major, &minor)) {
        std::cerr << "barista-dashboard-bridge: XTest input extension is unavailable\n";
        XCloseDisplay(dpy);
        return 1;
    }

    barista::api::AppHook hook(false);
    std::string error;
    if (!hook.start(endpoint(), error)) {
        std::cerr << "barista-dashboard-bridge: AppHook start failed: " << error << '\n';
        XCloseDisplay(dpy);
        return 1;
    }
    hook.set_active(false);

    const Window root = DefaultRootWindow(dpy);
    uint32_t previous_buttons = 0;
    bool previous_touch = false;
    bool had_target = false;
    bool had_connection = false;
    auto next_frame = Clock::now();
    auto next_search = Clock::time_point{};
    auto next_input = Clock::now();
    auto last_valid_input = Clock::time_point{};
    std::optional<TargetWindow> target;
    std::vector<uint8_t> rgb;

    std::cerr << "barista-dashboard-bridge: waiting for Barista at " << endpoint() << '\n';
    while (true) {
        const auto now = Clock::now();
        const bool connected = hook.connected();
        if (connected != had_connection) {
            std::cerr << "barista-dashboard-bridge: AppHook " << (connected ? "connected" : "disconnected") << '\n';
            had_connection = connected;
        }
        if (now >= next_search) {
            target = find_dashboard_window(dpy, root);
            next_search = now + std::chrono::milliseconds(250);
            const bool found = target.has_value();
            if (found != had_target) {
                std::cerr << "barista-dashboard-bridge: Home Assistant window "
                          << (found ? "found" : "not found") << '\n';
                had_target = found;
            }
            if (connected && target) {
                XRaiseWindow(dpy, target->id);
                XSetInputFocus(dpy, target->id, RevertToParent, CurrentTime);
            }
        }

        const bool presenting = connected && target.has_value();
        hook.set_active(presenting);
        if (presenting && now >= next_frame) {
            if (capture_rgb(dpy, *target, rgb))
                hook.submit_rgb(std::move(rgb), kOutputWidth, kOutputHeight);
            next_frame = now + std::chrono::milliseconds(66);
        }

        if (presenting && now >= next_input) {
            std::array<uint8_t, 128> report{};
            if (hook.read_input(report)) {
                apply_input(dpy, *target, report, previous_buttons, previous_touch);
                last_valid_input = now;
            } else if (last_valid_input != Clock::time_point{} &&
                       now - last_valid_input > std::chrono::milliseconds(500)) {
                release_inputs(dpy, previous_buttons, previous_touch);
                last_valid_input = {};
            }
            next_input = now + std::chrono::milliseconds(8);
        } else if (!presenting && (previous_buttons || previous_touch)) {
            release_inputs(dpy, previous_buttons, previous_touch);
            last_valid_input = {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
