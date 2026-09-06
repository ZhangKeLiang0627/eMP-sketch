#include "net/ws_server.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>

namespace sketch {
namespace {

// "#RRGGBB" → RGB565；解析失败回退墨色 #37352F
uint16_t parseColor(const std::string& hex)
{
    if (hex.size() >= 7 && hex[0] == '#') {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        const int r = nib(hex[1]) * 16 + nib(hex[2]);
        const int g = nib(hex[3]) * 16 + nib(hex[4]);
        const int b = nib(hex[5]) * 16 + nib(hex[6]);
        return rgb565(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
    }
    return rgb565(0x37, 0x35, 0x2F);
}

void handleMessage(const std::string& raw,
                   const WsServer::DrawHandler& on_draw,
                   const WsServer::ClearHandler& on_clear,
                   const WsServer::ViewportHandler& on_viewport)
{
    try {
        const auto j      = nlohmann::json::parse(raw);
        const std::string type = j.value("type", "");

        if (type == "draw") {
            Stroke s;
            s.color = parseColor(j.value("color", "#37352F"));
            s.width = static_cast<uint8_t>(j.value("width", 3));
            if (j.contains("points") && j["points"].is_array()) {
                for (const auto& p : j["points"]) {
                    if (p.is_array() && p.size() >= 2) {
                        s.pts.push_back(p[0].get<int32_t>());
                        s.pts.push_back(p[1].get<int32_t>());
                    }
                }
            }
            on_draw(s);
        } else if (type == "clear") {
            on_clear();
        } else if (type == "viewport") {
            Viewport vp;
            vp.scale    = j.value("scale", 1.0f);
            vp.offset_x = j.value("x", 0.0f);
            vp.offset_y = j.value("y", 0.0f);
            on_viewport(vp);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[sketch] bad ws message: %s\n", e.what());
    }
}

}  // namespace

WsServer::~WsServer()
{
    stop();
}

bool WsServer::start(int port,
                     const std::string& web_root,
                     DrawHandler on_draw,
                     ClearHandler on_clear,
                     ViewportHandler on_viewport)
{
    if (!_srv.set_mount_point("/", web_root)) {
        std::fprintf(stderr, "[sketch] warning: web root '%s' not servable (WS 仍可用)\n",
                     web_root.c_str());
    }

    _srv.WebSocket("/ws", [on_draw, on_clear, on_viewport](
                              const httplib::Request&, httplib::ws::WebSocket& ws) {
        std::string msg;
        while (ws.read(msg) == httplib::ws::Text) {
            handleMessage(msg, on_draw, on_clear, on_viewport);
        }
    });

    _running.store(true);
    _thread = std::thread([this, port]() {
        _srv.listen("0.0.0.0", port);
        _running.store(false);
    });
    return true;
}

void WsServer::stop()
{
    _running.store(false);
    _srv.stop();
    if (_thread.joinable()) {
        _thread.join();
    }
}

}  // namespace sketch
