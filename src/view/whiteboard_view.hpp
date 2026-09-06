#pragma once

#include "core/whiteboard.hpp"

#include <lvgl.h>

#include <functional>
#include <string>

namespace sketch {

// 视图层钩子：板端 UI 事件 → 由 main 组装网络动作
struct UiHooks {
    // 板端触摸绘制的增量段（世界坐标，同 id 段由 addStroke 合并）；需 addStroke + 广播给浏览器
    std::function<void(const Stroke&)> on_local_stroke;
    // 顶栏"清空"按钮
    std::function<void()> on_clear;
    // 绘图模式切换（参数 true = 进入绘图/反向画图）
    std::function<void(bool)> on_mode_changed;
};

// 白板视图：LVGL 画布（RGB565 帧缓冲直接映射）+ eMP-gba/eMP-video 同款顶部操作栏
// （90% 宽 × 38 高、radius 5、bg #EEEEEE @90%、400ms ease-out 展开/收回动画、
//  色块圆角按钮 + 居中标题）。按钮：截图 / 清空 / 绘图(锁定切换)；右侧 x 收起，
//  顶缘点按/下滑重新展开。绘图模式下板端触摸 = 画图（反向画图）。
class WhiteboardView {
public:
    void onEnter(lv_obj_t* parent, Whiteboard& board, const std::string& status_text,
                 const UiHooks& hooks);
    void onExit();
    void tick();
    bool drawMode() const { return _draw_mode; }
    bool barVisible() const { return _bar_visible; }

    static constexpr int32_t kBarW = 480 * 9 / 10;   // 90% 屏宽（gba/eMP-video topCont）
    static constexpr int32_t kBarH = 38;

private:
    lv_obj_t* makeButton(lv_obj_t* parent, const char* text, lv_color_t bg,
                         lv_color_t pressed, int w, int h);
    void animObj(lv_obj_t* obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to);
    void topBarShow();
    void topBarHide();
    void updateModeButton();
    void pollInput();            // 触摸：绘图模式画线 / 顶缘手势展开
    void doScreenshot();         // 保存当前屏（/dev/fb0 首帧）为 BMP

    lv_obj_t* _canvas       = nullptr;
    lv_obj_t* _bar          = nullptr;
    lv_obj_t* _title        = nullptr;
    lv_obj_t* _shot_btn     = nullptr;
    lv_obj_t* _clear_btn    = nullptr;
    lv_obj_t* _mode_btn     = nullptr;
    lv_obj_t* _mode_btn_lbl = nullptr;
    lv_obj_t* _close_btn    = nullptr;
    Whiteboard* _board      = nullptr;
    UiHooks _hooks;

    bool _bar_visible = false;
    bool _draw_mode   = false;
    bool _edge_armed   = false;   // 手势：触点起始于顶缘
    // 触摸绘制状态
    uint32_t _touch_id = 5000000; // 板端绘制对象 id 基数（避开浏览器侧小 id）
    bool _touching = false;
    bool _last_valid = false;
    int32_t _last_wx = 0, _last_wy = 0;
};

}  // namespace sketch
