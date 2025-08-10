/**
 * Copyright (c) 2024 Your Name
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "quicktransport.hpp"    // 包含QUIC传输层头文件
#include "configuration.hpp"     // 包含配置类头文件
#include "global.hpp"           // 包含全局初始化头文件
#include "utils.hpp"            // 包含工具函数头文件
#include "message.hpp"          // 包含消息类头文件

#include <cstring>              // C字符串操作函数
#include <iostream>             // 输入输出流
#include <sys/socket.h>         // Socket系统调用
#include <netinet/in.h>         // 网络地址结构
#include <arpa/inet.h>          // 网络地址转换函数
#include <openssl/ssl.h>        // OpenSSL库头文件

#include "message.hpp"          // 再次包含消息类头文件

namespace rtc::impl {

// 静态初始化标志 - 用于确保lsquic库只初始化一次
static bool gQuicInitialized = false;

// 静态初始化函数 - 初始化lsquic库和BoringSSL
void QuicTransport::Init() {
    if (gQuicInitialized) return;  // 如果已经初始化，直接返回

    // 初始化BoringSSL库
    SSL_library_init();           // 初始化SSL库
    SSL_load_error_strings();     // 加载SSL错误字符串

    // 初始化lsquic库，设置为客户端模式
    if (lsquic_global_init(LSQUIC_GLOBAL_CLIENT) != 0) {
        throw std::runtime_error("Failed to initialize lsquic library");  // 初始化失败时抛出异常
    }

    gQuicInitialized = true;     // 设置初始化标志为true
}

// 静态清理函数 - 清理lsquic库资源
void QuicTransport::Cleanup() {
    if (!gQuicInitialized) return;  // 如果未初始化，直接返回

    lsquic_global_cleanup();     // 清理lsquic库资源
    EVP_cleanup();               // 清理OpenSSL资源
    gQuicInitialized = false;    // 重置初始化标志
}

// 构造函数 - 创建QUIC传输层对象
QuicTransport::QuicTransport(shared_ptr<Transport> lower, const Configuration &config,
                             const QuicSettings &settings,
                             message_callback recvCallback,
                             amount_callback bufferedAmountCallback,
                             state_callback stateChangeCallback)
    : Transport(lower, stateChangeCallback),  // 调用基类构造函数
      mMaxMessageSize(config.maxMessageSize.value_or(65536)),  // 设置最大消息大小，默认65536字节
      mSettings(settings),  // 保存QUIC设置
      mBufferedAmountCallback(std::move(bufferedAmountCallback)) {  // 移动缓冲量回调函数

    // 设置接收回调函数
    onRecv(std::move(recvCallback));

    // 初始化lsquic引擎设置结构体
    memset(&mEngineSettings, 0, sizeof(mEngineSettings));  // 清零设置结构体
    lsquic_engine_init_settings(&mEngineSettings, 0);      // 使用默认值初始化引擎设置

    // 应用自定义设置到引擎设置中
    mEngineSettings.es_versions = LSQUIC_DF_VERSIONS;      // 使用默认QUIC版本
    mEngineSettings.es_check_tp_sanity = 0;                // 禁用证书验证（仅用于测试）
    mEngineSettings.es_max_streams_in = settings.maxStreamsIn;  // 设置最大入流数量
    mEngineSettings.es_handshake_to = settings.handshakeTimeout;  // 设置握手超时时间
    mEngineSettings.es_idle_conn_to = settings.idleTimeout;      // 设置空闲连接超时时间
    mEngineSettings.es_ping_period = settings.pingPeriod;        // 设置Ping周期
    mEngineSettings.es_support_tcid0 = settings.supportTcid0;    // 设置是否支持TCID0
    mEngineSettings.es_support_nstp = settings.supportNstp;      // 设置是否支持NSTP
    mEngineSettings.es_delayed_acks = settings.delayedAcks;      // 设置是否启用延迟ACK

    // 设置lsquic流回调函数结构体
    mStreamCallbacks = {
        .on_new_conn = on_new_conn,           // 新连接回调函数
        .on_goaway_received = nullptr,        // GOAWAY帧接收回调（未使用）
        .on_conn_closed = on_conn_closed,     // 连接关闭回调函数
        .on_new_stream = on_new_stream,       // 新流创建回调函数
        .on_read = on_stream_read,            // 流读取回调函数
        .on_write = on_stream_write,          // 流写入回调函数
        .on_close = on_stream_close,          // 流关闭回调函数
        .on_dg_write = nullptr,               // 数据报写入回调（未使用）
        .on_datagram = nullptr,               // 数据报接收回调（未使用）
        .on_hsk_done = on_hsk_done,          // 握手完成回调函数
        .on_new_token = nullptr,              // 新令牌回调（未使用）
        .on_sess_resume_info = nullptr,       // 会话恢复信息回调（未使用）
        .on_reset = nullptr,                  // 重置回调（未使用）
        .on_conncloseframe_received = nullptr, // 连接关闭帧接收回调（未使用）
    };

    // 初始化lsquic引擎API结构体
    memset(&mEngineApi, 0, sizeof(mEngineApi));  // 清零API结构体
    mEngineApi.ea_settings = &mEngineSettings;   // 设置引擎设置指针
    mEngineApi.ea_stream_if = &mStreamCallbacks; // 设置流回调接口指针
    mEngineApi.ea_stream_if_ctx = this;          // 设置流回调上下文为当前对象
    mEngineApi.ea_packets_out = send_packets_out; // 设置包发送回调函数
    mEngineApi.ea_packets_out_ctx = this;        // 设置包发送回调上下文
    mEngineApi.ea_alpn = "h3,h3-29,h3-27,h3-25,h3-24,h3-23";  // 设置ALPN协议列表

    // 设置SSL上下文获取函数 - 用于创建TLS连接
    mEngineApi.ea_get_ssl_ctx = [](void *peer_ctx, const struct sockaddr *local) -> struct ssl_ctx_st* {
        std::cout << "Creating SSL context..." << std::endl;  // 输出调试信息

        // 创建TLS方法上下文
        SSL_CTX *ctx = SSL_CTX_new(TLS_method());
        if (!ctx) {
            std::cerr << "Failed to create SSL context" << std::endl;  // 创建失败时输出错误
            return nullptr;
        }

        // 设置TLS版本为TLS1.3
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);  // 设置最小协议版本
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);  // 设置最大协议版本

        // 设置默认验证路径
        SSL_CTX_set_default_verify_paths(ctx);  // 设置默认证书验证路径

        // 设置ALPN协议列表
        const unsigned char alpn_protos[] = {
            8, 'h', '3', '-', '2', '9',    // h3-29协议
            2, 'h', '3',                   // h3协议
        };
        SSL_CTX_set_alpn_protos(ctx, alpn_protos, sizeof(alpn_protos));  // 设置ALPN协议

        // 禁用证书验证（仅用于测试）
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);  // 设置不验证证书

        std::cout << "SSL context created successfully" << std::endl;  // 输出成功信息
        return ctx;  // 返回SSL上下文
    };

    // 创建lsquic引擎
    mEngine = lsquic_engine_new(0, &mEngineApi);  // 创建引擎，0表示客户端模式
    std::cout << "client_ctx->engine=" << mEngine << std::endl;  // 输出引擎指针
    if (!mEngine) {
        throw std::runtime_error("Failed to create lsquic engine");
    }

    // 初始化连接上下文
    mConnCtx = std::make_unique<QuicConnCtx>();  // 创建连接上下文智能指针
    mConnCtx->transport = this;  // 设置传输层指针
    mConnCtx->conn = nullptr;    // 初始化连接指针为nullptr
}

// 析构函数 - 清理资源
QuicTransport::~QuicTransport() {
    if (mConn) {
        lsquic_conn_close(mConn);  // 关闭QUIC连接
    }
    if (mEngine) {
        lsquic_engine_destroy(mEngine);  // 销毁lsquic引擎
    }
}

// 启动QUIC传输层
void QuicTransport::start() {
    if (state() == State::Disconnected) {  // 如果当前状态是断开连接
        changeState(State::Connecting);     // 改变状态为正在连接
        connect();                          // 调用连接函数
    }
}

// 停止QUIC传输层
void QuicTransport::stop() {
    if (mConn) {
        lsquic_conn_close(mConn);  // 关闭QUIC连接
        mConn = nullptr;           // 重置连接指针
    }
    changeState(State::Disconnected);  // 改变状态为断开连接
}

// 发送消息 - 重写基类方法
bool QuicTransport::send(message_ptr message) {
    if (state() != State::Connected) {  // 如果状态不是已连接
        return false;                   // 返回失败
    }

    mSendQueue.push(std::move(message));  // 将消息添加到发送队列
    enqueueFlush();                       // 触发刷新操作
    return true;                          // 返回成功
}

// 刷新发送队列
bool QuicTransport::flush() {
    return trySendQueue();  // 尝试发送队列中的数据
}

// 关闭指定流
void QuicTransport::closeStream(unsigned int stream) {
    if (mConnCtx) {  // 如果连接上下文存在
        std::lock_guard<std::mutex> lock(mConnCtx->streamsMutex);  // 获取流映射表锁
        auto it = mConnCtx->streamIds.find(stream);  // 查找流ID对应的流
        if (it != mConnCtx->streamIds.end()) {  // 如果找到流
            lsquic_stream_close(it->second);     // 关闭QUIC流
            mConnCtx->streams.erase(it->second); // 从流映射表中移除
            mConnCtx->streamIds.erase(it);       // 从流ID映射表中移除
        }
    }
}

// 关闭连接
void QuicTransport::close() {
    if (mConn) {
        lsquic_conn_close(mConn);  // 关闭QUIC连接
    }
    changeState(State::Disconnected);  // 改变状态为断开连接
}

// 获取最大流数量
unsigned int QuicTransport::maxStream() const {
    return mSettings.maxStreamsOut;  // 返回最大出流数量
}

// 清除统计信息
void QuicTransport::clearStats() {
    mBytesSent = 0;      // 重置发送字节数
    mBytesReceived = 0;  // 重置接收字节数
}

// 获取发送字节数
size_t QuicTransport::bytesSent() {
    return mBytesSent.load();  // 返回发送字节数
}

// 获取接收字节数
size_t QuicTransport::bytesReceived() {
    return mBytesReceived.load();  // 返回接收字节数
}

// 获取RTT时间
optional<std::chrono::milliseconds> QuicTransport::rtt() {
    if (mConn) {  // 如果连接存在
        struct lsquic_conn_info info;  // 连接信息结构体
        if (lsquic_conn_get_info(mConn, &info) == 0) {  // 获取连接信息
            return std::chrono::milliseconds(info.lci_rtt / 1000);  // 返回RTT时间（转换为毫秒）
        }
    }
    return std::nullopt;  // 返回空值
}

// 私有方法实现

// 建立QUIC连接
void QuicTransport::connect() {
    // 这里将在有下层传输层时实现
    // 目前只是模拟连接
    changeState(State::Connected);  // 改变状态为已连接
}

// 关闭QUIC连接
void QuicTransport::shutdown() {
    if (mConn) {
        lsquic_conn_close(mConn);  // 关闭QUIC连接
        mConn = nullptr;           // 重置连接指针
    }
}

// 处理接收到的消息
void QuicTransport::incoming(message_ptr message) {
    // 处理从下层传输层接收到的QUIC包
    if (mEngine && message->size() > 0) {  // 如果引擎存在且消息不为空
        // 将消息转换为QUIC包格式
        // 这是一个简化的实现
        struct sockaddr_in local_addr, peer_addr;  // 本地和远程地址结构体
        memset(&local_addr, 0, sizeof(local_addr));  // 清零本地地址
        memset(&peer_addr, 0, sizeof(peer_addr));    // 清零远程地址
        local_addr.sin_family = AF_INET;  // 设置本地地址族为IPv4
        peer_addr.sin_family = AF_INET;   // 设置远程地址族为IPv4

        // 将接收到的数据传递给lsquic引擎处理
        lsquic_engine_packet_in(mEngine, reinterpret_cast<const unsigned char*>(message->data()), message->size(),
                               (struct sockaddr *)&local_addr,
                               (struct sockaddr *)&peer_addr, nullptr, 0);
    }
}

// 处理发送的消息
bool QuicTransport::outgoing(message_ptr message) {
    // 将QUIC包发送到下层传输层
    if (auto lower = getLower()) {  // 获取下层传输层
        return lower->send(std::move(message));  // 发送消息到下层
    }
    return false;  // 如果没有下层传输层，返回失败
}

// 处理接收数据
void QuicTransport::doRecv() {
    // 处理从QUIC流接收到的数据
    // 这将由QUIC回调函数调用
}

// 处理刷新操作
void QuicTransport::doFlush() {
    trySendQueue();  // 尝试发送队列中的数据
}

// 将接收操作加入队列
void QuicTransport::enqueueRecv() {
    if (++mPendingRecvCount == 1) {  // 如果待处理接收计数为1
        mProcessor.enqueue([this]() {  // 将接收操作加入处理器队列
            doRecv();                  // 执行接收操作
            --mPendingRecvCount;       // 减少待处理接收计数
        });
    }
}

// 将刷新操作加入队列
void QuicTransport::enqueueFlush() {
    if (++mPendingFlushCount == 1) {  // 如果待处理刷新计数为1
        mProcessor.enqueue([this]() {  // 将刷新操作加入处理器队列
            doFlush();                 // 执行刷新操作
            --mPendingFlushCount;      // 减少待处理刷新计数
        });
    }
}

// 尝试发送队列中的数据
bool QuicTransport::trySendQueue() {
    std::lock_guard<std::recursive_mutex> lock(mSendMutex);  // 获取发送锁

    while (!mSendQueue.empty() && !mSendShutdown) {  // 当队列不为空且未关闭时
        auto message = mSendQueue.peek();  // 查看队列中的消息
        if (message && trySendMessage(std::move(*message))) {  // 如果消息存在且发送成功
            mSendQueue.pop();  // 从队列中移除消息
        } else {
            break;  // 发送失败时退出循环
        }
    }

    return mSendQueue.empty();  // 返回队列是否为空
}

// 尝试发送单个消息
bool QuicTransport::trySendMessage(message_ptr message) {
    // 通过QUIC流发送消息
    if (mConnCtx && mConnCtx->conn) {  // 如果连接上下文和连接都存在
        // 为这个消息创建一个新流
        lsquic_conn_make_stream(mConnCtx->conn);  // 创建QUIC流

        // 注意：在实际实现中，我们需要跟踪创建的流
        // 目前，我们只是模拟发送消息
        mBytesSent += message->size();  // 增加发送字节数统计
        return true;                    // 返回成功
    }
    return false;  // 返回失败
}

// 更新缓冲量
void QuicTransport::updateBufferedAmount(uint16_t streamId, ptrdiff_t delta) {
    std::lock_guard<std::recursive_mutex> lock(mSendMutex);  // 获取发送锁
    mBufferedAmount[streamId] += delta;  // 更新指定流的缓冲量
    triggerBufferedAmount(streamId, mBufferedAmount[streamId]);  // 触发缓冲量回调
}

// 触发缓冲量回调
void QuicTransport::triggerBufferedAmount(uint16_t streamId, size_t amount) {
    if (mBufferedAmountCallback) {  // 如果缓冲量回调函数存在
        mBufferedAmountCallback(streamId, amount);  // 调用缓冲量回调函数
    }
}

// 发送重置流信号
void QuicTransport::sendReset(uint16_t streamId) {
    if (mConnCtx) {  // 如果连接上下文存在
        std::lock_guard<std::mutex> lock(mConnCtx->streamsMutex);  // 获取流映射表锁
        auto it = mConnCtx->streamIds.find(streamId);  // 查找流ID对应的流
        if (it != mConnCtx->streamIds.end()) {  // 如果找到流
            // 注意：lsquic_stream_reset在这个版本中可能不可用
            // 我们将关闭流作为替代
            lsquic_stream_close(it->second);  // 关闭QUIC流
        }
    }
}

// 处理接收到的数据
void QuicTransport::processData(binary &&data, uint16_t streamId, StreamType type) {
    // 处理接收到的数据并创建适当的消息
    auto message = make_message(data.begin(), data.end(), Message::Type::Binary);  // 创建二进制消息
    recv(std::move(message));  // 向上层传递消息
    mBytesReceived += data.size();  // 增加接收字节数统计
}

// 静态回调函数实现

// 新连接回调
lsquic_conn_ctx_t *QuicTransport::on_new_conn(void *stream_if_ctx, lsquic_conn_t *conn) {
    auto *transport = static_cast<QuicTransport *>(stream_if_ctx);  // 转换为传输层指针
    transport->mConnCtx->conn = conn;  // 设置连接指针
    transport->changeState(State::Connected);  // 改变状态为已连接
    return reinterpret_cast<lsquic_conn_ctx_t *>(transport->mConnCtx.get());  // 返回连接上下文
}

// 连接关闭回调
void QuicTransport::on_conn_closed(lsquic_conn_t *conn) {
    auto *connCtx = reinterpret_cast<QuicConnCtx *>(lsquic_conn_get_ctx(conn));  // 获取连接上下文
    if (connCtx && connCtx->transport) {  // 如果连接上下文和传输层都存在
        connCtx->transport->changeState(State::Disconnected);  // 改变状态为断开连接
    }
}

// 新流回调
lsquic_stream_ctx_t *QuicTransport::on_new_stream(void *stream_if_ctx, lsquic_stream_t *stream) {
    auto *transport = static_cast<QuicTransport *>(stream_if_ctx);  // 转换为传输层指针
    auto *streamCtx = new QuicStreamCtx();  // 创建流上下文
    streamCtx->connCtx = transport->mConnCtx.get();  // 设置连接上下文指针
    streamCtx->streamId = transport->mConnCtx->nextStreamId++;  // 分配流ID

    {
        std::lock_guard<std::mutex> lock(transport->mConnCtx->streamsMutex);  // 获取流映射表锁
        transport->mConnCtx->streams[stream] = streamCtx->streamId;  // 添加流到流ID的映射
        transport->mConnCtx->streamIds[streamCtx->streamId] = stream;  // 添加流ID到流的映射
    }

    return reinterpret_cast<lsquic_stream_ctx_t *>(streamCtx);  // 返回流上下文
}

// 流读取回调
void QuicTransport::on_stream_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    auto *streamCtx = reinterpret_cast<QuicStreamCtx *>(h);  // 转换为流上下文
    unsigned char buf[4096];  // 读取缓冲区
    ssize_t nread = lsquic_stream_read(stream, buf, sizeof(buf));  // 从流中读取数据

    if (nread > 0) {  // 如果读取到数据
        binary data(buf, buf + nread);  // 创建二进制数据
        streamCtx->connCtx->transport->processData(std::move(data), streamCtx->streamId, STREAM_BINARY);  // 处理数据
    }
}

// 流写入回调
void QuicTransport::on_stream_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    // 向流写入数据
    // 这由trySendMessage处理
}

// 流关闭回调
void QuicTransport::on_stream_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    auto *streamCtx = reinterpret_cast<QuicStreamCtx *>(h);  // 转换为流上下文
    if (streamCtx) {  // 如果流上下文存在
        streamCtx->isClosed = true;  // 设置关闭标志
        delete streamCtx;            // 删除流上下文
    }
}

// 包发送回调
int QuicTransport::send_packets_out(void *ctx, const struct lsquic_out_spec *specs, unsigned n_specs) {
    auto *transport = static_cast<QuicTransport *>(ctx);  // 转换为传输层指针

    for (unsigned i = 0; i < n_specs; ++i) {  // 遍历所有包规格
        // 从QUIC包数据创建消息
        size_t total_len = 0;  // 总长度
        for (unsigned j = 0; j < specs[i].iovlen; j++) {  // 计算总长度
            total_len += specs[i].iov[j].iov_len;
        }

        std::vector<uint8_t> packet_data;  // 包数据向量
        packet_data.reserve(total_len);     // 预分配空间

        for (unsigned j = 0; j < specs[i].iovlen; j++) {  // 复制包数据
            const uint8_t *data = static_cast<const uint8_t *>(specs[i].iov[j].iov_base);
            packet_data.insert(packet_data.end(), data, data + specs[i].iov[j].iov_len);
        }

        auto message = make_message(packet_data.begin(), packet_data.end(), Message::Type::Binary);  // 创建消息
        transport->outgoing(std::move(message));  // 发送到下层传输层
    }

    return 0;  // 返回成功
}

// 握手完成回调
void QuicTransport::on_hsk_done(lsquic_conn_t *c, enum lsquic_hsk_status s) {
    auto *connCtx = reinterpret_cast<QuicConnCtx *>(lsquic_conn_get_ctx(c));  // 获取连接上下文
    if (connCtx && connCtx->transport) {  // 如果连接上下文和传输层都存在
        if (s == LSQ_HSK_OK || s == LSQ_HSK_RESUMED_OK) {  // 如果握手成功
            connCtx->transport->changeState(State::Connected);  // 改变状态为已连接
        } else {
            connCtx->transport->changeState(State::Failed);  // 改变状态为失败
        }
    }
}

} // namespace rtc::impl