#include "core/whiteboard.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sketch {
namespace {

constexpr uint16_t kPaperColor = rgb565(kPaperR, kPaperG, kPaperB);

// RGB565 → 每通道近似 8bit（用于 alpha 合成）
inline uint8_t r5(uint16_t c) { return static_cast<uint8_t>(((c >> 11) & 0x1F) << 3); }
inline uint8_t g6(uint16_t c) { return static_cast<uint8_t>(((c >> 5) & 0x3F) << 2); }
inline uint8_t b5(uint16_t c) { return static_cast<uint8_t>((c & 0x1F) << 3); }

}  // namespace

Whiteboard::Whiteboard()
{
    _fb.resize(static_cast<size_t>(kScreenWidth) * kScreenHeight, kPaperColor);
}

void Whiteboard::cacheBitmap(uint32_t img_id, const ImageBitmap& bmp)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!bmp.valid()) {
        return;
    }
    _bitmaps[img_id] = bmp;
    setDirty();  // 引用该图的图片对象可能已存在，重绘一次
}

void Whiteboard::addStroke(const Stroke& stroke)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!stroke.hasPoints()) {
        return;
    }

    // 同 id 增量段合并到末笔（浏览器把一笔拆成多段消息推送）
    if (stroke.id != 0 && !_items.empty()) {
        SceneItem& last = _items.back();
        if (last.kind == SceneItem::Kind::Stroke && last.stroke.id == stroke.id) {
            // 追加段共享首点（浏览器发 [上一已发点,当前点]），跳过重复点对
            std::vector<int32_t>& pts = last.stroke.pts;
            const size_t n = pts.size();
            if (n >= 2 && stroke.pts.size() >= 2 &&
                pts[n - 2] == stroke.pts[0] && pts[n - 1] == stroke.pts[1]) {
                pts.insert(pts.end(), stroke.pts.begin() + 2, stroke.pts.end());
            } else {
                pts.insert(pts.end(), stroke.pts.begin(), stroke.pts.end());
            }
            setDirty();
            return;
        }
    }

    SceneItem item;
    item.kind   = SceneItem::Kind::Stroke;
    item.id     = stroke.id != 0 ? stroke.id : static_cast<uint32_t>(_items.size() + 1);
    item.stroke = stroke;
    _items.push_back(item);
    pushHistory(HistoryEntry{HistoryEntry::Op::AddStroke, stroke, {}, {}});
    setDirty();
}

void Whiteboard::addImage(const ImageItem& image)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!image.valid()) {
        return;
    }
    SceneItem item;
    item.kind  = SceneItem::Kind::Image;
    item.id    = image.id != 0 ? image.id : static_cast<uint32_t>(_items.size() + 1);
    item.image = image;
    _items.push_back(item);
    pushHistory(HistoryEntry{HistoryEntry::Op::AddImage, {}, item.image, {}});
    setDirty();
}

bool Whiteboard::updateImage(uint32_t id, const ImageItem& geo)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (id == 0 || !geo.valid()) {
        return false;
    }
    SceneItem* item = findItemLocked(id);
    if (item == nullptr || item->kind != SceneItem::Kind::Image) {
        return false;
    }
    item->image = geo;
    setDirty();
    return true;
}

void Whiteboard::removeImage(uint32_t id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    SceneItem* item = findItemLocked(id);
    if (item == nullptr || item->kind != SceneItem::Kind::Image) {
        return;
    }
    pushHistory(HistoryEntry{HistoryEntry::Op::RemoveImage, {}, item->image, {}});
    removeItemLocked(id);
    setDirty();
}

void Whiteboard::undo()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_undo_stack.empty()) {
        return;
    }
    HistoryEntry e = _undo_stack.back();
    _undo_stack.pop_back();

    switch (e.op) {
        case HistoryEntry::Op::AddStroke: {
            // 移除完整末笔（含增量段合并后的全部点）
            SceneItem* cur = findItemLocked(e.stroke.id);
            if (cur != nullptr && cur->kind == SceneItem::Kind::Stroke) {
                HistoryEntry redo_entry;
                redo_entry.op     = HistoryEntry::Op::AddStroke;
                redo_entry.stroke = cur->stroke;
                _redo_stack.push_back(redo_entry);
                removeItemLocked(e.stroke.id);
            }
            break;
        }
        case HistoryEntry::Op::AddImage: {
            SceneItem* cur = findItemLocked(e.image.id);
            if (cur != nullptr && cur->kind == SceneItem::Kind::Image) {
                HistoryEntry redo_entry;
                redo_entry.op    = HistoryEntry::Op::AddImage;
                redo_entry.image = cur->image;
                _redo_stack.push_back(redo_entry);
                removeItemLocked(e.image.id);
            }
            break;
        }
        case HistoryEntry::Op::RemoveImage: {
            // 恢复被删图片
            SceneItem item;
            item.kind  = SceneItem::Kind::Image;
            item.id    = e.image.id;
            item.image = e.image;
            _items.push_back(item);
            HistoryEntry redo_entry;
            redo_entry.op    = HistoryEntry::Op::RemoveImage;
            redo_entry.image = e.image;
            _redo_stack.push_back(redo_entry);
            break;
        }
        case HistoryEntry::Op::ClearBoard: {
            _items = e.snapshot;
            HistoryEntry redo_entry;
            redo_entry.op = HistoryEntry::Op::ClearBoard;
            _redo_stack.push_back(redo_entry);
            break;
        }
    }
    setDirty();
}

void Whiteboard::redo()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_redo_stack.empty()) {
        return;
    }
    HistoryEntry e = _redo_stack.back();
    _redo_stack.pop_back();

    // 重做会把它重新变回"已执行操作"，压回 undo 栈；此时不能清空 redo 栈
    // （还有后续条目要重做），因此不走 pushHistory()
    auto pushUndoOnly = [this](const HistoryEntry& entry) { _undo_stack.push_back(entry); };

    switch (e.op) {
        case HistoryEntry::Op::AddStroke: {
            SceneItem item;
            item.kind   = SceneItem::Kind::Stroke;
            item.id     = e.stroke.id != 0 ? e.stroke.id : static_cast<uint32_t>(_items.size() + 1);
            item.stroke = e.stroke;
            _items.push_back(item);
            pushUndoOnly(HistoryEntry{HistoryEntry::Op::AddStroke, e.stroke, {}, {}});
            break;
        }
        case HistoryEntry::Op::AddImage: {
            SceneItem item;
            item.kind  = SceneItem::Kind::Image;
            item.id    = e.image.id;
            item.image = e.image;
            _items.push_back(item);
            pushUndoOnly(HistoryEntry{HistoryEntry::Op::AddImage, {}, e.image, {}});
            break;
        }
        case HistoryEntry::Op::RemoveImage: {
            SceneItem* cur = findItemLocked(e.image.id);
            if (cur != nullptr && cur->kind == SceneItem::Kind::Image) {
                HistoryEntry undo_entry;
                undo_entry.op    = HistoryEntry::Op::RemoveImage;
                undo_entry.image = cur->image;
                pushUndoOnly(undo_entry);
                removeItemLocked(e.image.id);
            }
            break;
        }
        case HistoryEntry::Op::ClearBoard: {
            if (!_items.empty()) {
                pushUndoOnly(HistoryEntry{HistoryEntry::Op::ClearBoard, {}, {}, _items});
                _items.clear();
            }
            break;
        }
    }
    setDirty();
}

void Whiteboard::clear()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_items.empty()) {
        return;
    }
    pushHistory(HistoryEntry{HistoryEntry::Op::ClearBoard, {}, {}, _items});
    _items.clear();
    setDirty();
}

void Whiteboard::setViewport(const Viewport& vp)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _vp    = vp;
    setDirty();
}

bool Whiteboard::renderIfDirty()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_dirty) {
        return false;
    }
    renderLocked();
    _dirty = false;
    return true;
}

void Whiteboard::pushHistory(const HistoryEntry& e)
{
    _undo_stack.push_back(e);
    // 新的正向操作使 redo 分支失效
    _redo_stack.clear();
}

SceneItem* Whiteboard::findItemLocked(uint32_t id)
{
    if (id == 0) {
        return nullptr;
    }
    for (auto it = _items.rbegin(); it != _items.rend(); ++it) {
        if (it->id == id) {
            return &*it;
        }
    }
    return nullptr;
}

void Whiteboard::removeItemLocked(uint32_t id)
{
    _items.erase(std::remove_if(_items.begin(), _items.end(),
                                [id](const SceneItem& s) { return s.id == id; }),
                 _items.end());
}

void Whiteboard::fill(uint16_t color)
{
    std::fill(_fb.begin(), _fb.end(), color);
}

void Whiteboard::setPixel(int32_t x, int32_t y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= kScreenWidth || y >= kScreenHeight) {
        return;
    }
    _fb[static_cast<size_t>(y) * kScreenWidth + x] = color;
}

void Whiteboard::blendPixel(int32_t x, int32_t y, uint16_t color, uint8_t alpha)
{
    if (x < 0 || y < 0 || x >= kScreenWidth || y >= kScreenHeight) {
        return;
    }
    uint16_t* dst = &_fb[static_cast<size_t>(y) * kScreenWidth + x];
    if (alpha >= 255) {
        *dst = color;
        return;
    }
    const int a  = alpha;
    const int ia = 255 - a;
    const uint8_t r = static_cast<uint8_t>((r5(color) * a + r5(*dst) * ia) / 255);
    const uint8_t g = static_cast<uint8_t>((g6(color) * a + g6(*dst) * ia) / 255);
    const uint8_t b = static_cast<uint8_t>((b5(color) * a + b5(*dst) * ia) / 255);
    *dst = rgb565(r, g, b);
}

void Whiteboard::renderLocked()
{
    fill(kPaperColor);
    for (const SceneItem& it : _items) {
        if (it.kind == SceneItem::Kind::Stroke) {
            drawStrokeLocked(it.stroke);
        } else {
            auto found = _bitmaps.find(it.image.img);
            if (found != _bitmaps.end()) {
                drawImageLocked(it.image, found->second);
            }
        }
    }
}

void Whiteboard::drawStrokeLocked(const Stroke& s)
{
    const float scale = _vp.scale > 0.0f ? _vp.scale : 1.0f;

    uint16_t paint_color = s.color;
    if (s.brush == BrushKind::Eraser) {
        paint_color = kPaperColor;
    }
    const int32_t w = std::max(1, static_cast<int32_t>(std::lround(static_cast<float>(s.width) * scale)));
    const uint8_t alpha = (s.brush == BrushKind::Eraser) ? 255u : s.alpha;
    const int32_t radius = w / 2;
    const size_t npts = s.pts.size() / 2;
    if (npts == 0) {
        return;
    }

    std::vector<int32_t> sx(npts), sy(npts);
    int32_t minx = kScreenWidth, miny = kScreenHeight, maxx = -1, maxy = -1;
    for (size_t i = 0; i < npts; ++i) {
        sx[i] = static_cast<int32_t>(
            std::lround((static_cast<float>(s.pts[i * 2]) - _vp.offset_x) * scale));
        sy[i] = static_cast<int32_t>(
            std::lround((static_cast<float>(s.pts[i * 2 + 1]) - _vp.offset_y) * scale));
        minx = std::min(minx, sx[i]); maxx = std::max(maxx, sx[i]);
        miny = std::min(miny, sy[i]); maxy = std::max(maxy, sy[i]);
    }
    minx = std::max<int32_t>(0, minx - radius); miny = std::max<int32_t>(0, miny - radius);
    maxx = std::min<int32_t>(kScreenWidth - 1, maxx + radius);
    maxy = std::min<int32_t>(kScreenHeight - 1, maxy + radius);
    if (maxx < minx || maxy < miny) {
        return;
    }
    const int32_t bw = maxx - minx + 1;
    const int32_t bh = maxy - miny + 1;

    if (alpha >= 255) {
        auto stamp_paint = [&](int32_t cx, int32_t cy) {
            for (int32_t dy = -radius; dy <= radius; ++dy) {
                for (int32_t dx = -radius; dx <= radius; ++dx) {
                    if (dx * dx + dy * dy <= radius * radius + radius) {
                        setPixel(cx + dx, cy + dy, paint_color);
                    }
                }
            }
        };
        auto trace = [&](const auto& mark) {
            for (size_t i = 0; i + 1 < npts; ++i) {
                int32_t x0 = sx[i], y0 = sy[i];
                const int32_t x1 = sx[i + 1], y1 = sy[i + 1];
                const int32_t dx = std::abs(x1 - x0);
                const int32_t dy = -std::abs(y1 - y0);
                const int32_t sx_step = x0 < x1 ? 1 : -1;
                const int32_t sy_step = y0 < y1 ? 1 : -1;
                int32_t err = dx + dy;
                for (;;) {
                    mark(x0, y0);
                    if (x0 == x1 && y0 == y1) {
                        break;
                    }
                    const int32_t e2 = 2 * err;
                    if (e2 >= dy) {
                        err += dy;
                        x0 += sx_step;
                    }
                    if (e2 <= dx) {
                        err += dx;
                        y0 += sy_step;
                    }
                }
            }
            if (npts == 1) {
                mark(sx[0], sy[0]);
            }
        };
        trace(stamp_paint);
        return;
    }

    // 半透明：覆盖掩码（每像素一次）→ 统一对 dst 混合
    std::vector<uint8_t> cover(static_cast<size_t>(bw) * bh, 0);
    auto mark_cover = [&](int32_t cx, int32_t cy) {
        for (int32_t dy = -radius; dy <= radius; ++dy) {
            for (int32_t dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dy * dy <= radius * radius + radius) {
                    const int32_t bx = cx + dx - minx;
                    const int32_t by = cy + dy - miny;
                    if (bx >= 0 && by >= 0 && bx < bw && by < bh) {
                        cover[static_cast<size_t>(by) * bw + bx] = 1;
                    }
                }
            }
        }
    };
    for (size_t i = 0; i + 1 < npts; ++i) {
        int32_t x0 = sx[i], y0 = sy[i];
        const int32_t x1 = sx[i + 1], y1 = sy[i + 1];
        const int32_t dx = std::abs(x1 - x0);
        const int32_t dy = -std::abs(y1 - y0);
        const int32_t sx_step = x0 < x1 ? 1 : -1;
        const int32_t sy_step = y0 < y1 ? 1 : -1;
        int32_t err = dx + dy;
        for (;;) {
            mark_cover(x0, y0);
            if (x0 == x1 && y0 == y1) {
                break;
            }
            const int32_t e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx_step;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy_step;
            }
        }
    }
    if (npts == 1) {
        mark_cover(sx[0], sy[0]);
    }
    for (int32_t by = 0; by < bh; ++by) {
        for (int32_t bx = 0; bx < bw; ++bx) {
            if (cover[static_cast<size_t>(by) * bw + bx]) {
                blendPixel(minx + bx, miny + by, paint_color, alpha);
            }
        }
    }
}

// 旋转缩放 blit：目标屏幕像素 → 反变换到源图坐标 → 最近邻采样。
// 图片局部坐标 u,v ∈ [-0.5, 0.5]（源中心为原点）。
// world = center + R(u*w, v*h)；screen = (world - offset) * scale
void Whiteboard::drawImageLocked(const ImageItem& img, const ImageBitmap& bmp)
{
    const float scale = _vp.scale > 0.0f ? _vp.scale : 1.0f;
    const float cosr = std::cos(img.rot);
    const float sinr = std::sin(img.rot);
    const float ww = img.w * scale;   // 屏幕尺寸（世界宽 × 视口缩放）
    const float hh = img.h * scale;
    const float cxs = (img.cx - _vp.offset_x) * scale;   // 中心屏幕坐标
    const float cys = (img.cy - _vp.offset_y) * scale;

    // 屏幕外接矩形（旋转后）
    const float ext_x = std::abs(ww * cosr) + std::abs(hh * sinr);
    const float ext_y = std::abs(ww * sinr) + std::abs(hh * cosr);
    int32_t x0 = static_cast<int32_t>(std::floor(cxs - ext_x / 2.0f));
    int32_t y0 = static_cast<int32_t>(std::floor(cys - ext_y / 2.0f));
    int32_t x1 = static_cast<int32_t>(std::ceil(cxs + ext_x / 2.0f));
    int32_t y1 = static_cast<int32_t>(std::ceil(cys + ext_y / 2.0f));
    x0 = std::max<int32_t>(x0, 0);
    y0 = std::max<int32_t>(y0, 0);
    x1 = std::min<int32_t>(x1, kScreenWidth - 1);
    y1 = std::min<int32_t>(y1, kScreenHeight - 1);
    if (x0 > x1 || y0 > y1) {
        return;
    }

    // 逆变换矩阵：world = c + R·local  ⇒  local = Rᵀ·(world - c)
    // 局部像素半宽 = 源 w/h 一半（源像素单位）→ 采样系数
    const float inv_w2 = (bmp.w > 1) ? (bmp.w / ww) : 0.0f;   // 屏幕 dx → 源 u(像素)
    const float inv_h2 = (bmp.h > 1) ? (bmp.h / hh) : 0.0f;

    for (int32_t py = y0; py <= y1; ++py) {
        const float wy = (py / scale) + _vp.offset_y;   // 屏幕 y → 世界 y
        for (int32_t px = x0; px <= x1; ++px) {
            const float wx = (px / scale) + _vp.offset_x;   // 屏幕 x → 世界 x
            const float dxw = wx - img.cx;
            const float dyw = wy - img.cy;
            // 逆旋转：d = Rᵀ·Δ（旋转正向与浏览器 canvas rotate 一致：y 向下顺时针）
            const float lx = dxw * cosr + dyw * sinr;
            const float ly = -dxw * sinr + dyw * cosr;
            // 世界长度 → 源像素坐标
            const float su = lx * (bmp.w / img.w) + bmp.w / 2.0f;
            const float sv = ly * (bmp.h / img.h) + bmp.h / 2.0f;
            if (su < 0 || sv < 0 || su >= static_cast<float>(bmp.w) || sv >= static_cast<float>(bmp.h)) {
                continue;
            }
            const int32_t u = static_cast<int32_t>(su);
            const int32_t v = static_cast<int32_t>(sv);
            setPixel(px, py, bmp.px[static_cast<size_t>(v) * bmp.w + u]);
        }
    }
}

}  // namespace sketch
