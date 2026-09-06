#include "view/whiteboard_view.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace sketch {
namespace {

constexpr int32_t kFbBytesVisible = 480 * 480 * 4;   // /dev/fb0 首可见帧（XRGB8888）
constexpr int32_t kAnimTime       = 400;             // eMP-gba/eMP-video: 400ms ease-out

}  // namespace

void WhiteboardView::onEnter(lv_obj_t* parent, Whiteboard& board, const std::string& status_text,
                             const UiHooks& hooks)
{
    _board = &board;
    _hooks = hooks;

    // 画布（全屏 480x480，直接映射 RGB565 帧缓冲；顶栏覆盖其上）
    _canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(_canvas, const_cast<uint16_t*>(_board->framebuffer()),
                         Whiteboard::width(), Whiteboard::height(), LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(_canvas, Whiteboard::width(), Whiteboard::height());
    lv_obj_set_pos(_canvas, 0, 0);

    // ---- eMP-gba/eMP-video topCont 同款顶栏：90% 宽 × 38 高、radius 5、#EEEEEE@90% ----
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, kBarW, kBarH);
    // 收起位（y=-40、宽 20），随后播放展开动画
    lv_obj_set_pos(bar, (Whiteboard::width() - 20) / 2, -kBarH);
    lv_obj_set_style_bg_opa(bar, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xEEEEEE), 0);
    lv_obj_set_style_radius(bar, 5, 0);
    _bar = bar;
    _bar_visible = false;

    // 左排按钮：截图 / 清空（同 gba 位置与配色习惯：截图蓝、操作绿）
    _shot_btn = makeButton(bar, "Shot", lv_color_hex(0x0078BA), lv_color_hex(0x005E93), 56, 34);
    lv_obj_align(_shot_btn, LV_ALIGN_LEFT_MID, 10, 0);
    _clear_btn = makeButton(bar, "Clear", lv_color_hex(0x4EA35A), lv_color_hex(0x3D8346), 56, 34);
    lv_obj_align(_clear_btn, LV_ALIGN_LEFT_MID, 74, 0);

    // 右侧：绘图(反向画图)开关 + x 收起（同 gba x 按钮）
    _mode_btn = makeButton(bar, "Draw", lv_color_hex(0x0078BA), lv_color_hex(0x005E93), 62, 34);
    lv_obj_align(_mode_btn, LV_ALIGN_RIGHT_MID, -52, 0);
    _mode_btn_lbl = lv_obj_get_child(_mode_btn, 0);
    _close_btn = makeButton(bar, "x", lv_color_hex(0xFF6056), lv_color_hex(0xE44543), 34, 34);
    lv_obj_align(_close_btn, LV_ALIGN_RIGHT_MID, -10, 0);

    // 居中标题
    _title = lv_label_create(bar);
    lv_label_set_text(_title, "eMP-sketch");
    lv_obj_set_style_text_color(_title, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(_title);

    lv_obj_add_event_cb(_shot_btn, [](lv_event_t* e) {
        auto* self = static_cast<WhiteboardView*>(lv_event_get_user_data(e));
        self->doScreenshot();
    }, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_clear_btn, [](lv_event_t* e) {
        auto* self = static_cast<WhiteboardView*>(lv_event_get_user_data(e));
        if (self->_hooks.on_clear) {
            self->_hooks.on_clear();
        }
    }, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_mode_btn, [](lv_event_t* e) {
        auto* self = static_cast<WhiteboardView*>(lv_event_get_user_data(e));
        self->_draw_mode = !self->_draw_mode;
        self->updateModeButton();
        if (self->_hooks.on_mode_changed) {
            self->_hooks.on_mode_changed(self->_draw_mode);
        }
    }, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_close_btn, [](lv_event_t* e) {
        auto* self = static_cast<WhiteboardView*>(lv_event_get_user_data(e));
        self->topBarHide();
    }, LV_EVENT_CLICKED, this);

    // 启动：展开动画进入（eMP-gba 同款 expand）
    topBarShow();

    // 首次渲染
    _board->renderIfDirty();
    lv_obj_invalidate(_canvas);
}

void WhiteboardView::onExit()
{
    _board        = nullptr;
    _canvas       = nullptr;
    _bar          = nullptr;
    _title        = nullptr;
    _shot_btn     = nullptr;
    _clear_btn    = nullptr;
    _mode_btn     = nullptr;
    _mode_btn_lbl = nullptr;
    _close_btn    = nullptr;
}

lv_obj_t* WhiteboardView::makeButton(lv_obj_t* parent, const char* text, lv_color_t bg,
                                     lv_color_t pressed, int w, int h)
{
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_color(btn, pressed, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 6);

    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_center(label);
    return btn;
}

void WhiteboardView::animObj(lv_obj_t* obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, kAnimTime);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* eMP-gba top_bar_show：下滑 + 居中展开（宽 20 → 90%） */
void WhiteboardView::topBarShow()
{
    if (_bar_visible) {
        return;
    }
    _bar_visible = true;
    const int w = kBarW;
    animObj(_bar, (lv_anim_exec_xcb_t)lv_obj_set_y, -kBarH, 0);
    animObj(_bar, (lv_anim_exec_xcb_t)lv_obj_set_width, 20, w);
    animObj(_bar, (lv_anim_exec_xcb_t)lv_obj_set_x,
            (Whiteboard::width() - 20) / 2, (Whiteboard::width() - w) / 2);
}

void WhiteboardView::topBarHide()
{
    if (!_bar_visible) {
        return;
    }
    _bar_visible = false;
    _draw_mode = false;               // 收起时同步退出绘图，避免误触
    updateModeButton();
    if (_hooks.on_mode_changed) {
        _hooks.on_mode_changed(false);
    }
    const int w = kBarW;
    animObj(_bar, (lv_anim_exec_xcb_t)lv_obj_set_y, 0, -kBarH);
    animObj(_bar, (lv_anim_exec_xcb_t)lv_obj_set_width, w, 20);
    animObj(_bar, (lv_anim_exec_xcb_t)lv_obj_set_x,
            (Whiteboard::width() - w) / 2, (Whiteboard::width() - 20) / 2);
}

void WhiteboardView::updateModeButton()
{
    if (!_mode_btn || !_mode_btn_lbl) {
        return;
    }
    // OFF(锁定，可进入绘图)=蓝；ON(绘图激活)=红"Lock"
    lv_label_set_text(_mode_btn_lbl, _draw_mode ? "Lock" : "Draw");
    lv_obj_set_style_bg_color(_mode_btn, _draw_mode ? lv_color_hex(0xFF6056)
                                                    : lv_color_hex(0x0078BA), 0);
    lv_obj_set_style_bg_color(_mode_btn, _draw_mode ? lv_color_hex(0xE44543)
                                                    : lv_color_hex(0x005E93), LV_STATE_PRESSED);
    lv_obj_invalidate(_mode_btn);
}

void WhiteboardView::tick()
{
    if (!_board || !_canvas) {
        return;
    }
    if (_board->renderIfDirty()) {
        lv_obj_invalidate(_canvas);
    }
    pollInput();
}

void WhiteboardView::pollInput()
{
    // 扫描 LVGL 已解析的输入设备（lv_evdev），取当前触点
    bool pressed = false;
    lv_point_t pt = {0, 0};
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            lv_indev_get_point(indev, &pt);
            pressed = true;
            break;
        }
    }

    // 顶缘手势：隐藏状态下点按/下滑顶部 16px 区展开（画图模式不抢占）
    if (!_draw_mode) {
        if (pressed && !_bar_visible && pt.y >= 0 && pt.y <= 16) {
            _edge_armed = true;
        }
        if (!pressed && _edge_armed) {
            _edge_armed = false;
            topBarShow();
        }
    }
    if (pressed && _bar_visible && pt.y <= kBarH && !_edge_armed && !_bar_visible) {
        /* bar 空白区不处理（按钮各自响应） */
    }

    if (!_draw_mode) {
        return;
    }

    if (pressed && pt.y <= kBarH) {
        _touching = false;   // 顶栏区域：交给按钮
        _last_valid = false;
        return;
    }
    const Viewport& vp = _board->viewport();
    const float scale = vp.scale > 0.0f ? vp.scale : 1.0f;
    const int32_t wx = static_cast<int32_t>(std::lround(pt.x / scale + vp.offset_x));
    const int32_t wy = static_cast<int32_t>(std::lround(pt.y / scale + vp.offset_y));

    if (!pressed) {
        _touching = false;
        _last_valid = false;
        return;
    }
    if (!_touching) {
        _touching = true;
        _last_valid = false;
        ++_touch_id;
    }
    if (!_last_valid) {
        _last_wx = wx;
        _last_wy = wy;
        _last_valid = true;
        return;
    }
    if (wx == _last_wx && wy == _last_wy) {
        return;
    }
    Stroke s;
    s.id    = _touch_id;
    s.brush = BrushKind::Pen;
    s.alpha = 255;
    s.color = rgb565(0x37, 0x35, 0x2F);
    s.width = 3;
    s.pts.push_back(_last_wx);
    s.pts.push_back(_last_wy);
    s.pts.push_back(wx);
    s.pts.push_back(wy);
    _board->addStroke(s);
    if (_hooks.on_local_stroke) {
        _hooks.on_local_stroke(s);
    }
    _last_wx = wx;
    _last_wy = wy;
}

void WhiteboardView::doScreenshot()
{
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&now, &tmv);
    char path[96];
    std::snprintf(path, sizeof(path), "/root/sk_%04d%02d%02d_%02d%02d%02d.bmp",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    int fd = ::open("/dev/fb0", O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "[sketch] screenshot: open fb0 failed\n");
        return;
    }
    std::vector<uint8_t> buf(kFbBytesVisible);
    const ssize_t rd = ::read(fd, buf.data(), buf.size());
    ::close(fd);
    if (rd < 480 * 480 * 4) {
        std::fprintf(stderr, "[sketch] screenshot: short read %zd\n", rd);
        return;
    }

    // 写 32bpp BMP（BI_RGB，BGRX，自底向上）
    const int w = 480, h = 480;
    const int row = w * 4;
    const uint32_t img_size = static_cast<uint32_t>(row) * h;
    FILE* fp = std::fopen(path, "wb");
    if (!fp) {
        std::fprintf(stderr, "[sketch] screenshot: cannot open %s\n", path);
        return;
    }
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    uint32_t file_size = 54 + img_size;
    hdr[2] = file_size & 0xFF; hdr[3] = (file_size >> 8) & 0xFF;
    hdr[4] = (file_size >> 16) & 0xFF; hdr[5] = (file_size >> 24) & 0xFF;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF; hdr[20] = (w >> 16) & 0xFF; hdr[21] = (w >> 24) & 0xFF;
    hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF; hdr[24] = (h >> 16) & 0xFF; hdr[25] = (h >> 24) & 0xFF;
    hdr[26] = 1;
    hdr[28] = 32;
    uint32_t pix = img_size;
    hdr[34] = pix & 0xFF; hdr[35] = (pix >> 8) & 0xFF;
    hdr[36] = (pix >> 16) & 0xFF; hdr[37] = (pix >> 24) & 0xFF;
    std::fwrite(hdr, 1, sizeof(hdr), fp);
    for (int y = h - 1; y >= 0; --y) {
        std::fwrite(buf.data() + static_cast<size_t>(y) * row, 1, row, fp);
    }
    std::fclose(fp);
    std::fprintf(stderr, "[sketch] screenshot saved: %s\n", path);
}

}  // namespace sketch
