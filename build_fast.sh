#!/bin/bash

# 快速构建脚本 - 优化IOPS和编译速度
echo "开始快速构建..."

# 设置环境变量优化编译
export MAKEFLAGS="-j$(nproc)"
export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)

# 创建构建目录
mkdir -p build_fast
cd build_fast

# 配置CMake，优化编译设置
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DNO_TESTS=ON \
    -DNO_EXAMPLES=OFF \
    -DCMAKE_CXX_FLAGS="-pipe -O2 -fno-verbose-asm" \
    -DCMAKE_C_FLAGS="-pipe -O2 -fno-verbose-asm" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache

# 只构建transport-quic-demo，限制并行数量
echo "开始构建transport-quic-demo..."
make transport-quic-demo -j2

echo "构建完成！"
echo "可执行文件位置: ./build_fast/examples/transport-quic/transport-quic-demo" 