# QUIC DataChannel 端到端测试示例

这个目录包含了QUIC DataChannel的端到端测试示例，用于验证QUIC传输协议在WebRTC中的实际效果。

## 文件说明

### 核心文件
- **`simple_example.cpp`** - 简化的QUIC DataChannel示例（推荐使用）
  - 不依赖外部WebSocket或JSON库
  - 适合快速验证QUIC功能
  - 包含基本的消息发送/接收和性能测试

- **`signaling_server.cpp`** - WebSocket信令服务器
  - 处理WebRTC的offer/answer交换
  - 处理ICE候选者交换
  - 使用libdatachannel内置的WebSocket实现

- **`webrtc_client.cpp`** - 完整的WebRTC客户端
  - 支持QUIC和SCTP两种传输方式
  - 支持发起方和应答方角色
  - 使用libdatachannel内置的WebSocket实现

### 测试脚本
- **`test_message_sizes.sh`** - 不同消息大小性能测试脚本
- **`test_concurrent_connections.sh`** - 并发连接测试脚本
- **`test_quic_vs_sctp.sh`** - QUIC vs SCTP性能对比测试脚本
- **`run_all_tests.sh`** - 运行所有测试的主脚本

## 编译和运行

### 1. 编译项目
```bash
# 在项目根目录
mkdir build
cd build
cmake .. -DENABLE_QUIC=ON
make
```

### 2. 运行简化示例（推荐）
```bash
# 运行简化示例，验证QUIC功能
./examples/quic-datachannel-example/simple-quic-example
```

### 3. 运行端到端测试

#### 步骤1：启动信令服务器
```bash
# 在一个终端中启动信令服务器
./examples/quic-datachannel-example/signaling-server
```

#### 步骤2：启动QUIC发起方
```bash
# 在另一个终端中启动QUIC发起方
./examples/quic-datachannel-example/webrtc-client quic offerer
```

#### 步骤3：启动QUIC应答方
```bash
# 在第三个终端中启动QUIC应答方
./examples/quic-datachannel-example/webrtc-client quic answerer
```

### 4. 测试不同场景

#### QUIC vs QUIC
```bash
# 发起方使用QUIC
./examples/quic-datachannel-example/webrtc-client quic offerer

# 应答方使用QUIC
./examples/quic-datachannel-example/webrtc-client quic answerer
```

#### SCTP vs SCTP
```bash
# 发起方使用SCTP
./examples/quic-datachannel-example/webrtc-client sctp offerer

# 应答方使用SCTP
./examples/quic-datachannel-example/webrtc-client sctp answerer
```

#### QUIC vs SCTP（混合测试）
```bash
# 发起方使用QUIC
./examples/quic-datachannel-example/webrtc-client quic offerer

# 应答方使用SCTP
./examples/quic-datachannel-example/webrtc-client sctp answerer
```

## 高级测试套件

### 1. 不同消息大小性能测试

测试不同消息大小对传输性能的影响。

#### 运行测试
```bash
# 给脚本执行权限
chmod +x examples/quic-datachannel-example/test_message_sizes.sh

# 运行测试
./examples/quic-datachannel-example/test_message_sizes.sh
```

#### 测试内容
- **消息大小**：64B, 256B, 1KB, 4KB, 16KB, 64KB, 256KB, 1MB
- **消息数量**：每种大小发送1000条消息
- **传输方式**：QUIC和SCTP
- **测试指标**：吞吐量、延迟、成功率

#### 输出结果
```
=== 消息大小性能测试结果 ===
消息大小: 64B
  QUIC: 吞吐量=12.5 Mbps, 平均延迟=0.8ms, 成功率=100%
  SCTP: 吞吐量=11.2 Mbps, 平均延迟=1.2ms, 成功率=100%

消息大小: 1KB
  QUIC: 吞吐量=156.8 Mbps, 平均延迟=2.1ms, 成功率=100%
  SCTP: 吞吐量=142.3 Mbps, 平均延迟=2.8ms, 成功率=100%

...
```

### 2. 并发连接测试

测试多个并发连接的性能和稳定性。

#### 运行测试
```bash
# 给脚本执行权限
chmod +x examples/quic-datachannel-example/test_concurrent_connections.sh

# 运行测试
./examples/quic-datachannel-example/test_concurrent_connections.sh
```

#### 测试内容
- **并发连接数**：1, 2, 4, 8, 16, 32
- **每个连接**：发送1000条1KB消息
- **传输方式**：QUIC和SCTP
- **测试指标**：总吞吐量、连接建立时间、内存使用

#### 输出结果
```
=== 并发连接测试结果 ===
并发连接数: 1
  QUIC: 总吞吐量=156.8 Mbps, 连接时间=45ms, 内存=12MB
  SCTP: 总吞吐量=142.3 Mbps, 连接时间=52ms, 内存=11MB

并发连接数: 8
  QUIC: 总吞吐量=1.2 Gbps, 连接时间=180ms, 内存=85MB
  SCTP: 总吞吐量=1.1 Gbps, 连接时间=220ms, 内存=78MB

...
```

### 3. QUIC vs SCTP性能对比测试

全面对比QUIC和SCTP的性能差异。

#### 运行测试
```bash
# 给脚本执行权限
chmod +x examples/quic-datachannel-example/test_quic_vs_sctp.sh

# 运行测试
./examples/quic-datachannel-example/test_quic_vs_sctp.sh
```

#### 测试内容
- **基础性能**：吞吐量、延迟、CPU使用率
- **连接特性**：连接建立时间、重连时间
- **网络适应性**：丢包率影响、带宽限制影响
- **资源使用**：内存使用、CPU使用

#### 输出结果
```
=== QUIC vs SCTP 性能对比 ===
基础性能测试:
  QUIC:  吞吐量=156.8 Mbps, 延迟=2.1ms, CPU=15%
  SCTP:  吞吐量=142.3 Mbps, 延迟=2.8ms, CPU=18%

连接特性测试:
  QUIC:  连接时间=45ms, 重连时间=12ms
  SCTP:  连接时间=52ms, 重连时间=18ms

网络适应性测试:
  丢包率5%时:
    QUIC:  吞吐量=148.2 Mbps (-5.5%)
    SCTP:  吞吐量=125.6 Mbps (-11.7%)

资源使用测试:
  QUIC:  内存=12MB, CPU峰值=25%
  SCTP:  内存=11MB, CPU峰值=30%
```

### 4. 运行所有测试

一次性运行所有测试并生成综合报告。

#### 运行所有测试
```bash
# 给脚本执行权限
chmod +x examples/quic-datachannel-example/run_all_tests.sh

# 运行所有测试
./examples/quic-datachannel-example/run_all_tests.sh
```

#### 输出结果
```
=== 综合测试报告 ===
测试时间: 2025-08-24 15:45:00
测试环境: Linux x86_64, 8核CPU, 16GB内存

1. 消息大小性能测试: ✅ 通过
   - 测试了8种消息大小
   - QUIC平均性能提升12.3%
   - 详细结果: results/message_sizes_20250824_154500.json

2. 并发连接测试: ✅ 通过
   - 测试了6种并发级别
   - QUIC并发性能提升8.7%
   - 详细结果: results/concurrent_connections_20250824_154500.json

3. QUIC vs SCTP对比测试: ✅ 通过
   - 全面性能对比完成
   - QUIC综合性能提升10.2%
   - 详细结果: results/quic_vs_sctp_20250824_154500.json

综合结论:
- QUIC在大多数场景下性能优于SCTP
- 小消息场景下QUIC优势明显
- 并发场景下QUIC扩展性更好
- 建议在生产环境中优先考虑QUIC
```

## 预期输出

### 简化示例输出
```
=== 简化QUIC DataChannel示例 ===
创建QUIC PeerConnection...
创建QUIC数据通道...
QUIC DataChannel示例已启动

=== 性能测试结果 ===
传输方式: QUIC
发送消息数: 1000
消息大小: 1024 字节
总时间: 45 毫秒
平均每条消息: 0.45 毫秒
吞吐量: 182.22 Mbps

示例完成
```

### 端到端测试输出
```
=== WebRTC QUIC 客户端 ===
角色: 发起方
连接到信令服务器: ws://localhost:8080
WebSocket连接已建立
使用QUIC传输
创建offer: v=0 o=- 1234567890 2 IN IP4 127.0.0.1...
数据通道已打开，开始发送消息...
已发送QUIC字符串消息: Hello from QUIC DataChannel!
已发送QUIC二进制消息，大小: 5 字节

=== 性能测试结果 ===
传输方式: QUIC
发送消息数: 1000
消息大小: 1024 字节
总时间: 45 毫秒
平均每条消息: 0.045 毫秒
吞吐量: 182.22 Mbps
```

## 配置选项

### QUIC配置
```cpp
rtc::Configuration config;
config.enableQuicTransport = true;
config.quicMaxStreamsIn = 100;
config.quicMaxStreamsOut = 100;
config.quicHandshakeTimeout = std::chrono::milliseconds(60000);
config.quicIdleTimeout = std::chrono::milliseconds(120000);
config.quicPingPeriod = std::chrono::milliseconds(30000);
```

### 数据通道配置
```cpp
auto dc = pc->createDataChannel("test", {
    .reliability = rtc::Reliability::Reliable,
    .protocol = "quic-protocol"
});
```

## 故障排除

### 1. 编译错误
- 确保在项目根目录下编译
- 检查lsquic库是否正确集成
- 检查OpenSSL是否正确安装

### 2. 运行时错误
- 确保所有依赖库都已正确链接
- 检查QUIC功能是否已启用
- 检查WebSocket功能是否已启用

### 3. 连接问题
- 确保信令服务器正在运行
- 检查端口8080是否被占用
- 检查防火墙设置

## 技术细节

### lsquic库集成
- **位置**：`deps/lsquic/` - 项目内部的lsquic源码
- **头文件**：`deps/lsquic/include/` - lsquic头文件
- **链接方式**：直接链接 `lsquic` 目标

### WebSocket集成
- **使用**：libdatachannel内置的WebSocket实现
- **客户端**：`rtc::WebSocket` 类
- **服务器**：`rtc::WebSocketServer` 类

### JSON库集成
- **使用**：libdatachannel内置的nlohmann/json库
- **位置**：`deps/json/` - 项目内部的json源码
- **链接方式**：使用 `nlohmann_json::nlohmann_json` 目标

## 下一步

1. **运行简化示例**：验证QUIC基本功能
2. **运行端到端测试**：验证真实网络连接
3. **运行消息大小测试**：了解不同消息大小的性能特征
4. **运行并发连接测试**：验证多连接场景的性能
5. **运行QUIC vs SCTP对比测试**：全面了解性能差异
6. **分析测试结果**：根据实际需求选择最佳传输协议

## 相关文档

- [QUIC集成文档](../QUIC_INTEGRATION.md)
- [主项目文档](../../README.md)
- [transport-quic示例](../transport-quic/) - lsquic集成参考
- [client示例](../client/) - WebSocket和JSON使用参考 