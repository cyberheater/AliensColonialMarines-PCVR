#!/bin/sh
# Cross-compiles d3d9.dll (32-bit) for Aliens: Colonial Marines.
set -e
cd "$(dirname "$0")"
SYS=${SYSROOT:-/home/claude/sysroot}   # mingw-w64 i686 headers + import libraries
DEPS=${DEPS:-/home/claude/deps}        # OpenVR headers (openvr/headers) and Vulkan headers (vulkan)
mkdir -p build
FL="--target=i686-w64-windows-gnu -nostdinc -isystem /usr/lib/llvm-18/lib/clang/18/include -isystem $SYS/include \
 -I$DEPS/openvr/headers -I$DEPS/vulkan -DOPENVR_NO_STL -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 \
 -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-stack-protector -mno-stack-arg-probe \
 -fno-builtin-sinf -fno-builtin-cosf -fno-builtin-atan2f -fno-builtin-sqrtf -fno-builtin-tanf -fno-builtin-atanf \
 -Wall -Wno-unused-function -Wno-misleading-indentation"
clang++ $FL -c src/acmvr.cpp -o build/acmvr.o
ld.lld-18 -m i386pe --shared --exclude-all-symbols -e _DllMain@12 --enable-stdcall-fixup --dynamicbase --nxcompat \
  -o build/d3d9.dll build/acmvr.o src/exports.def -L$SYS/lib -lkernel32 -luser32 -lgdi32 -lmsvcrt -ld3d11
echo built build/d3d9.dll
