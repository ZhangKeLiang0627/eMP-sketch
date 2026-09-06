#
# Allwinner T113-S3 (TinaLinux) 交叉编译工具链文件。
#
# 本文件只是薄委托：真正的工具链配置在 eMP-toolchain 仓库里统一维护，
# 任何 eMP 系列项目（mainPage / settings / tokenMonitor / template 等）都引用同一份，
# 避免各项目各拷贝一份造成漂移。
#
# 依赖：https://github.com/ZhangKeLiang0627/eMP-toolchain
#
# 用法：
#   export T113_SDK=/path/to/eMP-toolchain        # 解压 setup.sh 后含 toolchain/ 与 sysroot/
#   export STAGING_DIR=$T113_SDK/sysroot
#   cmake -S . -B build/t113 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/build_for_t113s3.cmake \
#         -DT113_SDK=$T113_SDK \
#         -DTEMPLATE_EVDEV_TOUCH=/dev/input/event1
#   cmake --build build/t113 -j$(nproc)
#
if(NOT DEFINED T113_SDK)
    set(T113_SDK $ENV{T113_SDK})
endif()
if(NOT T113_SDK)
    message(FATAL_ERROR "T113_SDK 未设置。请 export T113_SDK=/path/to/eMP-toolchain 或传 -DT113_SDK=")
endif()

set(DELEGATE "${T113_SDK}/cmake/build_for_t113s3.cmake")
if(NOT EXISTS "${DELEGATE}")
    message(FATAL_ERROR "未找到 eMP-toolchain 工具链文件: ${DELEGATE}")
endif()

include("${DELEGATE}")
