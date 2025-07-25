#!/bin/bash

# 单个QUIC服务器测试脚本
echo "QUIC服务器连接测试"
echo "=================="

# 检查参数
if [ $# -ne 2 ]; then
    echo "用法: $0 <host> <port>"
    echo ""
    echo "可用的测试服务器:"
    echo "  quic.rocks:4433 (官方QUIC测试服务器)"
    echo "  quic.rocks:443 (标准HTTPS端口)"
    echo "  www.google.com:443 (Google QUIC)"
    echo "  www.cloudflare.com:443 (Cloudflare QUIC)"
    echo "  quic.aiortc.org:443 (AIO RTC QUIC)"
    echo "  http3-test.litespeedtech.com:443 (LiteSpeed HTTP/3)"
    echo "  nghttp2.org:443 (nghttp2 HTTP/3)"
    echo ""
    echo "示例: $0 quic.rocks 4433"
    exit 1
fi

HOST=$1
PORT=$2

echo "测试服务器: $HOST:$PORT"
echo ""

# 编译demo
echo "编译transport-quic-demo..."
cd /home/kratos/datachannel
make transport-quic-demo -j2

if [ $? -ne 0 ]; then
    echo "编译失败！"
    exit 1
fi

# 修改main.cpp中的服务器地址
echo "修改目标服务器为: $HOST:$PORT"
sed -i "s/const char\* target_host = \".*\";/const char* target_host = \"$HOST\";/" examples/transport-quic/main.cpp
sed -i "s/const int target_port = [0-9]*;/const int target_port = $PORT;/" examples/transport-quic/main.cpp

# 重新编译
echo "重新编译..."
make transport-quic-demo -j2

# 运行测试
echo "开始测试..."
timeout 30s ./cmake-build-debug-home/examples/transport-quic/transport-quic-demo

echo ""
echo "测试完成: $HOST:$PORT" 