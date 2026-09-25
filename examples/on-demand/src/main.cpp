#include "api/app_hook.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xdamage.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
    // Common Xvfb layout: read rows directly instead of 415k XGetPixel calls.
    const bool direct = image->bits_per_pixel == 32 && image->byte_order == LSBFirst &&
                        image->red_mask == 0xff0000 && image->green_mask == 0x00ff00 &&
                        image->blue_mask == 0x0000ff;
    for (unsigned y = 0; y < kOutputHeight; ++y) {
        const unsigned source_y = std::min(target.height - 1,
            static_cast<unsigned>((static_cast<uint64_t>(y) * target.height) / kOutputHeight));
        const auto* row = reinterpret_cast<const uint8_t*>(image->data) +
                          static_cast<size_t>(source_y) * image->bytes_per_line;
        for (unsigned x = 0; x < kOutputWidth; ++x) {
            const unsigned source_x = std::min(target.width - 1,
                static_cast<unsigned>((static_cast<uint64_t>(x) * target.width) / kOutputWidth));
            const size_t offset = (static_cast<size_t>(y) * kOutputWidth + x) * 3;
            if (direct) {
                const uint8_t* bgrx = row + static_cast<size_t>(source_x) * 4;
                rgb[offset] = bgrx[2];
                rgb[offset + 1] = bgrx[1];
                rgb[offset + 2] = bgrx[0];
                continue;
            }
            const unsigned long pixel = XGetPixel(image, source_x, source_y);
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

// Stick axes are little-endian 12-bit values at bytes 6..13 (LX, LY, RX, RY),
// centred near 2048. Returns -1..1 with a dead zone so a resting stick is 0.
double read_stick(const std::array<uint8_t, 128>& report, size_t axis) {
    const int raw = report[6 + axis * 2] | (report[7 + axis * 2] << 8);
    const double value = std::clamp((raw - 2048) / 1000.0, -1.0, 1.0);
    return std::abs(value) < 0.2 ? 0.0 : value;
}

// Either stick scrolls whatever is under the pointer, proportionally to deflection.
void apply_scroll(Display* dpy, const TargetWindow& target,
                  const std::array<uint8_t, 128>& report, double scroll[2]) {
    const double lx = read_stick(report, 0), ly = read_stick(report, 1);
    const double rx = read_stick(report, 2), ry = read_stick(report, 3);
    const double axes[2] = {std::abs(rx) > std::abs(lx) ? rx : lx,
                            std::abs(ry) > std::abs(ly) ? ry : ly};
    if (axes[0] == 0.0 && axes[1] == 0.0) {
        scroll[0] = scroll[1] = 0.0;
        return;
    }
    Window root_return{}, child{};
    int root_x{}, root_y{}, win_x{}, win_y{};
    unsigned mask{};
    if (XQueryPointer(dpy, target.id, &root_return, &child, &root_x, &root_y, &win_x, &win_y, &mask) &&
        (win_x < 0 || win_y < 0 || win_x >= static_cast<int>(target.width) ||
         win_y >= static_cast<int>(target.height)))
        XTestFakeMotionEvent(dpy, -1, target.x + target.width / 2, target.y + target.height / 2,
                             CurrentTime);
    // Called every 8 ms: full deflection is ~30 wheel steps per second.
    constexpr unsigned kButtons[2][2] = {{6, 7}, {5, 4}}; // {negative, positive}: left/right, down/up
    for (unsigned axis = 0; axis < 2; ++axis) {
        scroll[axis] += axes[axis] * axes[axis] * (axes[axis] < 0 ? -0.24 : 0.24);
        while (std::abs(scroll[axis]) >= 1.0) {
            const unsigned button = kButtons[axis][scroll[axis] > 0 ? 1 : 0];
            XTestFakeButtonEvent(dpy, button, True, CurrentTime);
            XTestFakeButtonEvent(dpy, button, False, CurrentTime);
            scroll[axis] += scroll[axis] > 0 ? -1.0 : 1.0;
        }
    }
}

void apply_input(Display* dpy, const TargetWindow& target,
                 const std::array<uint8_t, 128>& report,
                 uint32_t& previous_buttons, bool& previous_touch, double scroll[2]) {
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
    if (!touching) apply_scroll(dpy, target, report, scroll);
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

// Something is being held: a main-word button, a touch, or a deflected stick.
// Idle reports keep arriving, so activity is state, not the arrival of a report.
bool has_activity(const std::array<uint8_t, 128>& report) {
    if (report[2] || report[3]) return true;
    int x{}, y{};
    if (read_touch(report, x, y)) return true;
    for (size_t axis = 0; axis < 4; ++axis)
        if (read_stick(report, axis) != 0.0) return true;
    return false;
}

long env_seconds(const char* name, long fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return (*end == 0 && parsed > 0) ? parsed : fallback;
}

// Shared with the dashboard supervisor through the container's private /tmp.
constexpr const char* kParkFile = "/tmp/gamepad-park";
constexpr const char* kWakeFile = "/tmp/gamepad-wake";

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
    // Capture only when the screen changed. Damage is tracked on the root window
    // (as x11vnc does), so a relaunched or replaced browser window needs nothing
    // extra. Without the extension every frame is treated as changed.
    int damage_event{}, damage_error{};
    Damage damage = 0;
    if (XDamageQueryExtension(dpy, &damage_event, &damage_error))
        damage = XDamageCreate(dpy, root, XDamageReportNonEmpty);
    else
        std::cerr << "barista-dashboard-bridge: DAMAGE unavailable; capturing every frame\n";
    bool dirty = true, was_presenting = false;
    uint32_t previous_buttons = 0;
    bool previous_touch = false;
    double scroll[2] = {0.0, 0.0};
    bool had_target = false;
    bool had_connection = false;
    auto next_frame = Clock::now();
    auto next_search = Clock::time_point{};
    auto next_input = Clock::now();
    auto last_valid_input = Clock::time_point{};
    std::optional<TargetWindow> target;
    std::vector<uint8_t> rgb;

    // Screen timeout: after GAMEPAD_SCREEN_TIMEOUT seconds without a held button,
    // touch or stick the pad goes black (and dim, via Barista). After a further
    // GAMEPAD_PARK_AFTER seconds asleep the browser is parked: the dashboard
    // supervisor stops Chromium while this bridge keeps reading input.
    const std::chrono::seconds screen_timeout(env_seconds("GAMEPAD_SCREEN_TIMEOUT", 120));
    const std::chrono::seconds park_after(env_seconds("GAMEPAD_PARK_AFTER", 600));
    auto last_activity = Clock::now();
    bool awake = true, swallow = false, parked = false, was_connected = false;
    auto set_parked = [&parked](bool park) {
        if (park == parked) return;
        parked = park;
        if (park) {
            std::ofstream(kParkFile).put('1');
            std::cerr << "barista-dashboard-bridge: parking the browser\n";
        } else {
            std::remove(kParkFile);
            std::cerr << "barista-dashboard-bridge: browser wanted\n";
        }
    };
    std::remove(kParkFile);

    std::cerr << "barista-dashboard-bridge: waiting for Barista at " << endpoint() << '\n';
    while (true) {
        const auto now = Clock::now();
        const bool connected = hook.connected();
        if (connected != had_connection) {
            std::cerr << "barista-dashboard-bridge: AppHook " << (connected ? "connected" : "disconnected") << '\n';
            had_connection = connected;
        }
        if (now >= next_search) {
            // Remote wake (gamepadctl wake): same as a touch, minus the input.
            if (std::remove(kWakeFile) == 0 && connected) {
                last_activity = now;
                if (!awake) {
                    awake = true;
                    set_parked(false);
                    std::cerr << "barista-dashboard-bridge: screen awake (remote)\n";
                }
            }
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

        if (connected && !was_connected) {
            // A fresh session starts awake with the browser running.
            last_activity = now;
            awake = true;
            swallow = false;
            set_parked(false);
        }
        was_connected = connected;

        // Input is read whenever Barista is connected - also with the screen
        // asleep or the browser parked - so a touch can wake it.
        if (connected && now >= next_input) {
            std::array<uint8_t, 128> report{};
            if (hook.read_input(report)) {
                last_valid_input = now;
                const bool held = has_activity(report);
                if (held) last_activity = now;
                if (!awake && held) {
                    // The waking press only wakes; it must not also toggle a light.
                    awake = true;
                    swallow = true;
                    set_parked(false);
                    std::cerr << "barista-dashboard-bridge: screen awake\n";
                }
                if (swallow && !held) swallow = false;
                if (awake && !swallow && target)
                    apply_input(dpy, *target, report, previous_buttons, previous_touch, scroll);
            } else if (last_valid_input != Clock::time_point{} &&
                       now - last_valid_input > std::chrono::milliseconds(500)) {
                release_inputs(dpy, previous_buttons, previous_touch);
                last_valid_input = {};
            }
            next_input = now + std::chrono::milliseconds(8);
        }

        if (connected && awake && now - last_activity > screen_timeout) {
            awake = false;
            release_inputs(dpy, previous_buttons, previous_touch);
            // Black idle art; Barista also drops the backlight while the source is idle.
            hook.submit_rgb(std::vector<uint8_t>(kOutputWidth * kOutputHeight * 3, 0),
                            kOutputWidth, kOutputHeight, true);
            std::cerr << "barista-dashboard-bridge: screen asleep after "
                      << screen_timeout.count() << "s without input\n";
        }
        if (connected && !awake && !parked && now - last_activity > screen_timeout + park_after)
            set_parked(true);

        const bool presenting = connected && awake && target.has_value();
        hook.set_active(presenting);
        while (XPending(dpy)) {
            XEvent event;
            XNextEvent(dpy, &event);
            if (damage && event.type == damage_event + XDamageNotify) dirty = true;
        }
        // Waking replaced the picture with idle art; send the dashboard again.
        if (presenting && !was_presenting) dirty = true;
        was_presenting = presenting;
        if (presenting && now >= next_frame && (dirty || !damage)) {
            // Reset before copying, so a change during the copy triggers another.
            if (damage) XDamageSubtract(dpy, damage, None, None);
            dirty = false;
            if (capture_rgb(dpy, *target, rgb))
                hook.submit_rgb(std::move(rgb), kOutputWidth, kOutputHeight);
            // Match the GamePad's ~60 fps: at 15 fps each frame carried a large motion
            // step that the fixed-QP32 encoder smeared until later frames refined it.
            next_frame = now + std::chrono::milliseconds(16);
        }
        if (!presenting && (previous_buttons || previous_touch)) {
            release_inputs(dpy, previous_buttons, previous_touch);
            last_valid_input = {};
        }
        // Asleep or parked there is nothing to capture; input at 8 ms is plenty to wake.
        std::this_thread::sleep_for(std::chrono::milliseconds(presenting ? 2 : 8));
    }
}
