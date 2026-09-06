#pragma once

#include "core/sketch_types.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace sketch {

// 白板：场景图（v0.2 笔画对象，支持同 id 增量段合并）+ 视口 + 软件渲染器。
// 网络线程通过 addStroke/undo/redo/clear/setViewport 写入，主线程通过 renderIfDirty 消费。
class Whiteboard {
public:
    Whiteboard();

    // ---- 网络线程（线程安全）----
    // id==0：追加为新笔画；id!=0 且与末笔同 id：把该段合并进末笔（增量上屏的段合并）
    void addStroke(const Stroke& stroke);
    void undo();   // 撤销最近一次"添加笔画"或"清空"
    void redo();   // 重做
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
    // 历史条目：undo/redo 对称（AddStroke 的正向=添加，反向=按 id 移除；
    // ClearBoard 的正向=清空，反向=恢复快照）
    struct HistoryEntry {
        enum class Op : uint8_t { AddStroke, ClearBoard } op = Op::AddStroke;
        Stroke stroke;                 // AddStroke
        std::vector<Stroke> snapshot;  // ClearBoard：清空前的全部笔画
    };

    void renderLocked();
    void fill(uint16_t color);
    void setPixel(int32_t x, int32_t y, uint16_t color);
    void blendPixel(int32_t x, int32_t y, uint16_t color, uint8_t alpha);
    void drawStrokeLocked(const Stroke& s);
    void pushHistory(const HistoryEntry& e);
    void setDirty() { _dirty = true; }

    // 调用方需已持锁
    Stroke* findStrokeLocked(uint32_t id);
    void removeStrokeLocked(uint32_t id);

    mutable std::mutex _mutex;
    std::vector<Stroke> _strokes;          // 按绘制顺序
    std::vector<HistoryEntry> _undo_stack;
    std::vector<HistoryEntry> _redo_stack;
    Viewport _vp;
    std::vector<uint16_t> _fb;
    bool _dirty = true;
};

}  // namespace sketch
