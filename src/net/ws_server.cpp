#include "net/ws_server.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace sketch {
namespace {

// base64 → 原始字节（标准表，忽略空白）；失败返回 false
bool decodeBase64(const std::string& in, std::vector<uint8_t>& out)
{
    static const signed char kDec[256] = {
        /* 由 init 填充，见下 */
    };
    // 运行时建表（避免大静态初始化表）
    static signed char tbl[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) {
            tbl[i] = -1;
        }
        const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; alphabet[i]; ++i) {
            tbl[static_cast<uint8_t>(alphabet[i])] = static_cast<signed char>(i);
        }
        init = true;
    }
    (void)kDec;

    out.clear();
    out.reserve((in.size() / 4) * 3);
    int val = 0;
    int bits = 0;
    for (char ch : in) {
        if (ch == '=' || ch == '\n' || ch == '\r') {
            continue;
        }
        const signed char d = tbl[static_cast<uint8_t>(ch)];
        if (d < 0) {
            return false;
        }
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
        }
    }
    return true;
}

// 把 base64 的 RGB565 字节流填入 ImageBitmap
bool parseBitmap(const std::string& data_b64, uint32_t w, uint32_t h, ImageBitmap& bmp)
{
    std::vector<uint8_t> bytes;
    if (!decodeBase64(data_b64, bytes)) {
        return false;
    }
    if (bytes.size() != static_cast<size_t>(w) * h * 2) {
        return false;
    }
    ImageBitmap out;
    out.w = w;
    out.h = h;
    out.px.resize(static_cast<size_t>(w) * h);
    // 字节序 LE：低字节在前（x86/ARM 均小端）
    for (size_t i = 0; i < out.px.size(); ++i) {
        out.px[i] = static_cast<uint16_t>(bytes[i * 2]) |
                    (static_cast<uint16_t>(bytes[i * 2 + 1]) << 8);
    }
    bmp = std::move(out);
    return true;
}

// base64 编码（用于快照回发位图）
std::string encodeBase64(const uint8_t* data, size_t len)
{
    static const char* kTab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kTab[(v >> 18) & 63]);
        out.push_back(kTab[(v >> 12) & 63]);
        out.push_back(kTab[(v >> 6) & 63]);
        out.push_back(kTab[v & 63]);
        i += 3;
    }
    const size_t rest = len - i;
    if (rest == 1) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kTab[(v >> 18) & 63]);
        out.push_back(kTab[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rest == 2) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kTab[(v >> 18) & 63]);
        out.push_back(kTab[(v >> 12) & 63]);
        out.push_back(kTab[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

// RGB565 → "#RRGGBB"
std::string colorHex(uint16_t c)
{
    const int r = ((c >> 11) & 0x1F);
    const int g = ((c >> 5) & 0x3F);
    const int b = (c & 0x1F);
    const int r8 = (r << 3) | (r >> 2);
    const int g8 = (g << 2) | (g >> 4);
    const int b8 = (b << 3) | (b >> 2);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r8, g8, b8);
    return buf;
}

const char* brushName(BrushKind k)
{
    switch (k) {
        case BrushKind::Marker:      return "marker";
        case BrushKind::Highlighter: return "highlighter";
        case BrushKind::Eraser:      return "eraser";
        default:                     return "pen";
    }
}

// 白板快照 → 浏览器恢复用的 snapshot 消息
std::string buildSnapshotReply(const Whiteboard::BoardSnapshot& snap)
{
    nlohmann::json out;
    out["type"] = "snapshot";
    out["vp"]   = {{"scale", snap.vp.scale}, {"x", snap.vp.offset_x}, {"y", snap.vp.offset_y}};
    nlohmann::json items = nlohmann::json::array();
    for (const SceneItem& it : snap.items) {
        if (it.kind == SceneItem::Kind::Stroke) {
            const Stroke& s = it.stroke;
            items.push_back({{"k", 0},
                             {"id", it.id},
                             {"color", colorHex(s.color)},
                             {"brush", brushName(s.brush)},
                             {"alpha", s.alpha},
                             {"width", s.width},
                             {"pts", s.pts}});
        } else {
            const ImageItem& im = it.image;
            items.push_back({{"k", 1},
                             {"id", it.id},
                             {"img", im.img},
                             {"cx", im.cx},
                             {"cy", im.cy},
                             {"w", im.w},
                             {"h", im.h},
                             {"rot", im.rot}});
        }
    }
    nlohmann::json imgs = nlohmann::json::array();
    for (const auto& kv : snap.bitmaps) {
        const ImageBitmap& bmp = kv.second;
        imgs.push_back({{"img", kv.first},
                        {"w", bmp.w},
                        {"h", bmp.h},
                        {"data", encodeBase64(
                             reinterpret_cast<const uint8_t*>(bmp.px.data()),
                             bmp.px.size() * sizeof(uint16_t))}});
    }
    out["items"] = items;
    out["imgs"]  = imgs;
    out["undo"]  = snap.undo_depth;
    out["redo"]  = snap.redo_depth;
    return out.dump();
}

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

// "pen"/"marker"/"highlighter"/"eraser" → BrushKind；未知回退 Pen
BrushKind parseBrush(const std::string& name)
{
    if (name == "marker") {
        return BrushKind::Marker;
    }
    if (name == "highlighter") {
        return BrushKind::Highlighter;
    }
    if (name == "eraser") {
        return BrushKind::Eraser;
    }
    return BrushKind::Pen;
}

void handleMessage(const std::string& raw,
                   const WsServer::DrawHandler& on_draw,
                   const WsServer::ClearHandler& on_clear,
                   const WsServer::ViewportHandler& on_viewport,
                   const WsServer::UndoHandler& on_undo,
                   const WsServer::ImageDataHandler& on_img_data,
                   const WsServer::ImageAddHandler& on_img_add,
                   const WsServer::ImageGeoHandler& on_img_update,
                   const WsServer::ImageRemoveHandler& on_img_remove)
{
    try {
        const auto j      = nlohmann::json::parse(raw);
        const std::string type = j.value("type", "");

        if (type == "draw") {
            Stroke s;
            s.id    = j.value("id", static_cast<uint32_t>(0));
            s.brush = parseBrush(j.value("brush", std::string("pen")));
            s.alpha = static_cast<uint8_t>(j.value("alpha", 255));
            s.color = parseColor(j.value("color", "#37352F"));
            s.width = static_cast<uint8_t>(j.value("width", 3));
            if (j.contains("points") && j["points"].is_array()) {
                const auto& pts = j["points"];
                // 兼容两种点格式：扁平 [x0,y0,x1,y1,...]（paint.js 实发，板端渲染同构）
                // 与嵌套 [[x,y],[x,y],...]（README v0.1 早期文档格式）
                if (!pts.empty() && pts.front().is_array()) {
                    for (const auto& p : pts) {
                        if (p.is_array() && p.size() >= 2) {
                            s.pts.push_back(p[0].get<int32_t>());
                            s.pts.push_back(p[1].get<int32_t>());
                        }
                    }
                } else {
                    for (const auto& p : pts) {
                        if (p.is_number()) {
                            s.pts.push_back(p.get<int32_t>());
                        }
                    }
                    if (s.pts.size() % 2 != 0) {
                        s.pts.pop_back();  // 奇数尾点不成对，丢弃
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
        } else if (type == "undo") {
            on_undo(false);
        } else if (type == "redo") {
            on_undo(true);
        } else if (type == "img-data") {
            const uint32_t img_id = j.value("img", static_cast<uint32_t>(0));
            const uint32_t w = j.value("w", static_cast<uint32_t>(0));
            const uint32_t h = j.value("h", static_cast<uint32_t>(0));
            ImageBitmap bmp;
            if (parseBitmap(j.value("data", std::string()), w, h, bmp)) {
                on_img_data(img_id, bmp);
            } else {
                std::fprintf(stderr, "[sketch] bad img-data (w=%u h=%u)\n", w, h);
            }
        } else if (type == "img") {
            ImageItem im;
            im.id  = j.value("id", static_cast<uint32_t>(0));
            im.img = j.value("img", static_cast<uint32_t>(0));
            im.cx  = j.value("cx", 0.0f);
            im.cy  = j.value("cy", 0.0f);
            im.w   = j.value("w", 0.0f);
            im.h   = j.value("h", 0.0f);
            im.rot = j.value("rot", 0.0f);
            on_img_add(im);
        } else if (type == "img-update") {
            ImageItem im;
            im.id  = j.value("id", static_cast<uint32_t>(0));
            im.img = j.value("img", static_cast<uint32_t>(0));
            im.cx  = j.value("cx", 0.0f);
            im.cy  = j.value("cy", 0.0f);
            im.w   = j.value("w", 0.0f);
            im.h   = j.value("h", 0.0f);
            im.rot = j.value("rot", 0.0f);
            on_img_update(im.id, im);
        } else if (type == "img-remove") {
            on_img_remove(j.value("id", static_cast<uint32_t>(0)));
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
                     ViewportHandler on_viewport,
                     UndoHandler on_undo,
                     ImageDataHandler on_img_data,
                     ImageAddHandler on_img_add,
                     ImageGeoHandler on_img_update,
                     ImageRemoveHandler on_img_remove,
                     SnapshotProvider on_snapshot)
{
    if (!_srv.set_mount_point("/", web_root)) {
        std::fprintf(stderr, "[sketch] warning: web root '%s' not servable (WS 仍可用)\n",
                     web_root.c_str());
    }

    _srv.WebSocket("/ws", [this, on_draw, on_clear, on_viewport, on_undo, on_img_data, on_img_add,
                           on_img_update, on_img_remove, on_snapshot](
                              const httplib::Request&, httplib::ws::WebSocket& ws) {
        {
            std::lock_guard<std::mutex> lock(_client_mutex);
            _clients.push_back(&ws);
        }
        std::string msg;
        while (ws.read(msg) == httplib::ws::Text) {
            // 快照请求：回复当前场景（页面刷新后的恢复）
            if (on_snapshot) {
                try {
                    const auto j = nlohmann::json::parse(msg);
                    if (j.value("type", std::string()) == "sync") {
                        ws.send(buildSnapshotReply(on_snapshot()));
                        continue;
                    }
                } catch (...) {
                    // 非 JSON / 解析失败 → 走常规消息处理（会再报一次错，无妨）
                }
            }
            handleMessage(msg, on_draw, on_clear, on_viewport, on_undo, on_img_data,
                          on_img_add, on_img_update, on_img_remove);
        }
        {
            std::lock_guard<std::mutex> lock(_client_mutex);
            _clients.erase(std::remove(_clients.begin(), _clients.end(), &ws), _clients.end());
        }
    });

    _running.store(true);
    _thread = std::thread([this, port]() {
        _srv.listen("0.0.0.0", port);
        _running.store(false);
    });
    return true;
}

void WsServer::broadcastText(const std::string& text)
{
    std::lock_guard<std::mutex> lock(_client_mutex);
    for (httplib::ws::WebSocket* ws : _clients) {
        if (ws != nullptr) {
            ws->send(text);
        }
    }
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
