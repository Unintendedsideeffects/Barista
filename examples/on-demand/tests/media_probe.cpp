#include "api/app_hook.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "/run/barista/media-1000.sock";
    barista::api::AppHook server(true);
    std::string error;
    if (!server.start(path, error)) {
        std::cerr << "AppHook probe could not listen: " << error << '\n';
        return 1;
    }
    std::vector<uint8_t> frame(barista::api::FrameBytes);
    unsigned frames = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    while (std::chrono::steady_clock::now() < deadline) {
        bool active = false;
        if (server.connected() && server.read_video(frame, active) && active) {
            const auto [low, high] = std::minmax_element(
                frame.begin(), frame.begin() + barista::api::Width * barista::api::Height);
            if (*high - *low > 20 && ++frames >= 3) {
                const auto app = server.connected_app();
                if (app.uid != 1000) return 2;
                std::cout << "AppHook received nonuniform dashboard frames: uid=" << app.uid
                          << " bytes=" << frame.size() << " frames=" << frames << '\n';
                return 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cerr << "No dashboard frames received: " << server.rejection_reason() << '\n';
    return 3;
}
