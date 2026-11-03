#!/bin/sh
# Builds acmvr_launcher.exe (32-bit, to match the game).
set -e
cd "$(dirname "$0")"
SYS=${SYSROOT:-/home/claude/sysroot}   # mingw-w64 i686 headers + import libraries
DEPS=${DEPS:-/home/claude/deps}        # OpenVR headers (openvr/headers) and Vulkan headers (vulkan)
mkdir -p build
clang++ --target=i686-w64-windows-gnu -nostdinc -isystem /usr/lib/llvm-18/lib/clang/18/include -isystem $SYS/include \
 -I$DEPS/openvr/headers -DOPENVR_NO_STL -D__USE_MINGW_ANSI_STDIO=0 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -O2 -fno-exceptions -fno-rtti \
 -fno-threadsafe-statics -fno-stack-protector -mno-stack-arg-probe -Wall -Wno-unused-function -c src/launcher.cpp -o build/launcher.o
(cd src && llvm-windres-18 --target=pe-i386 launcher.rc -o ../build/launcher_res.o)
ld.lld-18 -m i386pe --subsystem windows -e _LauncherMain@0 --dynamicbase --nxcompat \
  -o build/acmvr_launcher.exe build/launcher.o build/launcher_res.o -L$SYS/lib -lkernel32 -luser32 -lgdi32 -lmsvcrt
echo built build/acmvr_launcher.exe
