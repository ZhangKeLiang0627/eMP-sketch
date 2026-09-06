#include "core/whiteboard.hpp"

#include <algorithm>
#include <cmath>

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

void Whiteboard::addStroke(const Stroke& stroke)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!stroke.hasPoints()) {
        return;
    }

    // 同 id 增量段合并到末笔（浏览器把一笔拆成多段消息推送）
    if (stroke.id != 0 && !_strokes.empty()) {
        Stroke& last = _strokes.back();
        if (last.id == stroke.id) {
            // 追加段共享首点（浏览器发 [上一已发点,当前点]），跳过重复点对
            const size_t n = last.pts.size();
            if (n >= 2 && stroke.pts.size() >= 2 &&
                last.pts[n - 2] == stroke.pts[0] && last.pts[n - 1] == stroke.pts[1]) {
                last.pts.insert(last.pts.end(), stroke.pts.begin() + 2, stroke.pts.end());
            } else {
                last.pts.insert(last.pts.end(), stroke.pts.begin(), stroke.pts.end());
            }
            setDirty();
            return;
        }
    }

    _strokes.push_back(stroke);
    pushHistory(HistoryEntry{HistoryEntry::Op::AddStroke, stroke, {}});
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

    if (e.op == HistoryEntry::Op::AddStroke) {
        // 反向：按 id 移除。若该笔后续被同 id 段合并变大，移除的是完整末笔。
        Stroke* cur = findStrokeLocked(e.stroke.id);
        if (cur != nullptr) {
            HistoryEntry redo_entry;
            redo_entry.op      = HistoryEntry::Op::AddStroke;
            redo_entry.stroke  = *cur;   // 记录完整对象供 redo
            _redo_stack.push_back(redo_entry);
            removeStrokeLocked(e.stroke.id);
        }
    } else {  // ClearBoard：恢复清空前快照
        _strokes = e.snapshot;
        HistoryEntry redo_entry;
        redo_entry.op = HistoryEntry::Op::ClearBoard;
        _redo_stack.push_back(redo_entry);
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

    if (e.op == HistoryEntry::Op::AddStroke) {
        _strokes.push_back(e.stroke);
        pushHistory(HistoryEntry{HistoryEntry::Op::AddStroke, e.stroke, {}});
    } else {  // ClearBoard：重做清空
        if (!_strokes.empty()) {
            pushHistory(HistoryEntry{HistoryEntry::Op::ClearBoard, {}, _strokes});
            _strokes.clear();
        }
    }
    setDirty();
}

void Whiteboard::clear()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_strokes.empty()) {
        return;
    }
    pushHistory(HistoryEntry{HistoryEntry::Op::ClearBoard, {}, _strokes});
    _strokes.clear();
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

Stroke* Whiteboard::findStrokeLocked(uint32_t id)
{
    if (id == 0) {
        return nullptr;
    }
    for (auto it = _strokes.rbegin(); it != _strokes.rend(); ++it) {
        if (it->id == id) {
            return &*it;
        }
    }
    return nullptr;
}

void Whiteboard::removeStrokeLocked(uint32_t id)
{
    _strokes.erase(std::remove_if(_strokes.begin(), _strokes.end(),
                                  [id](const Stroke& s) { return s.id == id; }),
                   _strokes.end());
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
    for (const Stroke& s : _strokes) {
        drawStrokeLocked(s);
    }
}

void Whiteboard::drawStrokeLocked(const Stroke& s)
{
    const float scale = _vp.scale > 0.0f ? _vp.scale : 1.0f;

    // 橡皮 = 纸面色实心覆盖（按对象序合成，可稳定重绘）
    uint16_t paint_color = s.color;
    if (s.brush == BrushKind::Eraser) {
        paint_color = kPaperColor;
    }
    // 最终宽度由客户端算好（荧光笔/橡皮加粗在客户端乘系数），板端按收到的 width 渲染
    const int32_t w = std::max(1, static_cast<int32_t>(std::lround(static_cast<float>(s.width) * scale)));
    const uint8_t alpha = (s.brush == BrushKind::Eraser) ? 255u : s.alpha;
    const int32_t radius = w / 2;
    const size_t npts = s.pts.size() / 2;
    if (npts == 0) {
        return;
    }

    // 把世界坐标折线投影到屏幕坐标
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
    // 外扩笔帽半径并裁剪到屏幕
    minx = std::max<int32_t>(0, minx - radius); miny = std::max<int32_t>(0, miny - radius);
    maxx = std::min<int32_t>(kScreenWidth - 1, maxx + radius);
    maxy = std::min<int32_t>(kScreenHeight - 1, maxy + radius);
    if (maxx < minx || maxy < miny) {
        return;
    }
    const int32_t bw = maxx - minx + 1;
    const int32_t bh = maxy - miny + 1;

    if (alpha >= 255) {
        // 不透明：直接印章式落笔（幂等，重叠无副作用）
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

    // 半透明：先画"覆盖掩码"（每像素只标记一次），再统一对 dst 混合一次。
    // 若按旧法逐印章直接混合，相邻印章重叠区会被合成 N 次 → 视觉接近不透明。
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
    // 沿线逐点推进（Bresenham），与不透明路径同一轨迹
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

}  // namespace sketch
