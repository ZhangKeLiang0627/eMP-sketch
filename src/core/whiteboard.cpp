#include "core/whiteboard.hpp"

#include <algorithm>
#include <cmath>

namespace sketch {
namespace {

// 纸面底色 #FAFAF8、默认墨色 #37352F（编辑部极简配色）
constexpr uint16_t kPaperColor = rgb565(0xFA, 0xFA, 0xF8);

}  // namespace

Whiteboard::Whiteboard()
{
    _fb.resize(static_cast<size_t>(kScreenWidth) * kScreenHeight, kPaperColor);
}

void Whiteboard::addStroke(const Stroke& stroke)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (stroke.pts.size() >= 2) {
        _strokes.push_back(stroke);
        _dirty = true;
    }
}

void Whiteboard::clear()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _strokes.clear();
    _dirty = true;
}

void Whiteboard::setViewport(const Viewport& vp)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _vp    = vp;
    _dirty = true;
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
    const int32_t thickness =
        std::max(1, static_cast<int32_t>(std::lround(static_cast<float>(s.width) * scale)));
    const int32_t radius = thickness / 2;

    // 印章式圆点：沿线段 Bresenham 步进，每步落一个填充圆（简单可靠，够 v0.1 用）
    auto stamp = [&](int32_t cx, int32_t cy) {
        for (int32_t dy = -radius; dy <= radius; ++dy) {
            for (int32_t dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dy * dy <= radius * radius + radius) {
                    setPixel(cx + dx, cy + dy, s.color);
                }
            }
        }
    };

    int32_t prev_sx = 0;
    int32_t prev_sy = 0;
    bool has_prev  = false;

    for (size_t i = 0; i + 1 < s.pts.size(); i += 2) {
        const int32_t sx = static_cast<int32_t>(
            std::lround((static_cast<float>(s.pts[i]) - _vp.offset_x) * scale));
        const int32_t sy = static_cast<int32_t>(
            std::lround((static_cast<float>(s.pts[i + 1]) - _vp.offset_y) * scale));

        if (has_prev) {
            int32_t x0 = prev_sx, y0 = prev_sy;
            const int32_t x1 = sx, y1 = sy;
            const int32_t dx = std::abs(x1 - x0);
            const int32_t dy = -std::abs(y1 - y0);
            const int32_t sx_step = x0 < x1 ? 1 : -1;
            const int32_t sy_step = y0 < y1 ? 1 : -1;
            int32_t err = dx + dy;

            for (;;) {
                stamp(x0, y0);
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
        } else {
            stamp(sx, sy);
        }

        prev_sx  = sx;
        prev_sy  = sy;
        has_prev = true;
    }
}

}  // namespace sketch
