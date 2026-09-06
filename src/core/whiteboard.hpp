#pragma once

#include "core/sketch_types.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace sketch {

// 白板：场景图（统一对象列表：笔画/图片，v0.2）+ 视口 + 软件渲染器。
// 网络线程通过 addStroke/addImage/updateImage/removeImage/undo/redo/clear/setViewport
// 写入，主线程通过 renderIfDirty 消费。
class Whiteboard {
public:
    Whiteboard();

    // ---- 网络线程（线程安全）----
    // 笔画：id==0 追加新笔；id!=0 且与末笔同 id 时把增量段合并进该笔
    void addStroke(const Stroke& stroke);
    // 图片
    void cacheBitmap(uint32_t img_id, const ImageBitmap& bmp);   // img-data
    void addImage(const ImageItem& image);                        // 新图片对象（入撤销历史）
    bool updateImage(uint32_t id, const ImageItem& geo);          // 几何变更（不入历史）
    void removeImage(uint32_t id);                                // 删除（入撤销历史）
    // 通用
    void undo();
    void redo();
    void clear();
    void setViewport(const Viewport& vp);

    // ---- 主线程 ----
    // 有脏帧时重新渲染到内部 RGB565 帧缓冲，返回 true 表示产生新帧
    bool renderIfDirty();
    const uint16_t* framebuffer() const { return _fb.data(); }
    const Viewport& viewport() const { return _vp; }

    // ---- 快照（新连接/刷新恢复用，线程安全）----
    struct BoardSnapshot {
        Viewport vp;
        std::vector<SceneItem> items;                    // 按绘制顺序
        std::map<uint32_t, ImageBitmap> bitmaps;         // 场景图片引用的位图
        size_t undo_depth = 0;                           // 撤销历史深度（供浏览器刷新后回退）
        size_t redo_depth = 0;
    };
    BoardSnapshot snapshot() const;

    static constexpr int32_t width()  { return kScreenWidth; }
    static constexpr int32_t height() { return kScreenHeight; }

private:
    // 历史条目：undo/redo 对称。
    // AddStroke 正向=添加/反向=按 id 移除；AddImage/RemoveImage 同理按对象整体；
    // ClearBoard 正向=清空/反向=恢复快照。
    struct HistoryEntry {
        enum class Op : uint8_t {
            AddStroke,
            AddImage,
            RemoveImage,
            ClearBoard,
        } op = Op::AddStroke;
        Stroke stroke;                    // AddStroke
        ImageItem image;                  // AddImage / RemoveImage
        std::vector<SceneItem> snapshot;  // ClearBoard：清空前的全部对象
    };

    void renderLocked();
    void fill(uint16_t color);
    void setPixel(int32_t x, int32_t y, uint16_t color);
    void blendPixel(int32_t x, int32_t y, uint16_t color, uint8_t alpha);
    void drawStrokeLocked(const Stroke& s);
    void drawImageLocked(const ImageItem& img, const ImageBitmap& bmp);
    void pushHistory(const HistoryEntry& e);
    void setDirty() { _dirty = true; }

    // 调用方需已持锁
    SceneItem* findItemLocked(uint32_t id);
    void removeItemLocked(uint32_t id);

    mutable std::mutex _mutex;
    std::vector<SceneItem> _items;                 // 按绘制顺序
    std::map<uint32_t, ImageBitmap> _bitmaps;      // img-data 位图缓存
    std::vector<HistoryEntry> _undo_stack;
    std::vector<HistoryEntry> _redo_stack;
    Viewport _vp;
    std::vector<uint16_t> _fb;
    bool _dirty = true;
};

}  // namespace sketch
