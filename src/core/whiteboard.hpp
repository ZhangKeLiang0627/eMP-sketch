#pragma once

#include "core/sketch_types.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace sketch {

// 白板：场景图（v0.1 仅笔画）+ 视口 + 软件渲染器。
// 网络线程通过 addStroke/clear/setViewport 写入，主线程通过 renderIfDirty 消费。
class Whiteboard {
public:
    Whiteboard();

    // ---- 网络线程（线程安全）----
    void addStroke(const Stroke& stroke);
    void clear();
    void setViewport(const Viewport& vp);

    // ---- 主线程 ----
    // 有脏帧时重新渲染到内部 RGB565 帧缓冲，返回 true 表示产生新帧
    bool renderIfDirty();
    const uint16_t* framebuffer() const { return _fb.data(); }
    const Viewport& viewport() const { return _vp; }

    static constexpr int32_t width()  { return kScreenWidth; }
    static constexpr int32_t height() { return kScreenHeight; }

private:
    void renderLocked();
    void fill(uint16_t color);
    void setPixel(int32_t x, int32_t y, uint16_t color);
    void drawStrokeLocked(const Stroke& s);

    mutable std::mutex _mutex;
    std::vector<Stroke> _strokes;
    Viewport _vp;
    std::vector<uint16_t> _fb;
    bool _dirty = true;
};

}  // namespace sketch
