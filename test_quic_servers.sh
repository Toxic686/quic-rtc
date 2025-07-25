#!/bin/bash

# QUIC服务器测试脚本
echo "QUIC服务器连接测试"
echo "=================="

# 编译demo
echo "编译transport-quic-demo..."
cd /home/kratos/datachannel
make transport-quic-demo -j2

if [ $? -ne 0 ]; then
    echo "编译失败！"
    exit 1
fi

# 测试服务器列表
servers=(
    "quic.rocks:4433"
    "quic.rocks:443"
    "www.google.com:443"
    "www.cloudflare.com:443"
    "quic.aiortc.org:443"
    "http3-test.litespeedtech.com:443"
    "nghttp2.org:443"
)

# 测试每个服务器
for server in "${servers[@]}"; do
    host=$(echo $server | cut -d: -f1)
    port=$(echo $server | cut -d: -f2)
    
    echo ""
    echo "测试服务器: $server"
    echo "----------------------------------------"
    
    # 修改main.cpp中的服务器地址
    sed -i "s/const char\* target_host = \".*\";/const char* target_host = \"$host\";/" examples/transport-quic/main.cpp
    sed -i "s/const int target_port = [0-9]*;/const int target_port = $port;/" examples/transport-quic/main.cpp
    
    # 重新编译
    make transport-quic-demo -j2
    
    # 运行测试
    timeout 30s ./cmake-build-debug-home/examples/transport-quic/transport-quic-demo
    
    echo "----------------------------------------"
    echo "测试完成: $server"
    echo ""
    
    # 等待一下再测试下一个
    sleep 2
done

echo "所有服务器测试完成！" 