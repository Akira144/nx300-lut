#!/usr/bin/env bash
# 全部构建逻辑都在这里（纯 shell，没有 YAML 缩进的坑）。
# 在 GitHub Actions 上由 build.yml 调用；也可以在本机 Ubuntu/WSL 里直接跑： bash ci.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

DIR="."
[ -f Makefile ] || DIR="nx300-lut"
echo "== 项目目录: $DIR"

echo "== 安装工具链"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake gcc-arm-linux-gnueabi \
    libjpeg-turbo8-dev python3-pip
sudo pip3 install --quiet --break-system-packages pillow

echo "== 交叉编译 libjpeg-turbo (armel)"
rm -rf /tmp/ljt
git clone --depth 1 https://github.com/libjpeg-turbo/libjpeg-turbo.git /tmp/ljt
cmake -S /tmp/ljt -B /tmp/ljt/build \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=arm \
    -DCMAKE_C_COMPILER=arm-linux-gnueabi-gcc \
    -DWITH_SIMD=0 -DENABLE_SHARED=0 -DWITH_TURBOJPEG=0 \
    -DCMAKE_INSTALL_PREFIX="$ROOT/arm-jpeg"
cmake --build /tmp/ljt/build -j"$(nproc)"
cmake --install /tmp/ljt/build

echo "== 本机编译 + 冒烟测试"
( cd "$ROOT/$DIR" && make )

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

echo "== 交叉编译 lutd"
( cd "$ROOT/$DIR" && make arm CROSS=arm-linux-gnueabi- JPEG_ARM="$ROOT/arm-jpeg" )
file "$ROOT/$DIR/lutd.arm"
ls -l "$ROOT/$DIR/lutd.arm"

echo "== 完成"
