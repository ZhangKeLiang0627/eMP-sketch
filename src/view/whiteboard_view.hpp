#pragma once

#include "core/whiteboard.hpp"

#include <lvgl.h>

#include <string>

namespace sketch {

// 白板视图：把 Whiteboard 的 RGB565 帧缓冲呈现到 LVGL 画布，并显示顶部状态栏。
class WhiteboardView {
public:
    void onEnter(lv_obj_t* parent, Whiteboard& board, const std::string& status_text);
    void onExit();
    void tick();

private:
    lv_obj_t* _canvas       = nullptr;
    lv_obj_t* _status_label = nullptr;
    Whiteboard* _board      = nullptr;
};

}  // namespace sketch
