/**
 * Copyright (c) 2024 Your Name
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_QUIC_TRANSPORT_H
#define RTC_IMPL_QUIC_TRANSPORT_H

#include "common.hpp"           // 包含通用定义和类型
#include "configuration.hpp"     // 包含配置类定义
#include "global.hpp"           // 包含全局初始化相关
#include "processor.hpp"         // 包含处理器类定义
#include "queue.hpp"            // 包含队列类定义
#include "transport.hpp"         // 包含传输层基类定义

#include <condition_variable>    // 条件变量，用于线程同步
#include <functional>           // 函数对象和绑定
#include <map>                 // 映射容器
#include <mutex>               // 互斥锁
#include <memory>              // 智能指针

#include "lsquic.h"            // lsquic库头文件

namespace rtc::impl {

class QuicTransport final : public Transport, public std::enable_shared_from_this<QuicTransport> {
public:
    // 静态初始化函数 - 初始化lsquic库和BoringSSL
    static void Init();
    // 静态清理函数 - 清理lsquic库资源
    static void Cleanup();

    // 缓冲量回调函数类型定义 - 用于通知上层缓冲数据量变化
    using amount_callback = std::function<void(uint16_t streamId, size_t amount)>;

    // QUIC传输层配置结构体
    struct QuicSettings {
        uint32_t maxStreamsIn = 100;                    // 最大入流数量
        uint32_t maxStreamsOut = 100;                   // 最大出流数量
        uint32_t handshakeTimeout = 60 * 1000 * 1000;  // 握手超时时间(微秒)
        uint32_t idleTimeout = 120 * 1000 * 1000;      // 空闲超时时间(微秒)
        uint32_t pingPeriod = 30 * 1000 * 1000;        // Ping周期(微秒)
        bool supportTcid0 = true;                       // 支持TCID0
        bool supportNstp = true;                        // 支持NSTP
        bool delayedAcks = true;                        // 启用延迟ACK
    };

    // 构造函数 - 创建QUIC传输层对象
    QuicTransport(shared_ptr<Transport> lower, const Configuration &config, 
                  const QuicSettings &settings,
                  message_callback recvCallback, 
                  amount_callback bufferedAmountCallback,
                  state_callback stateChangeCallback);
    // 析构函数 - 清理资源
    ~QuicTransport();

    // 设置缓冲量回调函数
    void onBufferedAmount(amount_callback callback);

    // 启动QUIC传输层
    void start() override;
    // 停止QUIC传输层
    void stop() override;
    // 发送消息 - 重写基类方法
    bool send(message_ptr message) override;
    // 刷新发送队列
    bool flush();
    // 关闭指定流
    void closeStream(unsigned int stream);
    // 关闭连接
    void close();

    // 获取最大流数量
    unsigned int maxStream() const;

    // 统计信息相关方法
    void clearStats();                                // 清除统计信息
    size_t bytesSent();                               // 获取发送字节数
    size_t bytesReceived();                           // 获取接收字节数
    optional<std::chrono::milliseconds> rtt();       // 获取RTT时间

private:
    // QUIC流类型枚举 - 对应SCTP的PPID，用于标识不同类型的流
    enum StreamType : uint32_t {
        STREAM_CONTROL = 50,              // 控制流 - 用于传输控制消息
        STREAM_STRING = 51,               // 字符串流 - 用于传输字符串数据
        STREAM_BINARY_PARTIAL = 52,       // 部分二进制流 - 用于传输部分二进制数据
        STREAM_BINARY = 53,               // 二进制流 - 用于传输完整二进制数据
        STREAM_STRING_PARTIAL = 54,       // 部分字符串流 - 用于传输部分字符串数据
        STREAM_STRING_EMPTY = 56,         // 空字符串流 - 用于传输空字符串
        STREAM_BINARY_EMPTY = 57          // 空二进制流 - 用于传输空二进制数据
    };

    // QUIC连接上下文结构体 - 管理QUIC连接和流
    struct QuicConnCtx {
        lsquic_conn_t *conn;                          // lsquic连接对象指针
        QuicTransport *transport;                      // 指向当前传输层的指针
        std::map<lsquic_stream_t*, uint16_t> streams; // 流到流ID的映射表
        std::map<uint16_t, lsquic_stream_t*> streamIds; // 流ID到流的映射表
        uint16_t nextStreamId = 0;                    // 下一个可用的流ID
        std::mutex streamsMutex;                      // 流映射表的互斥锁，确保线程安全
    };

    // QUIC流上下文结构体 - 管理单个QUIC流的状态
    struct QuicStreamCtx {
        uint16_t streamId;                            // 流ID
        QuicConnCtx *connCtx;                         // 指向连接上下文的指针
        std::vector<uint8_t> buffer;                  // 流数据缓冲区
        bool isOpen = false;                          // 流是否打开标志
        bool isClosed = false;                        // 流是否关闭标志
    };

    // lsquic回调函数声明 - 这些是静态函数，用于与lsquic库交互
    static lsquic_conn_ctx_t *on_new_conn(void *stream_if_ctx, lsquic_conn_t *conn);
    static void on_conn_closed(lsquic_conn_t *conn);
    static lsquic_stream_ctx_t *on_new_stream(void *stream_if_ctx, lsquic_stream_t *stream);
    static void on_stream_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *h);
    static void on_stream_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *h);
    static void on_stream_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *h);
    static int send_packets_out(void *ctx, const struct lsquic_out_spec *specs, unsigned n_specs);
    static void on_hsk_done(lsquic_conn_t *c, enum lsquic_hsk_status s);

    // 私有方法声明
    void connect();                                   // 建立QUIC连接
    void shutdown();                                  // 关闭QUIC连接
    void incoming(message_ptr message) override;      // 处理接收到的消息
    bool outgoing(message_ptr message) override;      // 处理发送的消息

    void doRecv();                                    // 处理接收数据
    void doFlush();                                   // 处理刷新操作
    void enqueueRecv();                               // 将接收操作加入队列
    void enqueueFlush();                              // 将刷新操作加入队列
    bool trySendQueue();                              // 尝试发送队列中的数据
    bool trySendMessage(message_ptr message);         // 尝试发送单个消息
    void updateBufferedAmount(uint16_t streamId, ptrdiff_t delta);  // 更新缓冲量
    void triggerBufferedAmount(uint16_t streamId, size_t amount);   // 触发缓冲量回调
    void sendReset(uint16_t streamId);                // 发送重置流信号

    void processData(binary &&data, uint16_t streamId, StreamType type);  // 处理接收到的数据

    // 成员变量
    const size_t mMaxMessageSize;                     // 最大消息大小限制
    const QuicSettings mSettings;                     // QUIC配置设置
    
    // lsquic引擎和连接相关
    lsquic_engine_t *mEngine;                         // lsquic引擎指针
    lsquic_conn_t *mConn;                             // QUIC连接指针
    std::unique_ptr<QuicConnCtx> mConnCtx;           // 连接上下文智能指针

    // 处理器和队列管理
    Processor mProcessor;                             // 处理器对象，用于异步处理
    std::atomic<int> mPendingRecvCount = 0;          // 待处理接收操作计数
    std::atomic<int> mPendingFlushCount = 0;         // 待处理刷新操作计数
    std::mutex mRecvMutex;                           // 接收操作的互斥锁
    std::recursive_mutex mSendMutex;                 // 发送操作的递归互斥锁
    Queue<message_ptr> mSendQueue;                   // 发送消息队列
    bool mSendShutdown = false;                      // 发送关闭标志
    std::map<uint16_t, size_t> mBufferedAmount;      // 流ID到缓冲量的映射
    amount_callback mBufferedAmountCallback;          // 缓冲量变化回调函数

    // 统计信息
    std::atomic<size_t> mBytesSent = 0;              // 发送字节数统计
    std::atomic<size_t> mBytesReceived = 0;          // 接收字节数统计

    // lsquic相关结构体
    lsquic_stream_if mStreamCallbacks;                // lsquic流回调接口
    lsquic_engine_api mEngineApi;                     // lsquic引擎API
    lsquic_engine_settings mEngineSettings;           // lsquic引擎设置
};

} // namespace rtc::impl

#endif 