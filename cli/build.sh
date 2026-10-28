#!/bin/sh
# 构建 AirScan-QR 终端发送端（静态可执行文件）
#
# 优先用 musl-gcc 静态链接：产物不依赖 glibc/发行版，老内核（2.6.x 起）可直接运行。
# miniz 单独编译：它自带几条无害告警（64 位下的类型范围检查），单独加 -Wno-type-limits
# 屏蔽，避免污染本项目源码的告警输出。

set -e
cd "$(dirname "$0")"

CFLAGS="-O2 -Wall -Wextra -std=c99"
MINIZ_DEFS="-DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_STDIO -Wno-type-limits"

build() {
    cc_cmd="$1"
    shift
    echo "== 编译 miniz（压缩库）=="
    $cc_cmd -c -O2 $MINIZ_DEFS miniz.c -o miniz.o
    echo "== 编译主程序（$cc_cmd）=="
    $cc_cmd $CFLAGS "$@" -o airscan-cli airscan-cli.c qrcodegen.c miniz.o
}

if command -v musl-gcc >/dev/null 2>&1; then
    echo "== 使用 musl-gcc 静态链接 =="
    build musl-gcc -static
elif cc $CFLAGS -static -o /tmp/airscan-probe.$$ -x c /dev/null -lc 2>/dev/null; then
    echo "== 未找到 musl-gcc，使用系统编译器静态链接 =="
    rm -f /tmp/airscan-probe.$$
    build cc -static
else
    echo "== 警告：没有静态链接条件，改用动态链接（产物对老系统兼容性下降） =="
    build cc
fi

echo "== 构建完成 =="
ls -l airscan-cli
if command -v file >/dev/null 2>&1; then file airscan-cli; fi
if command -v ldd >/dev/null 2>&1; then ldd airscan-cli 2>&1 | head -3; fi
