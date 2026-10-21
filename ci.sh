#!/usr/bin/env bash
# 全部构建逻辑都在这里（纯 shell，没有 YAML 缩进的坑）。
# GitHub Actions 由 build.yml 调用；本机 Ubuntu/WSL 也能直接跑： bash ci.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

DIR="."
[ -f Makefile ] || DIR="nx300-lut"
echo "== 环境"
uname -a
echo "repo root: $ROOT"
echo "project dir: $DIR"
ls -la "$ROOT" | head -30

echo
echo "== apt 基础包"
sudo apt-get update -qq || true
sudo apt-get install -y --no-install-recommends \
    build-essential cmake ca-certificates python3-pip || true

echo
echo "== ARM 交叉编译器（自动回退 armel -> armhf）"
CROSS_PKG=""
for p in gcc-arm-linux-gnueabi gcc-arm-linux-gnueabihf; do
    echo "-- 尝试 $p"
    if sudo apt-get install -y --no-install-recommends "$p"; then
        CROSS_PKG="$p"
        break
    fi
done
if [ -z "$CROSS_PKG" ]; then
    echo "!! 没有可用的 ARM 交叉编译器，停止"
    exit 1
fi
CROSS="${CROSS_PKG#gcc-}-"
echo "CROSS=$CROSS"

echo
echo "== libjpeg 开发头文件（自动回退包名）"
JPEG_DEV=""
for p in libjpeg-turbo8-dev libjpeg-dev libjpeg62-turbo-dev; do
    echo "-- 尝试 $p"
    if sudo apt-get install -y --no-install-recommends "$p"; then
        JPEG_DEV="$p"
        break
    fi
done
if [ -z "$JPEG_DEV" ]; then
    echo "!! 没有可用的 libjpeg 开发包，停止"
    exit 1
fi
echo "JPEG_DEV=$JPEG_DEV"

echo
echo "== pillow（只用于冒烟测试，装不上就跳过）"
sudo pip3 install --quiet --break-system-packages pillow || true
if python3 -c "import PIL" 2>/dev/null; then
    SMOKE=1
    echo "pillow ok"
else
    SMOKE=0
    echo "!! pillow 不可用，跳过冒烟测试"
fi

echo
echo "== 交叉编译 libjpeg-turbo"
rm -rf /tmp/ljt "$ROOT/arm-jpeg"
git clone --depth 1 https://github.com/libjpeg-turbo/libjpeg-turbo.git /tmp/ljt
cmake -S /tmp/ljt -B /tmp/ljt/build \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=arm \
    -DCMAKE_C_COMPILER="${CROSS}gcc" \
    -DWITH_SIMD=0 -DENABLE_SHARED=0 -DWITH_TURBOJPEG=0 \
    -DCMAKE_INSTALL_PREFIX="$ROOT/arm-jpeg"
cmake --build /tmp/ljt/build -j"$(nproc)"
cmake --install /tmp/ljt/build

echo
echo "== 本机编译"
( cd "$ROOT/$DIR" && make )

if [ "$SMOKE" = "1" ]; then
    echo
    echo "== 冒烟测试"
    python3 - <<'PY'
from PIL import Image
im = Image.new("RGB", (640, 480))
im.putdata([(x * 255 // 639, y * 255 // 479, 128)
            for y in range(480) for x in range(640)])
im.save("/tmp/in.jpg", quality=92)
print("test image ok")
PY

    python3 - <<'PY' > /tmp/warm.cube
n = 17
print('TITLE "smoke"')
print("LUT_3D_SIZE %d" % n)
for b in range(n):
    for g in range(n):
        for r in range(n):
            R, G, B = r / (n - 1), g / (n - 1), b / (n - 1)
            print("%.6f %.6f %.6f" % (min(1.0, R * 1.06), G, min(1.0, B * 0.9)))
PY

    ( cd "$ROOT/$DIR" && ./lutd --once /tmp/in.jpg /tmp/out.jpg /tmp/warm.cube 95 )
    file /tmp/in.jpg /tmp/out.jpg
    ls -l /tmp/in.jpg /tmp/out.jpg
fi

echo
echo "== 交叉编译 lutd"
( cd "$ROOT/$DIR" && make arm CROSS="$CROSS" JPEG_ARM="$ROOT/arm-jpeg" )
file "$ROOT/$DIR/lutd.arm"
ls -l "$ROOT/$DIR/lutd.arm"

echo
echo "== 完成"
