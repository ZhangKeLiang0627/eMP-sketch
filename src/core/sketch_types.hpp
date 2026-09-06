#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sketch {

// 渲染分辨率（与 LVGL 显示一致，LV_COLOR_DEPTH=16 → RGB565）
constexpr int32_t kScreenWidth  = 480;
constexpr int32_t kScreenHeight = 480;

// 纸面底色 #FAFAF8、默认墨色 #37352F（编辑部极简配色）
constexpr uint8_t kPaperR = 0xFA;
constexpr uint8_t kPaperG = 0xFA;
constexpr uint8_t kPaperB = 0xF8;

// RGB888 → RGB565
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

// 笔触类型（v0.2）：pen 实心 / marker 马克笔 / highlighter 荧光笔 / eraser 橡皮
enum class BrushKind : uint8_t {
    Pen = 0,
    Marker = 1,
    Highlighter = 2,
    Eraser = 3,
};

// 一条笔画 = 一段折线（世界坐标，扁平存储 x0,y0,x1,y1,...）。
// v0.2 起支持"增量段合并"：同一次落笔的多段消息共享同一 id（id!=0），
// 板端把同 id 段合并为单笔 —— 对象粒度 = 浏览器整笔，撤销/重做粒度一致。
struct Stroke {
    uint32_t id = 0;                 // 笔 id；0 = 独立新笔（每次消息新对象，兼容旧协议）
    uint32_t color = 0x000000;       // RGB565（eraser 由板端替换为纸面色）
    uint8_t  width = 3;              // px（世界坐标粗细）
    uint8_t  alpha = 255;            // 0-255，<255 时板端与既有像素做 alpha 合成
    BrushKind brush = BrushKind::Pen;

    std::vector<int32_t> pts;        // 世界坐标，偶数索引为 x，奇数索引为 y

    bool hasPoints() const { return pts.size() >= 2; }
};

// 图片位图（RGB565 原始像素，v0.2 图片对象的数据源，由 img-data 消息上传缓存）
struct ImageBitmap {
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint16_t> px;        // 行主序 w*h
    bool valid() const { return w > 0 && h > 0 && px.size() == static_cast<size_t>(w) * h; }
};

// 场景图片对象（v0.2）：世界坐标下的矩形贴图，中心 (cx,cy)、显示宽高 (w,h)、旋转 rot
struct ImageItem {
    uint32_t id = 0;                 // 场景对象 id
    uint32_t img = 0;                // 引用 ImageBitmap 缓存键
    float    cx = 0.0f;              // 中心 x（世界坐标）
    float    cy = 0.0f;
    float    w  = 0.0f;              // 显示宽（世界坐标，>0）
    float    h  = 0.0f;
    float    rot = 0.0f;             // 弧度（顺时针，y 向下）
    bool valid() const { return w > 0.0f && h > 0.0f; }
};

// 统一场景条目：场景图按此列表顺序重绘（笔画与图片可任意交错）
struct SceneItem {
    enum class Kind : uint8_t { Stroke = 0, Image = 1 } kind = Kind::Stroke;
    uint32_t id   = 0;               // 对象 id（Stroke 与其 id 一致）
    Stroke   stroke;                 // kind == Stroke
    ImageItem image;                 // kind == Image
};

// 视口：世界坐标 → 屏幕坐标的变换。screen = (world - offset) * scale
struct Viewport {
    float scale    = 1.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
};

}  // namespace sketch
