#!/usr/bin/env bash
# 全部构建逻辑都在这里（纯 shell，没有 YAML 缩进的坑）。
# GitHub Actions 由 build.yml 调用；本机 Ubuntu/WSL 也能直接跑： bash ci.sh
#
# 产出（都在项目目录下）：
#   lutd.arm      armel（软浮点），最稳的兜底
#   lutd-hf.arm   armhf + NEON，最快，NX300 上优先使用
#   lutd-musl.arm 可选，给内核低于 3.2 的机器
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

case "$CROSS_PKG" in
    *gnueabihf) CROSS_LIBC=libc6-dev-armhf-cross ;;
    *)          CROSS_LIBC=libc6-dev-armel-cross ;;
esac
echo "-- 安装 $CROSS_LIBC（crt1.o 在里面）"
sudo apt-get install -y "$CROSS_LIBC"

CROSS="${CROSS_PKG#gcc-}-"
echo "CROSS=$CROSS"

echo "-- 验证交叉编译器"
echo 'int main(void){return 0;}' > /tmp/t.c
"${CROSS}gcc" -static -o /tmp/t /tmp/t.c
file /tmp/t
rm -f /tmp/t.c /tmp/t

echo
echo "== libjpeg 开发头文件（本机编译用）"
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
echo "== pillow（只用于冒烟测试）"
sudo pip3 install --quiet --break-system-packages pillow || true
if python3 -c "import PIL" 2>/dev/null; then
    SMOKE=1
    echo "pillow ok"
else
    SMOKE=0
    echo "!! pillow 不可用，跳过冒烟测试"
fi

echo
echo "== 拉取 libjpeg-turbo 源码"
rm -rf /tmp/ljt
git clone --depth 1 https://github.com/libjpeg-turbo/libjpeg-turbo.git /tmp/ljt

echo
echo "== 交叉编译 libjpeg-turbo (armel, 无 SIMD)"
rm -rf "$ROOT/arm-jpeg"
cmake -S /tmp/ljt -B /tmp/ljt/build \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=arm \
    -DCMAKE_C_COMPILER="${CROSS}gcc" \
    -DWITH_SIMD=0 -DENABLE_SHARED=0 -DWITH_TURBOJPEG=0 \
    -DCMAKE_INSTALL_PREFIX="$ROOT/arm-jpeg"
cmake --build /tmp/ljt/build -j"$(nproc)"
cmake --install /tmp/ljt/build

echo
echo "== 本机编译 + 冒烟测试"
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

    ( cd "$ROOT/$DIR" && ./lutd --once /tmp/in.jpg /tmp/out.jpg /tmp/warm.cube 92 )
    file /tmp/in.jpg /tmp/out.jpg
    ls -l /tmp/in.jpg /tmp/out.jpg
fi

echo
echo "== 交叉编译 lutd.arm（armel，兜底）"
( cd "$ROOT/$DIR" && make arm CROSS="$CROSS" JPEG_ARM="$ROOT/arm-jpeg" )
file "$ROOT/$DIR/lutd.arm"
ls -l "$ROOT/$DIR/lutd.arm"

# ---------------------------------------------------------------------------
# armhf + NEON：libjpeg-turbo 的 NEON DCT 比纯 C 快 2-3 倍，编码是瓶颈所在。
# 静态链接，NX300 的 Cortex-A9 有 NEON/VFPv3，硬浮点 ABI 的静态二进制照样能跑。
# 失败不影响上面产出的 lutd.arm。
# ---------------------------------------------------------------------------
build_neon() {
    echo
    echo "== armhf + NEON 工具链"
    sudo apt-get install -y --no-install-recommends \
        gcc-arm-linux-gnueabihf libc6-dev-armhf-cross
    echo 'int main(void){return 0;}' > /tmp/t3.c
    arm-linux-gnueabihf-gcc -march=armv7-a -mfpu=neon -static -o /tmp/t3 /tmp/t3.c
    file /tmp/t3
    rm -f /tmp/t3.c /tmp/t3

    echo
    echo "== libjpeg-turbo (armhf + SIMD)"
    rm -rf /tmp/ljt3 "$ROOT/arm-jpeg-hf"
    cmake -S /tmp/ljt -B /tmp/ljt3 \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=arm \
        -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
        -DCMAKE_C_FLAGS="-march=armv7-a -mfpu=neon" \
        -DWITH_SIMD=1 -DENABLE_SHARED=0 -DWITH_TURBOJPEG=0 \
        -DCMAKE_INSTALL_PREFIX="$ROOT/arm-jpeg-hf" 2>&1 | tee /tmp/cmake-hf.log
    echo "-- SIMD 探测结果："
    grep -iE "simd|neon" /tmp/cmake-hf.log || echo "(cmake 没提 SIMD)"
    cmake --build /tmp/ljt3 -j"$(nproc)"
    cmake --install /tmp/ljt3

    echo "-- 确认 libjpeg 里真的有 NEON 符号"
    arm-linux-gnueabihf-nm -g "$ROOT/arm-jpeg-hf/lib/libjpeg.a" 2>/dev/null \
        | grep -ci "jsimd.*neon" || echo "(没找到，SIMD 可能没启用)"

    echo
    echo "== 编译 lutd-hf.arm（NEON）"
    ( cd "$ROOT/$DIR" && arm-linux-gnueabihf-gcc -O2 -Wall \
        -march=armv7-a -mfpu=neon -static \
        -I"$ROOT/arm-jpeg-hf/include" -o lutd-hf.arm src/lutd.c \
        -L"$ROOT/arm-jpeg-hf/lib" -ljpeg )
    file "$ROOT/$DIR/lutd-hf.arm"
    ls -l "$ROOT/$DIR/lutd-hf.arm"
}

echo
echo "== 可选：armhf + NEON 版（更快）"
if build_neon; then
    echo "neon ok"
else
    echo "!! NEON 版构建失败，跳过（armel 版仍然可用）"
fi

echo
echo "== 可选：musl 静态版（老内核兜底）"
if [ -f "$ROOT/musl.sh" ]; then
    if bash "$ROOT/musl.sh"; then
        echo "musl ok"
    else
        echo "!! musl 构建失败，跳过"
    fi
else
    echo "没有 musl.sh，跳过"
fi

echo
echo "== 完成"
ls -l "$ROOT/$DIR"/lutd*.arm 2>/dev/null || true
