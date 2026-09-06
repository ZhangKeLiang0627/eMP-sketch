#include "hal/sketch_lvgl_hal.hpp"
#include "core/whiteboard.hpp"
#include "net/ws_server.hpp"
#include "view/whiteboard_view.hpp"

#include <lvgl.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__linux__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace {

#if defined(_WIN32)
inline void sleepMs(uint32_t ms) { Sleep(ms); }
#else
inline void sleepMs(uint32_t ms) { usleep(ms * 1000); }
#endif

std::string envOrDefaultStr(const char* name, const char* fallback)
{
    const char* v = std::getenv(name);
    return (v && v[0]) ? v : fallback;
}

// 本机第一个非回环 IPv4，用于状态栏展示（板端走 USB RNDIS / 以太网 IP 会变）
std::string localIpv4()
{
#if defined(__linux__)
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) {
        return "127.0.0.1";
    }
    std::string result = "127.0.0.1";
    for (struct ifaddrs* p = ifa; p != nullptr; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        char buf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in*>(p->ifa_addr)->sin_addr,
                  buf, sizeof(buf));
        const std::string addr(buf);
        if (addr != "127.0.0.1") {
            result = addr;
            break;
        }
    }
    freeifaddrs(ifa);
    return result;
#else
    return "localhost";
#endif
}

}  // namespace

int main()
{
    constexpr int32_t kWidth  = 480;
    constexpr int32_t kHeight = 480;

    lv_init();
    if (!sketch::initLvglHal(kWidth, kHeight)) {
        return 1;
    }

    lv_display_t* display = lv_display_get_default();
    if (!display) {
        std::fprintf(stderr, "[sketch] failed to create LVGL display\n");
        return 1;
    }

    // 网络服务：HTTP 静态托管 + WebSocket（浏览器 → 板子单向镜像）
    const int port              = std::atoi(envOrDefaultStr("SKETCH_WS_PORT", SKETCH_WS_PORT).c_str());
    const std::string web_root  = envOrDefaultStr("SKETCH_WEB_ROOT", SKETCH_WEB_ROOT);

    sketch::Whiteboard board;

    sketch::WsServer server;
    server.start(port, web_root,
                 [&board](const sketch::Stroke& s) { board.addStroke(s); },
                 [&board]() { board.clear(); },
                 [&board](const sketch::Viewport& vp) { board.setViewport(vp); },
                 [&board](bool redo) {
                     if (redo) {
                         board.redo();
                     } else {
                         board.undo();
                     }
                 },
                 [&board](uint32_t img_id, const sketch::ImageBitmap& bmp) {
                     board.cacheBitmap(img_id, bmp);
                 },
                 [&board](const sketch::ImageItem& im) { board.addImage(im); },
                 [&board](uint32_t id, const sketch::ImageItem& geo) {
                     return board.updateImage(id, geo);
                 },
                 [&board](uint32_t id) { board.removeImage(id); },
                 [&board]() { return board.snapshot(); });

    // 状态栏（ASCII only：LVGL 默认字体无中文字形）
    const std::string status = "eMP-sketch  " + localIpv4() + ":" + std::to_string(port);

    sketch::WhiteboardView view;
    sketch::UiHooks hooks;
    // 板端触摸绘制段：本地已由 view 调 addStroke，此处仅广播给浏览器（世界坐标同构）
    hooks.on_local_stroke = [&server](const sketch::Stroke& s) {
        char color[8];
        const int r = ((s.color >> 11) & 0x1F);
        const int g = ((s.color >> 5) & 0x3F);
        const int b = (s.color & 0x1F);
        std::snprintf(color, sizeof(color), "#%02X%02X%02X",
                      (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
        std::string msg = "{\"type\":\"draw\",\"id\":" + std::to_string(s.id) +
                          ",\"brush\":\"pen\",\"alpha\":255,\"color\":\"" + color +
                          "\",\"width\":" + std::to_string(s.width) + ",\"points\":[";
        for (size_t i = 0; i < s.pts.size(); ++i) {
            if (i) {
                msg += ",";
            }
            msg += std::to_string(s.pts[i]);
        }
        msg += "]}";
        server.broadcastText(msg);
    };
    // 顶栏清空：本地已由 board.clear 处理（见 on_clear hook 顺序）
    hooks.on_clear = [&board, &server]() {
        board.clear();
        server.broadcastText("{\"type\":\"clear\"}");
    };
    hooks.on_mode_changed = [](bool draw) {
        std::fprintf(stderr, "[sketch] draw mode %s\n", draw ? "ON (touch draws)" : "OFF (locked)");
    };
    view.onEnter(lv_screen_active(), board, status, hooks);

    lv_obj_invalidate(lv_screen_active());
    while (true) {
        lv_timer_handler();
        if (!lv_display_get_default()) {
            std::fprintf(stderr, "[sketch] display closed\n");
            break;
        }
        view.tick();
        sleepMs(8);
    }

    view.onExit();
    server.stop();
    sketch::shutdownLvglHal();
    return 0;
}
