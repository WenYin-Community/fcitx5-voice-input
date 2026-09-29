#!/usr/bin/env bash
set -euo pipefail

PKG="fcitx5-voice-input"
TAG="${GITHUB_REF_NAME}"
# 跟随实际仓库（fork/迁移后仍取到正确的归档地址）
REPO_URL="https://github.com/${GITHUB_REPOSITORY}"

# Derive version: use tag if it's a release tag, otherwise use git SHA.
# pkgver only allows: alphanumeric, period, underscore, plus.
if [[ "${TAG}" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    VER="${TAG#v}"
    RELEASE=1
else
    VER="0.0.0+r$(git rev-parse --short HEAD)"
    RELEASE=0
fi

mkdir -p dist/aur/src
TARBALL="dist/aur/src/${PKG}-${VER}.tar.gz"

if [ "${RELEASE}" = "1" ]; then
    # 下载 AUR 用户实际会取到的标签归档：CI 构建与 AUR 构建用同一份源码，
    # 校验和因而对两端都成立
    curl -fsSL --retry 5 --retry-all-errors \
        "${REPO_URL}/archive/refs/tags/v${VER}.tar.gz" -o "${TARBALL}"
else
    # 非 tag（分支 CI）：标签归档还不存在，本地打包，仅用于验证构建，不推 AUR
    SRC_DIR="${PKG}-${VER}"
    rm -rf "${SRC_DIR}"
    mkdir -p "${SRC_DIR}"
    git archive HEAD | tar -x -C "${SRC_DIR}"
    cp -a third_party "${SRC_DIR}/"
    tar -czf "${TARBALL}" "${SRC_DIR}"
    rm -rf "${SRC_DIR}"
fi

# PKGBUILD with version and the tarball sha256 (model sha256 is a fixed source)
SHA256=$(sha256sum "${TARBALL}" | cut -d' ' -f1)
sed -e "s/^pkgver=.*/pkgver=${VER}/" \
    -e "s/sha256sums=('SKIP'/sha256sums=('${SHA256}'/" \
    aur/PKGBUILD > dist/aur/PKGBUILD

# 模型源随 PKGBUILD 一起交付，本地 makepkg 直接复用并校验（无需联网）
cp third_party/silero-vad/src/silero_vad/data/silero_vad.onnx dist/aur/src/silero_vad.onnx

echo "tarball=${TARBALL}" >> "${GITHUB_OUTPUT}"
echo "pkgbuild=dist/aur/PKGBUILD" >> "${GITHUB_OUTPUT}"
