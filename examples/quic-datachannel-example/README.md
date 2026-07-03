# QUIC DataChannel 示例项目

本目录包含基于 `libdatachannel` 和 `lsquic` 实现的 QUIC DataChannel 完整示例，支持高性能传输测试、拥塞控制算法配置及自动化性能验证。

## 📂 核心文件说明

*   **`webrtc_client.cpp`**
    *   **定位**: 核心命令行工具，功能最全。
    *   **功能**: 支持作为 Offerer/Answerer，支持 QUIC/SCTP 切换，支持配置拥塞控制算法 (Cubic/BBR)，内置完整的性能测试套件 (吞吐量/延迟/并发/稳定性)。
*   **`quic_performance_test.sh`**
    *   **定位**: 自动化性能测试脚本。
    *   **功能**: 一键运行完整的 QUIC 性能基准测试，覆盖不同包大小 (1KB-1MB)、高并发场景及长时间稳定性测试。
*   **`signaling_server.cpp`**
    *   **定位**: 基础信令服务器。
    *   **功能**: 基于 WebSocket 的简单信令交换服务，用于协助 P2P 连接建立。

## 🛠️ 编译指南

本项目已集成 **BoringSSL 自动管理**，您**无需**手动安装 BoringSSL 或 libevent。

```bash
# 在项目根目录执行
# -DENABLE_QUIC=ON 开启 QUIC 支持
# CMake 会自动通过 FetchContent 下载并编译固定版本 BoringSSL
cmake -S . -B build -DENABLE_QUIC=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

编译完成后，会在 `build/examples/quic-datachannel-example/` 目录下生成可执行文件。

## 🚀 快速开始

### 1. 手动运行端到端测试

需要打开三个终端窗口：

**终端 1: 启动信令服务器**
```bash
./examples/quic-datachannel-example/signaling-server
```

**终端 2: 启动接收端 (Answerer)**
```bash
# 语法: webrtc-client <transport_type> answerer <server_ip> <server_port>
./examples/quic-datachannel-example/webrtc-client quic answerer 127.0.0.1 8080
```

**终端 3: 启动发送端 (Offerer)**
```bash
# 语法: webrtc-client <transport_type> offerer <server_ip> <server_port> [options]
# 示例：使用 BBR 拥塞控制算法并发起性能测试套件
./examples/quic-datachannel-example/webrtc-client quic offerer 127.0.0.1 8080 --cc bbr --test-suite
```

### 2. 运行自动化性能测试脚本
这是进行基准测试的推荐方式：

```bash
cd examples/quic-datachannel-example
chmod +x quic_performance_test.sh

# 查看帮助
./quic_performance_test.sh --help

# 运行标准 QUIC 性能测试 (默认使用 Cubic)
./quic_performance_test.sh

# 指定使用 BBR 算法
./quic_performance_test.sh --cc bbr
```

## ⚙️ 高级配置 (C++ API)

`webrtc_client` 展示了如何通过 C++ API 配置 QUIC 参数，不再依赖环境变量：

```cpp
rtc::Configuration config;
config.enableQuicTransport = true;

// 配置拥塞控制算法
// 可选: QuicCongestionControl::Cubic, QuicCongestionControl::BBRv1, QuicCongestionControl::Adaptive
config.quicCongestionControl = rtc::QuicCongestionControl::BBRv1;

// 其他高级参数
config.quicMaxStreamsIn = 100;           // 最大入流数
config.quicMaxStreamsOut = 100;          // 最大出流数
config.quicHandshakeTimeout = std::chrono::seconds(15); // 握手超时
```

## 📊 性能测试包含内容

运行 `--test-suite` 或脚本时，会自动执行以下测试：
1.  **基础连接性**: 验证 QUIC 握手与 DataChannel 打开。
2.  **基础吞吐量**: 1KB, 4KB, 16KB 消息连续发送，计算 Mbps。
3.  **高并发测试**: 大量并发 Stream 同时传输。
4.  **大包传输**: 1MB 以上大消息传输，验证分片与重组性能。
5.  **拥塞控制验证**: 验证 BBR/Cubic 在不同网络条件下的表现。

## ⚠️ 常见问题

1.  **UDP 端口问题**: QUIC 使用 UDP，请确保防火墙允许 UDP 流量。虽然 DataChannel 运行在 DTLS 之上，但底层仍需 ICE 连通。
2.  **MTU 问题**: 为了兼容广域网，代码中已将 QUIC 的 UDP Payload 限制在 1200 字节左右，避免 IP 分片导致的丢包。
3.  **STUN 服务器**: `webrtc_client` 内置了公共 STUN 服务器列表，用于 NAT 穿透。如果内网测试失败，请检查网络是否允许访问外部 STUN。
