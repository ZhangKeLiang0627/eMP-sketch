#pragma once

#include <cstdint>

namespace sketch {

// 初始化 LVGL 显示与输入（HAL）：
//   - 桌面（SDL）：SDL 窗口 + 鼠标 + 键盘
//   - 嵌入式（fbdev + evdev）：framebuffer 显示 + evdev 触摸
// 成功返回 true；失败返回 false。
bool initLvglHal(int32_t width, int32_t height);

// 反初始化（关闭窗口/释放资源）
void shutdownLvglHal();

}  // namespace sketch
