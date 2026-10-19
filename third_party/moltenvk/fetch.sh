#!/bin/bash
# ============================================================================
# MoltenVK 获取脚本（iOS 端 Vulkan 运行时）。
#
# iOS 没有系统 Vulkan loader，引擎经 MoltenVK（Vulkan-on-Metal 的可移植性
# 实现）直链接入 Metal。本脚本从 KhronosGroup/MoltenVK 官方发行版下载
# iOS 包，提取两样东西到本目录：
#   - static/MoltenVK.xcframework  → MoltenVK.xcframework
#     （静态库切片：iphoneos-arm64 + 模拟器 arm64/x86_64，Xcode 按目标自动选片）
#   - include/                     → include/
#     （与该库 ABI 对齐的 Vulkan 头文件，比系统/_SDK 里的头更稳妥）
#
# 用法：
#   ./fetch.sh            # 拉取默认版本
#   ./fetch.sh v1.4.2     # 拉取指定版本（tag 名，需带 v 前缀）
#
# 重复执行会先清掉旧产物再下载，幂等。
# ============================================================================
set -euo pipefail

MVK_VERSION="${1:-v1.4.2}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAR_URL="https://github.com/KhronosGroup/MoltenVK/releases/download/${MVK_VERSION}/MoltenVK-ios.tar"
WORK_DIR="${SCRIPT_DIR}/.download"

echo ">> fetch MoltenVK ${MVK_VERSION} (ios)"
echo ">> ${TAR_URL}"

rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}"

curl -fL --progress-bar "${TAR_URL}" -o "${WORK_DIR}/MoltenVK-ios.tar"
tar -xf "${WORK_DIR}/MoltenVK-ios.tar" -C "${WORK_DIR}"

# 发行包是双层嵌套（MoltenVK/MoltenVK/...），内层才是 SDK 根（含 include/、
# 静态与动态 xcframework 等）。直接按 static/MoltenVK.xcframework 的位置定位，
# 不依赖具体层级深度。
SRC_FRAMEWORK="$(find "${WORK_DIR}" -type d -path "*/static/MoltenVK.xcframework" | head -1)"
if [[ -z "${SRC_FRAMEWORK}" ]]; then
    echo "!! 包内未找到 static/MoltenVK.xcframework，发行包结构可能已变化" >&2
    exit 1
fi
SRC_ROOT="$(dirname "$(dirname "${SRC_FRAMEWORK}")")"

# 清旧产物，落新产物。
rm -rf "${SCRIPT_DIR}/MoltenVK.xcframework" "${SCRIPT_DIR}/include"
cp -R "${SRC_ROOT}/static/MoltenVK.xcframework" "${SCRIPT_DIR}/MoltenVK.xcframework"
cp -R "${SRC_ROOT}/include" "${SCRIPT_DIR}/include"

rm -rf "${WORK_DIR}"

echo ">> done:"
echo "   ${SCRIPT_DIR}/MoltenVK.xcframework"
echo "   ${SCRIPT_DIR}/include"
