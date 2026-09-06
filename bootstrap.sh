#!/usr/bin/env bash
#
# bootstrap.sh — eMP-sketch 依赖引导脚本
#
# 职责：拉取第三方依赖仓库（lvgl / cpp-httplib / nlohmann-json）。
#
# 用法：
#   ./bootstrap.sh
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log()  { printf '[bootstrap] %s\n' "$*"; }
fail() { printf '[bootstrap] ERROR: %s\n' "$*" >&2; exit 1; }

PYTHON="$(command -v python3 || command -v python || true)"
[[ -n "$PYTHON" ]] || fail "需要 python3"

log "使用 Python: $PYTHON ($("$PYTHON" --version 2>&1))"
log "拉取依赖仓库 (repos.json) ..."
"$PYTHON" "${ROOT_DIR}/fetch_repos.py"

log "eMP-sketch bootstrap 完成。"
log "桌面（SDL）编译:"
log "  cmake -S . -B build/sdl -DSKETCH_USE_SDL=ON"
log "  cmake --build build/sdl -j$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
log "T113 交叉编译:"
log "  export T113_SDK=/path/to/eMP-toolchain"
log "  export STAGING_DIR=\$T113_SDK/sysroot"
log "  cmake -S . -B build/t113 -DCMAKE_TOOLCHAIN_FILE=cmake/build_for_t113s3.cmake -DT113_SDK=\$T113_SDK"
log "  cmake --build build/t113 -j\$(nproc)"
