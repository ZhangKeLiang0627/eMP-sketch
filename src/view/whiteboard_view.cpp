#include "view/whiteboard_view.hpp"

namespace sketch {

void WhiteboardView::onEnter(lv_obj_t* parent, Whiteboard& board, const std::string& status_text)
{
    _board = &board;

    // 画布直接指向白板的 RGB565 帧缓冲（构造后稳定不重分配），
    // 每帧只 invalidate、不拷贝，降低 CPU。
    _canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(_canvas, const_cast<uint16_t*>(_board->framebuffer()),
                         Whiteboard::width(), Whiteboard::height(), LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(_canvas, Whiteboard::width(), Whiteboard::height());
    lv_obj_center(_canvas);

    // 顶部状态栏（ASCII only：LVGL 默认字体无中文字形）
    _status_label = lv_label_create(parent);
    lv_label_set_text(_status_label, status_text.c_str());
    lv_obj_set_style_text_color(_status_label, lv_color_hex(0x37352F), 0);
    lv_obj_align(_status_label, LV_ALIGN_TOP_MID, 0, 6);

    // 首次渲染
    _board->renderIfDirty();
    lv_obj_invalidate(_canvas);
}

void WhiteboardView::onExit()
{
    _board        = nullptr;
    _canvas       = nullptr;
    _status_label = nullptr;
    // 对象随 parent 销毁一并释放
}

void WhiteboardView::tick()
{
    if (!_board || !_canvas) {
        return;
    }
    if (_board->renderIfDirty()) {
        lv_obj_invalidate(_canvas);
    }
}

}  // namespace sketch
