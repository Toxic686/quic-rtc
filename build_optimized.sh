#!/bin/bash

# 优化编译脚本 - 减少IOPS压力
echo "开始优化编译..."

# 设置环境变量优化编译
export MAKEFLAGS="-j$(nproc)"
export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)

# 创建构建目录
mkdir -p build_optimized
cd build_optimized

# 配置CMake，只构建需要的目标
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DNO_TESTS=ON \
    -DNO_EXAMPLES=OFF \
    -DCMAKE_CXX_FLAGS="-pipe -O1" \
    -DCMAKE_C_FLAGS="-pipe -O1" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# 只构建transport-quic-demo
echo "开始构建transport-quic-demo..."
make transport-quic-demo -j$(nproc)

echo "构建完成！"
echo "可执行文件位置: ./build_optimized/examples/transport-quic/transport-quic-demo" 