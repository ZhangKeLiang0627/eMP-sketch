#pragma once

#include "core/sketch_types.hpp"
#include "core/whiteboard.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include <httplib.h>

namespace sketch {

// HTTP + WebSocket 服务：托管网页静态资源，接收浏览器下发的笔画/视口/清空/撤销消息。
// v0.1 单向镜像：浏览器 → 板子（板子只接收，不回发）。
class WsServer {
public:
    using DrawHandler      = std::function<void(const Stroke&)>;
    using ClearHandler     = std::function<void()>;
    using ViewportHandler  = std::function<void(const Viewport&)>;
    using UndoHandler      = std::function<void(bool /*redo*/)>;
    using ImageDataHandler = std::function<void(uint32_t /*img_id*/, const ImageBitmap&)>;
    using ImageAddHandler  = std::function<void(const ImageItem&)>;
    using ImageGeoHandler  = std::function<bool(uint32_t /*id*/, const ImageItem& /*geo*/)>;
    using ImageRemoveHandler = std::function<void(uint32_t /*id*/)>;
    using SnapshotProvider = std::function<Whiteboard::BoardSnapshot()>;

    WsServer()  = default;
    ~WsServer();

    WsServer(const WsServer&)            = delete;
    WsServer& operator=(const WsServer&) = delete;

    // web_root：静态资源目录（可用 SKETCH_WEB_ROOT 环境变量覆盖编译期默认值）
    bool start(int port,
               const std::string& web_root,
               DrawHandler on_draw,
               ClearHandler on_clear,
               ViewportHandler on_viewport,
               UndoHandler on_undo,
               ImageDataHandler on_img_data,
               ImageAddHandler on_img_add,
               ImageGeoHandler on_img_update,
               ImageRemoveHandler on_img_remove,
               SnapshotProvider on_snapshot);
    void stop();
    bool running() const { return _running.load(); }

private:
    httplib::Server _srv;
    std::thread _thread;
    std::atomic<bool> _running{false};
};

}  // namespace sketch
