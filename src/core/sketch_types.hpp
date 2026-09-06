#pragma once

#include <cstdint>
#include <vector>

namespace sketch {

// 渲染分辨率（与 LVGL 显示一致，LV_COLOR_DEPTH=16 → RGB565）
constexpr int32_t kScreenWidth  = 480;
constexpr int32_t kScreenHeight = 480;

// 一条笔画 = 一段折线（世界坐标，扁平存储 x0,y0,x1,y1,...）
struct Stroke {
    uint32_t color = 0x000000;  // RGB565
    uint8_t  width = 3;         // px（世界坐标下的粗细）
    std::vector<int32_t> pts;   // 世界坐标，偶数索引为 x，奇数索引为 y
};

// 视口：世界坐标 → 屏幕坐标的变换。screen = (world - offset) * scale
struct Viewport {
    float scale    = 1.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
};

// RGB888 → RGB565
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

}  // namespace sketch
