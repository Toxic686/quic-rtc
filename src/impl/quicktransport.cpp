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
#include "certificate.hpp"      // 复用已有DTLS证书生成
#include "utils.hpp"            // 包含工具函数头文件
#include "message.hpp"          // 包含消息类头文件
#include "threadpool.hpp"        // 包含线程池定义，用于定期处理连接

#include <cstring>              // C字符串操作函数
#include <iostream>             // 输入输出流
#include <sys/socket.h>         // Socket系统调用
#include <netinet/in.h>         // 网络地址结构
#include <arpa/inet.h>          // 网络地址转换函数
#include <openssl/ssl.h>        // OpenSSL库头文件
#include <cstdio>               // FILE* 与 fwrite
#include <mutex>                // once_flag
#include <cerrno>               // errno
#include <string>
#include <cctype>
#include <cstdlib>

// 固定使用的 ALPN 协议串
static const unsigned char kQuicAlpn[] = {11, 'w','e','b','r','t','c','-','q','u','i','c'};

// ALPN 选择回调（服务端侧）
static int select_alpn_cb(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                          const unsigned char *in, unsigned int inlen, void *arg) {
    (void)ssl; (void)arg;
    // 使用 OpenSSL/BoringSSL 工具函数选择匹配的 ALPN
    if (SSL_select_next_proto(const_cast<unsigned char **>(out), outlen,
                              kQuicAlpn, sizeof(kQuicAlpn), in, inlen) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_NOACK;
}
#include <chrono>               // 时间相关功能

#include "message.hpp"          // 再次包含消息类头文件

namespace rtc::impl {

namespace {
// 通过环境变量控制 QUIC 的“包级别”trace日志（默认关闭）
// - RTC_QUIC_TRACE=1/true/on/yes  : 打开
// - 其他/未设置                  : 关闭
bool quic_trace_enabled() {
    static const bool enabled = []() -> bool {
        const char *v = std::getenv("RTC_QUIC_TRACE");
        if (!v) return false;
        std::string s(v);
        for (auto &c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s == "1" || s == "true" || s == "on" || s == "yes";
    }();
    return enabled;
}
} // namespace

// 静态初始化标志 - 用于确保lsquic库只初始化一次
static bool gQuicInitialized = false;

// 静态初始化函数 - 初始化lsquic库和BoringSSL
void QuicTransport::Init() {
    if (gQuicInitialized) return;  // 如果已经初始化，直接返回

    // 初始化BoringSSL库
    SSL_library_init();           // 初始化SSL库
    SSL_load_error_strings();     // 加载SSL错误字符串

    // 初始化lsquic日志，使环境变量 LSQUIC_LOG/LSQUIC_LOG_LEVEL 生效
    static bool logInitialized = false;
    if (!logInitialized) {
        // 简单的stderr日志回调
        static lsquic_logger_if logger_if = {
            // 返回0表示成功
            .log_buf = [](void *logger_ctx, const char *buf, size_t len) -> int {
                auto *f = static_cast<FILE *>(logger_ctx);
                if (!f) return -1;
                const size_t n = fwrite(buf, 1, len, f);
                fflush(f);
                return n == len ? 0 : -1;
            },
        };

        lsquic_logger_init(&logger_if, stderr, LLTS_HHMMSSMS);
        if (const char *lvl = std::getenv("LSQUIC_LOG_LEVEL"))
            lsquic_set_log_level(lvl);
        if (const char *opt = std::getenv("LSQUIC_LOG"))
            lsquic_logger_lopt(opt);
        logInitialized = true;
    }

    // 初始化lsquic库
    // 在WebRTC中，QUIC应该是对称的，两端都需要能够接收和发送数据
    // 使用LSQUIC_GLOBAL_CLIENT模式，但引擎创建时使用服务器模式
    if (lsquic_global_init(LSQUIC_GLOBAL_CLIENT) != 0) {
        throw std::runtime_error("Failed to initialize lsquic library");
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
                             state_callback stateChangeCallback,
                             bool isClient)
    : Transport(lower, stateChangeCallback),  // 调用基类构造函数
      mMaxMessageSize(config.maxMessageSize.value_or(65536)),  // 设置最大消息大小，默认65536字节
      mSettings(settings),  // 保存QUIC设置
      mIsClient(isClient),  // 保存客户端模式标志（必须在mBufferedAmountCallback之前初始化）
      mBufferedAmountCallback(std::move(bufferedAmountCallback)) {  // 移动缓冲量回调函数

    // 设置接收回调函数
    onRecv(std::move(recvCallback));

    // 初始化lsquic引擎设置结构体
    memset(&mEngineSettings, 0, sizeof(mEngineSettings));
    
    // WebRTC中QUIC运行在DTLS之上
    // offerer (WebRTC角色): 使用CLIENT模式，主动发起连接
    // answerer (WebRTC角色): 使用SERVER模式，等待接收连接
    
    // 根据WebRTC角色决定lsquic模式
    unsigned engine_flags = isClient ? 0 : 1;  // 0 = LSENG_CLIENT, 1 = LSENG_SERVER
    lsquic_engine_init_settings(&mEngineSettings, engine_flags);
    
    // 应用自定义设置到引擎设置中
    mEngineSettings.es_versions = LSQUIC_DF_VERSIONS;      // 使用默认QUIC版本
    mEngineSettings.es_check_tp_sanity = 0;                // 禁用证书验证（仅用于测试）
    mEngineSettings.es_max_streams_in = settings.maxStreamsIn;   // 设置最大入流数量（对端可开入流）
    // 出流配额：使用 IETF 初始流上限参数，确保本端可主动开双向/单向流
    mEngineSettings.es_init_max_streams_bidi = settings.maxStreamsOut;
    mEngineSettings.es_init_max_streams_uni  = settings.maxStreamsOut;
    
    // 在服务器模式下，es_handshake_to 不能超过约16秒（MAX_MINI_CONN_LIFESPAN_IN_USEC）
    const unsigned long MAX_SERVER_HANDSHAKE_TIMEOUT = 15 * 1000 * 1000;
    if (settings.handshakeTimeout > MAX_SERVER_HANDSHAKE_TIMEOUT) {
        mEngineSettings.es_handshake_to = MAX_SERVER_HANDSHAKE_TIMEOUT;
    } else {
        mEngineSettings.es_handshake_to = settings.handshakeTimeout;
    }
    
    // 注意：es_idle_conn_to的单位是秒，不是毫秒
    mEngineSettings.es_idle_conn_to = settings.idleTimeout / 1000;
    mEngineSettings.es_ping_period = settings.pingPeriod;
    mEngineSettings.es_support_tcid0 = settings.supportTcid0;
    mEngineSettings.es_support_nstp = settings.supportNstp;
    mEngineSettings.es_delayed_acks = settings.delayedAcks;
    
    // 收敛 MTU：
    // - QUIC 要求 Initial 的 UDP payload >= 1200
    // - IPv4 上应设置为 1200 + 28 = 1228
    static constexpr unsigned short kQuicMinInitialUdpPayload = 1200;
    static constexpr unsigned short kIpv4UdpIpOverhead = 28;
    mEngineSettings.es_base_plpmtu = kQuicMinInitialUdpPayload + kIpv4UdpIpOverhead; // 1228

    // 拥塞控制算法配置
    // 0: Default (Adaptive), 1: Cubic, 2: BBRv1, 3: Adaptive
    switch (mSettings.congestionControl) {
    case QuicCongestionControl::Cubic:
        mEngineSettings.es_cc_algo = 1;
        break;
    case QuicCongestionControl::BBRv1:
        mEngineSettings.es_cc_algo = 2;
        break;
    case QuicCongestionControl::Adaptive:
        mEngineSettings.es_cc_algo = 3;
        break;
    case QuicCongestionControl::Default:
    default:
        // 兼容：如果未设置（Default），尝试读取环境变量作为后备
        if (const char *cc = std::getenv("RTC_QUIC_CC_ALGO")) {
            std::string s(cc);
            if (s == "bbr" || s == "BBR" || s == "2") {
                mEngineSettings.es_cc_algo = 2; // BBRv1
            } else if (s == "cubic" || s == "CUBIC" || s == "1") {
                mEngineSettings.es_cc_algo = 1; // Cubic
            } else {
                mEngineSettings.es_cc_algo = 3; // Adaptive
            }
        } else {
            mEngineSettings.es_cc_algo = 3; // Default to Adaptive
        }
        break;
    }

    // 禁用 DPLPMTUD 的“探测增大”，防止叠加外层开销后超 MTU
    mEngineSettings.es_dplpmtud = 0;
    mEngineSettings.es_max_plpmtu = mEngineSettings.es_base_plpmtu;
    // 向对端宣告：我们最多愿意接收的 QUIC UDP payload
    mEngineSettings.es_max_udp_payload_size_rx = kQuicMinInitialUdpPayload;
    
    // 启用 Datagram 扩展 (RFC 9221)
    mEngineSettings.es_datagrams = 1;

    // BBRv1 Tuning
    if (settings.bbrMinRttExpiry.has_value())
        mEngineSettings.es_bbr_min_rtt_expiry = settings.bbrMinRttExpiry.value();
    if (settings.bbrInitCwnd.has_value())
        mEngineSettings.es_bbr_init_cwnd = settings.bbrInitCwnd.value();
    if (settings.bbrCwndGain.has_value())
        mEngineSettings.es_bbr_cwnd_gain = settings.bbrCwndGain.value();
    if (settings.bbrPacingGain.has_value())
        mEngineSettings.es_bbr_pacing_gain = settings.bbrPacingGain.value();

    // 设置lsquic流回调函数结构体
    mStreamCallbacks = {
        .on_new_conn = on_new_conn,           // 新连接回调函数
        .on_goaway_received = nullptr,        // GOAWAY帧接收回调（未使用）
        .on_conn_closed = on_conn_closed,     // 连接关闭回调函数
        .on_new_stream = on_new_stream,       // 新流创建回调函数
        .on_read = on_stream_read,            // 流读取回调函数
        .on_write = on_stream_write,          // 流写入回调函数
        .on_close = on_stream_close,          // 流关闭回调函数
        .on_dg_write = on_dg_write,           // 数据报写入回调
        .on_datagram = on_datagram,           // 数据报接收回调
        .on_hsk_done = on_hsk_done,           // 握手完成回调函数
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
    
    // 关键：必须设置ea_alpn，两端必须一致
    // ALPN用于协商应用协议，即使不是HTTP/3也需要设置
    static const char alpn_str[] = "webrtc-quic";  // 两端使用相同的ALPN字符串
    mEngineApi.ea_alpn = alpn_str;

    // 设置SSL上下文获取函数 - 用于创建TLS连接
    // 关键：这个回调必须正确返回有效的SSL_CTX，否则lsquic无法进行TLS握手
    mEngineApi.ea_get_ssl_ctx = [](void *peer_ctx, const struct sockaddr *local) -> struct ssl_ctx_st* {
        (void)peer_ctx; 
        (void)local;    
        
        // 创建TLS方法上下文
        SSL_CTX *ctx = SSL_CTX_new(TLS_method());
        if (!ctx) {
            return nullptr;
        }

        // 设置TLS版本为TLS1.3
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

        // 设置默认验证路径
        SSL_CTX_set_default_verify_paths(ctx);

        // 设置 ALPN 与 ea_alpn 一致（客户端发送 & 服务端选择）
        SSL_CTX_set_alpn_protos(ctx, kQuicAlpn, sizeof(kQuicAlpn));
        SSL_CTX_set_alpn_select_cb(ctx, select_alpn_cb, nullptr);

        // 生成并加载自签名证书（复用现有证书生成逻辑）
        static std::once_flag cert_once;
        static certificate_ptr quic_cert;
        std::call_once(cert_once, []() {
            quic_cert = std::make_shared<Certificate>(
                Certificate::Generate(CertificateType::Default, "webrtc-quic"));
        });
        if (quic_cert) {
            auto [x509, pkey] = quic_cert->credentials();
            SSL_CTX_use_certificate(ctx, x509);
            SSL_CTX_use_PrivateKey(ctx, pkey);
        }

        // 禁用证书验证（仅用于测试）
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

        return ctx;
    };

    // 创建lsquic引擎
    // 在WebRTC中，QUIC运行在DTLS之上，通过回调实现包收发，不依赖真实UDP socket
    
    // 使用lsquic_engine_check_settings检查设置
    char err_buf[256];
    int check_result = lsquic_engine_check_settings(&mEngineSettings, engine_flags, err_buf, sizeof(err_buf));
    if (check_result != 0) {
        throw std::runtime_error(std::string("Engine settings check failed: ") + err_buf);
    }
    
    mEngine = lsquic_engine_new(engine_flags, &mEngineApi);
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
    // 先调用基类的start()来注册接收回调
    auto lower = getLower();
    
    Transport::start();
    
    if (state() == State::Disconnected) {  // 如果当前状态是断开连接
        changeState(State::Connecting);     // 改变状态为正在连接
        connect();                          // 调用连接函数
    }
}

// 停止QUIC传输层
void QuicTransport::stop() {
    stopPeriodicProcessing();  // 停止定期处理
    
    if (mConn) {
        lsquic_conn_close(mConn);  // 关闭QUIC连接
        mConn = nullptr;           // 重置连接指针
    }
    changeState(State::Disconnected);  // 改变状态为断开连接
}

// 发送消息 - 重写基类方法
bool QuicTransport::send(message_ptr message) {
    // 在CLIENT模式下，允许在Connecting状态时也发送数据，以触发连接建立
    // 在SERVER模式下，只有在Connected状态时才能发送数据
    State currentState = state();
    if (mIsClient) {
        // CLIENT模式：允许在Connecting或Connected状态时发送数据
        if (currentState != State::Connecting && currentState != State::Connected) {
            return false;  // 返回失败
        }
    } else {
        // SERVER模式：只有在Connected状态时才能发送数据
        if (currentState != State::Connected) {
            return false;  // 返回失败
        }
    }

    // QUIC 是字节流：必须做消息边界封装，否则对端会看到“28+5=33”这种粘包/拆包，
    // 也就无法按 DataChannel 的 message 语义交付给上层。
    //
    // Wire framing:
    //   [1 byte kind][4 bytes big-endian length][payload bytes]
    // kind: 0=Control, 1=String, 2=Binary, 3=Reset
    auto frameMessage = [](const message_ptr &msg) -> message_ptr {
        if (!msg)
            return nullptr;
        uint8_t kind = 2;
        switch (msg->type) {
        case Message::Control: kind = 0; break;
        case Message::String:  kind = 1; break;
        case Message::Binary:  kind = 2; break;
        case Message::Reset:   kind = 3; break;
        default: kind = 2; break;
        }
        const uint32_t len = static_cast<uint32_t>(msg->size());
        binary out;
        out.resize(5u + len);
        out[0] = std::byte{kind};
        out[1] = std::byte{static_cast<uint8_t>((len >> 24) & 0xFF)};
        out[2] = std::byte{static_cast<uint8_t>((len >> 16) & 0xFF)};
        out[3] = std::byte{static_cast<uint8_t>((len >> 8) & 0xFF)};
        out[4] = std::byte{static_cast<uint8_t>(len & 0xFF)};
        if (len > 0) {
            std::copy(msg->begin(), msg->end(), out.begin() + 5);
        }
        auto framed = make_message(std::move(out), Message::Binary, msg->stream, msg->reliability, msg->frameInfo);
        framed->dscp = msg->dscp;
        return framed;
    };

    // 将消息封装为 wire 格式后入队
    mSendQueue.push(frameMessage(message));  // 将消息添加到发送队列
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
    // QUIC连接通过lsquic引擎建立
    // 在WebRTC中，QUIC运行在DTLS之上，通过回调实现包收发，不依赖真实UDP socket
    
    if (mIsClient) {
        // offerer：使用CLIENT模式，调用lsquic_engine_connect()生成Client Initial包
        
        // 从下层传输层（DTLS）获取ICE候选地址
        // 注意：在WebRTC中，地址信息主要用于lsquic内部标识，实际数据包通过DTLS传输层发送
        struct sockaddr_in local_addr, peer_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        memset(&peer_addr, 0, sizeof(peer_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1
        local_addr.sin_port = htons(0);  // 端口不重要，因为通过DTLS传输
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1
        peer_addr.sin_port = htons(0);  // 端口不重要
        
        // 调用lsquic_engine_connect()创建连接
        // 这会触发lsquic生成Client Initial包，并通过send_packets_out回调发送
        lsquic_conn_t *conn = lsquic_engine_connect(
            mEngine,
            N_LSQVER,  // 让引擎选择版本
            (struct sockaddr *)&local_addr,
            (struct sockaddr *)&peer_addr,
            this,      // peer_ctx
            nullptr,   // conn_ctx
            nullptr,   // hostname (SNI，可选)
            mEngineSettings.es_base_plpmtu,  // base_plpmtu：固定为 1200
            nullptr, 0, // sess_resume (无会话恢复)
            nullptr, 0  // token (无token)
        );
        
        if (conn) {
            // 连接对象会在on_new_conn回调中设置到mConnCtx
            // 但我们需要立即设置，以便能够发送数据
            if (!mConnCtx) {
                mConnCtx = std::make_unique<QuicConnCtx>();
                mConnCtx->transport = this;
            }
            mConnCtx->conn = conn;
            
            // 处理连接，触发初始包的生成和发送（防重入）
            processEngineOnce("connect_initial");
        } else {
            // 失败处理
        }
    } else {
        // answerer：使用SERVER模式，等待接收offerer发送的Client Initial包来创建连接
    }
    
    // 主动处理连接，触发QUIC握手
    if (mEngine) {
        processEngineOnce("connect_start");
    }
    
    // 开始定期处理，确保及时发送和接收
    startPeriodicProcessing();
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
    // lsquic 不是线程安全的：DTLS 收包线程可能与 periodic/flush 并发，必须串行化
    std::lock_guard<std::recursive_mutex> lsqLock(mLsquicMutex);
    if (quic_trace_enabled()) {
        std::cout << "🔍 [QUIC] QuicTransport::incoming() 被调用" << std::endl;
        std::cout << "   📦 消息大小: " << (message ? message->size() : 0) << " 字节" << std::endl;
        std::cout << "   🔍 引擎指针: " << mEngine << std::endl;
        std::cout << "   📋 当前状态: " << (state() == State::Connecting ? "Connecting" :
                                             state() == State::Connected ? "Connected" : "其他") << std::endl;
    }
    
    if (!message) {
        std::cout << "⚠️  [QUIC] 收到空消息，可能表示连接关闭" << std::endl;
        return;
    }
    
    if (!mEngine) {
        std::cerr << "❌ [QUIC] 错误: lsquic引擎不存在！" << std::endl;
        return;
    }
    
    if (message->size() == 0) {
        if (quic_trace_enabled()) {
            std::cout << "⚠️  [QUIC] 收到空数据包，跳过处理" << std::endl;
        }
        return;
    }

    // 非 trace 模式下，只在连接生命周期内打印一次“首个收到的 QUIC 包”，便于定位跨公网丢包/握手卡住
    if (!quic_trace_enabled() && !mLoggedFirstPacketIn.exchange(true)) {
        std::cout << "📥 [QUIC] 收到首个QUIC数据包，大小: " << message->size() << " 字节" << std::endl;
    }
    
    if (quic_trace_enabled()) {
        std::cout << "📥 [QUIC] 收到QUIC数据包，大小: " << message->size() << " 字节" << std::endl;
        std::cout << "   🔍 数据包前16字节（十六进制）: ";
        for (size_t i = 0; i < std::min(message->size(), size_t(16)); ++i) {
            printf("%02x ", static_cast<unsigned char>(message->data()[i]));
        }
        std::cout << std::endl;
    }
    
    // 构造sockaddr用于lsquic_engine_packet_in()
    // 在WebRTC中，QUIC运行在DTLS之上，地址信息主要用于lsquic内部标识
    // 实际数据包通过DTLS传输层发送，地址信息不重要
    struct sockaddr_in local_addr, peer_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    memset(&peer_addr, 0, sizeof(peer_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1
    local_addr.sin_port = htons(0);
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1
    peer_addr.sin_port = htons(0);

    // 将接收到的数据传递给lsquic引擎处理
    // 在WebRTC中，QUIC运行在DTLS之上，通过回调实现包收发
    // - SERVER模式：接收到Client Initial包会触发on_new_conn回调
    // - CLIENT模式：接收到Server的响应包会继续握手
    if (quic_trace_enabled()) {
        std::cout << "   🔄 调用lsquic_engine_packet_in()处理QUIC包..." << std::endl;
        std::cout << "   📋 WebRTC角色: " << (mIsClient ? "offerer" : "answerer") << std::endl;
        std::cout << "   🔧 LSQUIC引擎模式: " << (mIsClient ? "CLIENT" : "SERVER") << std::endl;
        std::cout << "   📍 本地地址: 127.0.0.1:0 (虚拟地址，实际通过DTLS传输)" << std::endl;
        std::cout << "   📍 远程地址: 127.0.0.1:0 (虚拟地址，实际通过DTLS传输)" << std::endl;
    }
    int ret = lsquic_engine_packet_in(mEngine, reinterpret_cast<const unsigned char*>(message->data()), message->size(),
                               (struct sockaddr *)&local_addr,
                               (struct sockaddr *)&peer_addr, this, 0);  // 使用this作为peer_ctx
    
    if (quic_trace_enabled()) {
        std::cout << "   📊 lsquic_engine_packet_in()返回值: " << ret << std::endl;
        if (ret == 0) {
            // 包被处理，可能需要处理连接
            std::cout << "   ✅ QUIC包已被连接处理" << std::endl;
            std::cout << "   🔍 检查是否触发on_new_conn回调..." << std::endl;
        } else if (ret == 1) {
            // 包被处理，但不是由连接处理的（可能是版本协商等）
            std::cout << "   ⚠️  QUIC包被处理，但不是由连接处理（可能是版本协商）" << std::endl;
        } else {
            // 处理错误
            std::cerr << "   ❌ QUIC包处理失败，返回值: " << ret << std::endl;
        }
    } else if (ret < 0) {
        // 非 trace 模式下只保留错误
        std::cerr << "   ❌ QUIC包处理失败，返回值: " << ret << std::endl;
    }
    
    // 处理连接，触发输出（避免重入）
    processEngineOnce("incoming");
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
    
    // 定期处理连接，确保QUIC握手能够进行（避免重入）
    if (mEngine && state() == State::Connecting) {
        processEngineOnce("doRecv");
    }
}

// 处理刷新操作
void QuicTransport::doFlush() {
    trySendQueue();  // 尝试写入数据到 stream（可能只写入到 lsquic 的内部发送队列）
    // 关键：这里不要频繁 tick conns（process_conns），只把已生成的 UDP payload 立刻发出去即可。
    // tick conns 由 packet_in/connecting/periodic 驱动。
    sendUnsentPacketsOnce("flush");
}

// 将接收操作加入队列
void QuicTransport::enqueueRecv() {
    // 只在从 0 -> 1 时调度一次任务，避免风暴式 enqueue
    if (mPendingRecvCount.fetch_add(1) == 0) {
        mProcessor.enqueue([this]() {
            for (;;) {
                doRecv();
                // 消费一个“接收请求”
                const int prev = mPendingRecvCount.fetch_sub(1);
                if (prev <= 1)
                    break;
                // doRecv 基本不做重活（主要是 processEngineOnce），继续合并处理即可
            }
        });
    }
}

// 将刷新操作加入队列
void QuicTransport::enqueueFlush() {
    // 只在从 0 -> 1 时调度一次任务；后续请求合并到计数里
    if (mPendingFlushCount.fetch_add(1) == 0) {
        mProcessor.enqueue([this]() {
            for (;;) {
                const size_t before = mSendQueue.size();
                doFlush();
                const size_t after = mSendQueue.size();

                const int prev = mPendingFlushCount.fetch_sub(1);
                if (prev <= 1)
                    break;

                // 如果本轮 flush 没有任何进展（通常是流控/阻塞），继续空转毫无意义。
                // 折叠剩余请求，等待 on_stream_write / 后续 send 再触发 flush。
                if (after == before) {
                    mPendingFlushCount.exchange(0);
                    break;
                }
            }
        });
    }
}

// 尝试发送队列中的数据
bool QuicTransport::trySendQueue() {
    // 同时保护：1) 发送队列 2) lsquic 内部状态（stream_write/flush 不是线程安全的）
    std::scoped_lock lock(mLsquicMutex, mSendMutex);

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
    // 未握手完成（或无连接）时，不创建流，保持队列等待 on_hsk_done 触发
    if (!mConnCtx || !mConnCtx->conn || state() != State::Connected) {
        if (mIsClient && mEngine) {
            // 客户端模式仍触发一次引擎处理，生成 Client Initial
            processEngineOnce("trySend_no_conn");
        }
        return false;
    }
    
    // 通过QUIC流发送消息
    if (mConnCtx && mConnCtx->conn) {  // 如果连接上下文和连接都存在
        // 0) 检查是否应走 Datagram (不可靠/无序传输)
        bool useDatagram = false;
        if (message->reliability) {
            const auto &r = *message->reliability;
            // 只要设置了部分可靠性参数或 unordered，就使用 Datagram
            if (r.unordered || r.maxRetransmits.has_value() || r.maxPacketLifeTime.has_value()) {
                useDatagram = true;
            }
        }

        if (useDatagram) {
            uint16_t sid = static_cast<uint16_t>(message->stream);
            // Payload format: [StreamID (4 bytes)][Data]
            // Note: Data 已经被 send() 加上了 [Kind][Len] 帧头，这里再加一层 Datagram 路由头
            size_t totalLen = 4 + message->size();
            
            // lsquic 可能限制 Datagram 大小（通常 ~1200 字节）。若超限则无法发送。
            // 这里暂不做分片，超大包直接丢弃或由 lsquic 拒绝。
            std::vector<unsigned char> buf(totalLen);
            
            // Write StreamID (Big Endian)
            buf[0] = static_cast<unsigned char>((sid >> 24) & 0xFF);
            buf[1] = static_cast<unsigned char>((sid >> 16) & 0xFF);
            buf[2] = static_cast<unsigned char>((sid >> 8) & 0xFF);
            buf[3] = static_cast<unsigned char>(sid & 0xFF);
            
            // Copy message data
            if (message->size() > 0) {
                std::memcpy(buf.data() + 4, message->data(), message->size());
            }

            ssize_t sent = lsquic_conn_write_datagram(mConnCtx->conn, buf.data(), buf.size());
            if (sent > 0) {
                mBytesSent += static_cast<size_t>(sent);
                // 触发一次引擎处理，尽快把 Datagram 刷到底层
                processEngineOnce("datagram_sent");
                return true;
            } else {
                // 发送失败（如缓冲区满、包过大）。对于不可靠通道，直接丢弃。
                if (quic_trace_enabled()) {
                    std::cerr << "   ⚠️ [QUIC] Datagram 发送丢弃/失败, stream=" << sid 
                              << ", len=" << totalLen << ", ret=" << sent << ", errno=" << errno << std::endl;
                }
                // 返回 true 表示“已处理（丢弃）”，将从队列中移除
                return true;
            }
        }

        // 1) 若该 stream 已存在，复用同一个 QUIC stream（DataChannel 语义：一个通道固定一个 stream）
        {
            std::lock_guard<std::mutex> lock(mConnCtx->streamsMutex);
            auto it = mConnCtx->streamIds.find(static_cast<uint16_t>(message->stream));
            if (it != mConnCtx->streamIds.end() && it->second) {
                lsquic_stream_t *stream = it->second;
                // QUIC stream 是字节流：一次 write 可能写入 0/部分字节。
                // 用 mSendOffsets 跟踪已发送偏移，避免对 message 做 erase（大包会退化到 O(n^2)）。
                const rtc::Message* key = message.get();
                size_t &off = mSendOffsets[key];
                if (off > message->size()) off = 0;

                const auto *base = reinterpret_cast<const char *>(message->data());
                while (off < message->size()) {
                    const size_t remaining = message->size() - off;
                    const ssize_t written = lsquic_stream_write(stream, base + off, remaining);
                    if (written > 0) {
                        off += static_cast<size_t>(written);
                        mBytesSent += static_cast<size_t>(written);
                        continue; // 尽量一次 flush 写满
                    }
                    if (written == 0 || errno == EWOULDBLOCK || errno == EAGAIN) {
                        lsquic_stream_flush(stream);
                        lsquic_stream_wantwrite(stream, 1);
                        // 不要在写路径 tick conns，避免触发 lsquic tick 断言；只发未发送包即可
                        sendUnsentPacketsOnce("stream_write_blocked");
                        return false;
                    }
                    std::cerr << "   ❌ [QUIC] stream_write失败，stream=" << message->stream
                              << " errno=" << errno << std::endl;
                    mSendOffsets.erase(key);
                    lsquic_stream_close(stream);
                    return false;
                }

                // message 已完全写入
                mSendOffsets.erase(key);
                lsquic_stream_flush(stream);
                lsquic_stream_wantwrite(stream, 0);
                // 同上：只发送未发送包，不 tick conns
                sendUnsentPacketsOnce("stream_write_reuse_done");
                return true;
            }
        }

        // 2) stream 还不存在：只允许 OPEN 控制报文触发新建流
        // 由于我们对 QUIC 发送做了 framing，OPEN 位于 payload 的首字节（offset=5）
        const auto *wire = reinterpret_cast<const uint8_t *>(message->data());
        const bool isOpen =
            message->size() >= 6 &&
            wire[0] == 0 /*Control*/ &&
            wire[5] == 0x03 /*OPEN*/;
        if (!isOpen) {
            // 不是 OPEN，则等待 OPEN 建流后再发
            return false;
        }

        // 检查可用的可双向流配额，避免触发 lsquic 内部断言
        if (lsquic_conn_n_avail_streams(mConnCtx->conn) == 0) {
            std::cerr << "   ⚠️ [QUIC] 无可用流配额，发送延后" << std::endl;
            return false;
        }
        // 为这个消息创建一个新流（流对象会在 on_new_stream 回调中交付）
        {
            std::lock_guard<std::mutex> lk(mPendingStreamMutex);
            mPendingStreamMessages.push(std::move(message));
        }
        // 某些 lsquic 版本返回 void，这里只调用，不检查返回值
        lsquic_conn_make_stream(mConnCtx->conn);
        // 触发引擎处理，促使 on_new_stream 尽快到来
        processEngineOnce("stream_create");
        return true;                    // 返回成功（等待回调写入）
    }
    return false;  // 返回失败
}

void QuicTransport::sendUnsentPacketsOnce(const char *reason) {
    if (!mEngine)
        return;
    // 串行化 lsquic_engine_has_unsent_packets/send_unsent_packets，避免与 process_conns 并发
    std::lock_guard<std::recursive_mutex> lsqLock(mLsquicMutex);

    // 复用同一个“正在处理引擎”的闸门，避免和 processEngineOnce 交叠
    if (mEngineProcessing.test_and_set()) {
        // 引擎正在处理（或即将处理），无需在这里重复发
        return;
    }
    struct FlagGuard {
        std::atomic_flag &flag;
        ~FlagGuard() { flag.clear(); }
    } guard{mEngineProcessing};

    if (quic_trace_enabled() && reason) {
        std::cout << "   📤 发送lsquic未发送包 [" << reason << "]" << std::endl;
    }
    if (lsquic_engine_has_unsent_packets(mEngine)) {
        lsquic_engine_send_unsent_packets(mEngine);
    }
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

void QuicTransport::onBufferedAmount(amount_callback callback) {
    mBufferedAmountCallback = std::move(callback);
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
    // 根据内容/流类型推断消息类型，确保数据通道控制报文能被正确识别
    // 参照 DataChannel 定义：0x03 为 OPEN，0x02 为 ACK
    static constexpr uint8_t kMsgOpen = 0x03;
    static constexpr uint8_t kMsgAck  = 0x02;

    Message::Type msgType = Message::Type::Binary;
    if (!data.empty()) {
        const auto first = static_cast<uint8_t>(data.front());
        if (type == STREAM_CONTROL || first == kMsgOpen || first == kMsgAck) {
            msgType = Message::Type::Control;
        } else if (type == STREAM_STRING || type == STREAM_STRING_PARTIAL || type == STREAM_STRING_EMPTY) {
            msgType = Message::Type::String;
        }
    }

    size_t dataSize = data.size();  // 在移动之前保存大小
    auto message = make_message(std::move(data), msgType, streamId);  // 保留流ID
    recv(std::move(message));  // 向上层传递消息
    mBytesReceived += dataSize;  // 增加接收字节数统计
}

// 静态回调函数实现

// 新连接回调
lsquic_conn_ctx_t *QuicTransport::on_new_conn(void *stream_if_ctx, lsquic_conn_t *conn) {
    if (quic_trace_enabled()) {
        std::cout << "🔍 [QUIC] on_new_conn() 回调被调用" << std::endl;
    }
    
    auto *transport = static_cast<QuicTransport *>(stream_if_ctx);  // 转换为传输层指针
    if (!transport || !transport->mConnCtx) {
        return nullptr;
    }
    
    transport->mConnCtx->conn = conn;  // 设置连接指针
    
    // 注意：这里不要提前标记 Connected。
    // QUIC 的流配额（MAX_STREAMS）要等握手/transport parameters 交换完成后才可靠，
    // 提前切到 Connected 会导致发送线程过早查询配额，从而一直看到 0 并延后发送。
    return reinterpret_cast<lsquic_conn_ctx_t *>(transport->mConnCtx.get());  // 返回连接上下文
}

// 连接关闭回调
void QuicTransport::on_conn_closed(lsquic_conn_t *conn) {
    auto *connCtx = reinterpret_cast<QuicConnCtx *>(lsquic_conn_get_ctx(conn));  // 获取连接上下文
    if (connCtx && connCtx->transport) {  // 如果连接上下文和传输层都存在
        connCtx->transport->stopPeriodicProcessing();  // 停止定期处理
        connCtx->transport->changeState(State::Disconnected);  // 改变状态为断开连接
    }
    // 清理连接上下文，避免销毁时 lsquic 断言 cn_conn_ctx 非空
    lsquic_conn_set_ctx(conn, nullptr);
}

// 新流回调
lsquic_stream_ctx_t *QuicTransport::on_new_stream(void *stream_if_ctx, lsquic_stream_t *stream) {
    auto *transport = static_cast<QuicTransport *>(stream_if_ctx);  // 转换为传输层指针
    auto *streamCtx = new QuicStreamCtx();  // 创建流上下文
    streamCtx->connCtx = transport->mConnCtx.get();  // 设置连接上下文指针
    // 使用 lsquic 提供的真实流 ID
    streamCtx->streamId = static_cast<uint16_t>(lsquic_stream_id(stream));

    {
        std::lock_guard<std::mutex> lock(transport->mConnCtx->streamsMutex);  // 获取流映射表锁
        transport->mConnCtx->streams[stream] = streamCtx->streamId;  // 添加流到流ID的映射
        transport->mConnCtx->streamIds[streamCtx->streamId] = stream;  // 添加流ID到流的映射
    }
    streamCtx->isOpen = true;

    // 无论是本端新开流还是对端开流，都需要声明想读数据
    lsquic_stream_wantread(stream, 1);

    // SERVER 侧如果已经收到了可读的应用流，说明已经具备 1-RTT 解密能力，
    // 可以认为连接已“可用”以发送 DataChannel ACK（避免一直卡在 Connecting）
    if (!transport->mIsClient && transport->state() == State::Connecting) {
        transport->changeState(State::Connected);
        if (quic_trace_enabled()) {
            std::cout << "   ✅ [QUIC SERVER] 收到新流，提升状态为Connected" << std::endl;
        }
    }

    // 如果有待发送消息，把第一条写入该新流
    message_ptr pending;
    {
        std::lock_guard<std::mutex> lk(transport->mPendingStreamMutex);
        if (!transport->mPendingStreamMessages.empty()) {
            pending = std::move(transport->mPendingStreamMessages.front());
            transport->mPendingStreamMessages.pop();
        }
    }
    if (pending && stream) {
        const auto *data_ptr = reinterpret_cast<const char *>(pending->data());
        const size_t data_len = pending->size();
        ssize_t written = lsquic_stream_write(stream, data_ptr, data_len);
        if (written < 0) {
            lsquic_stream_close(stream);
        } else {
            lsquic_stream_flush(stream);
            transport->mBytesSent += static_cast<size_t>(written);
            // 确保数据尽快发出
            transport->processEngineOnce("stream_write_cb");
            // 关键：OPEN 建流后，立即继续 flush 发送队列里后续的应用数据（1KB/10KB/...）
            // 否则队列可能会因为“stream 尚未存在”而停在第一条非 OPEN 消息上，导致对端只收到很少数据。
            transport->enqueueFlush();
        }
    }
    // 如果没有待发送消息，保持流开启，等待对端发送数据

    return reinterpret_cast<lsquic_stream_ctx_t *>(streamCtx);  // 返回流上下文
}

// 流读取回调
void QuicTransport::on_stream_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    auto *streamCtx = reinterpret_cast<QuicStreamCtx *>(h);  // 转换为流上下文
    unsigned char buf[4096];  // 读取缓冲区
    // QUIC stream 是字节流：一次回调里需要尽可能把可读数据 drain 掉，
    // 否则某些实现/时序下可能不会频繁再触发回调，导致“对端在发但本端不读”。
    auto *transport = (streamCtx && streamCtx->connCtx) ? streamCtx->connCtx->transport : nullptr;
    if (!transport)
        return;

    // Frame decode: [kind][len(4)][payload]
    auto decodeOne = [&](Message::Type &outType, binary &outPayload) -> bool {
        auto &b = streamCtx->buffer;
        size_t &off = streamCtx->bufferOffset;
        if (b.size() < off + 5)
            return false;
        const uint8_t kind = b[off];
        const uint32_t len =
            (uint32_t(b[off + 1]) << 24) |
            (uint32_t(b[off + 2]) << 16) |
            (uint32_t(b[off + 3]) << 8)  |
            (uint32_t(b[off + 4]));
        // 基本防御：长度异常直接关闭流，避免内存膨胀
        if (len > transport->mMaxMessageSize) {
            std::cerr << "   ❌ [QUIC] 收到过大消息，len=" << len
                      << " (max=" << transport->mMaxMessageSize << "), 关闭流" << std::endl;
            lsquic_stream_close(stream);
            streamCtx->isOpen = false;
            return false;
        }
        if (b.size() < off + 5u + len)
            return false; // 等待更多数据

        switch (kind) {
        case 0: outType = Message::Control; break;
        case 1: outType = Message::String;  break;
        case 2: outType = Message::Binary;  break;
        case 3: outType = Message::Reset;   break;
        default: outType = Message::Binary; break;
        }
        outPayload = make_binary_from_chars(reinterpret_cast<const char *>(b.data() + off + 5), len);
        off += 5u + len;

        // 适时压缩 buffer，避免无限增长
        if (off == b.size()) {
            b.clear();
            off = 0;
        } else if (off > 64 * 1024) {
            b.erase(b.begin(), b.begin() + static_cast<long>(off));
            off = 0;
        }
        return true;
    };

    for (;;) {
        const ssize_t nread = lsquic_stream_read(stream, buf, sizeof(buf));  // 从流中读取数据
        if (nread > 0) {
            // 追加到流缓冲区
            streamCtx->buffer.insert(streamCtx->buffer.end(), buf, buf + nread);

            // 尽可能解析完整 frame 并向上层交付
            for (;;) {
                Message::Type type;
                binary payload;
                if (!decodeOne(type, payload))
                    break;
                auto msg = make_message(std::move(payload), type, streamCtx->streamId);
                const size_t payloadSize = msg->size();
                transport->recv(std::move(msg));
                transport->mBytesReceived += payloadSize;
            }
            continue;
        }

        if (nread == 0) {
            // 对端发送FIN，停止继续读取，避免回调空转
            lsquic_stream_wantread(stream, 0);
            lsquic_stream_shutdown(stream, 0);  // 关闭读方向
            streamCtx->isOpen = false;
            break;
        }

        // nread < 0
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            // 当前无更多数据，保持 wantread=1，等待下一次回调
            break;
        }

        std::cerr << "   ❌ [QUIC] on_stream_read 读取失败，errno=" << errno << std::endl;
        lsquic_stream_close(stream);
        streamCtx->isOpen = false;
        break;
    }
}

// 流写入回调
void QuicTransport::on_stream_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    auto *streamCtx = reinterpret_cast<QuicStreamCtx *>(h);
    if (!streamCtx || !streamCtx->connCtx || !streamCtx->connCtx->transport)
        return;

    auto *transport = streamCtx->connCtx->transport;

    // 注意：lsquic 期望用户在 on_stream_write 里“实际写入”，否则会判定用户代码没有进展并刷 warn。
    // 我们这里在回调线程里直接尝试推进发送队列（已用 mLsquicMutex 串行化 lsquic 调用，安全）。
    transport->trySendQueue();
    transport->sendUnsentPacketsOnce("on_stream_write");

    // 只在“队列头就是这个 stream 的消息”时保持 wantwrite=1，否则关闭，避免对一个无关 stream 回调风暴。
    {
        std::scoped_lock lk(transport->mLsquicMutex, transport->mSendMutex);
        bool want = false;
        if (auto peek = transport->mSendQueue.peek()) {
            if (*peek) {
                const uint16_t headStream = static_cast<uint16_t>((*peek)->stream);
                want = (headStream == streamCtx->streamId);
            }
        }
        lsquic_stream_wantwrite(stream, want ? 1 : 0);
    }
}

// 流关闭回调
void QuicTransport::on_stream_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    (void)stream; // 标记参数为已使用，避免警告
    
    auto *streamCtx = reinterpret_cast<QuicStreamCtx *>(h);  // 转换为流上下文
    if (streamCtx) {  // 如果流上下文存在
        streamCtx->isClosed = true;  // 设置关闭标志
        delete streamCtx;            // 删除流上下文
    }
}

// 数据报接收回调 (RFC 9221)
void QuicTransport::on_datagram(lsquic_conn_t *c, const void *buf, size_t sz) {
    auto *connCtx = reinterpret_cast<QuicConnCtx *>(lsquic_conn_get_ctx(c));
    if (!connCtx || !connCtx->transport) return;
    
    // Framing check: 至少包含 4 字节 StreamID + 1 字节 Kind + 4 字节 Len = 9 字节
    if (sz < 9) {
        if (quic_trace_enabled()) {
            std::cerr << "   ⚠️ [QUIC] 收到过短 Datagram (" << sz << " bytes), 丢弃" << std::endl;
        }
        return;
    }

    const uint8_t *p = static_cast<const uint8_t *>(buf);
    // 1. 解析 StreamID (Big Endian, 4 bytes)
    uint32_t id32 = (uint32_t(p[0]) << 24) | 
                    (uint32_t(p[1]) << 16) | 
                    (uint32_t(p[2]) << 8)  | 
                    (uint32_t(p[3]));
    uint16_t streamId = static_cast<uint16_t>(id32);

    // 2. 解析 Kind (1 byte)
    uint8_t kind = p[4];

    // 3. 解析 Length (Big Endian, 4 bytes)
    uint32_t len = (uint32_t(p[5]) << 24) | 
                   (uint32_t(p[6]) << 16) | 
                   (uint32_t(p[7]) << 8)  | 
                   (uint32_t(p[8]));

    // 校验完整性
    if (sz < 9 + len) {
        if (quic_trace_enabled()) {
            std::cerr << "   ⚠️ [QUIC] Datagram 长度校验失败: declared=" << len << ", actual=" << (sz - 9) << std::endl;
        }
        return;
    }

    if (quic_trace_enabled()) {
        std::cout << "   📥 [QUIC] 收到 Datagram: stream=" << streamId << ", kind=" << int(kind) << ", len=" << len << std::endl;
    }

    // 映射消息类型
    Message::Type type = Message::Type::Binary;
    switch (kind) {
    case 0: type = Message::Control; break;
    case 1: type = Message::String;  break;
    case 2: type = Message::Binary;  break;
    case 3: type = Message::Reset;   break;
    default: type = Message::Binary; break;
    }

    // 提取负载并交付
    binary payload = make_binary_from_chars(p + 9, len);
    auto msg = make_message(std::move(payload), type, streamId);
    
    connCtx->transport->recv(std::move(msg));
    connCtx->transport->mBytesReceived += len;
}

// 数据报可写回调
void QuicTransport::on_dg_write(lsquic_conn_t *c) {
    // 当 lsquic 内部 Datagram 缓冲区有空间时被调用。
    // 目前采用 fire-and-forget 策略（发送满则丢弃），暂不需要在此回调中补发。
    (void)c;
}

bool QuicTransport::processEngineOnce(const char *reason) {
    if (!mEngine)
        return false;
    // process_conns 期间会回调到 on_stream_read/write 等用户代码；必须在同一把 lsquic 锁下执行，禁止并发
    std::lock_guard<std::recursive_mutex> lsqLock(mLsquicMutex);
    if (mEngineProcessing.test_and_set()) {
        // 已有一次处理在进行，标记需要再处理一次，避免丢包/握手卡住
        mEngineProcessPending.store(true);
        return false;
    }
    struct FlagGuard {
        std::atomic_flag &flag;
        ~FlagGuard() { flag.clear(); }
    } guard{mEngineProcessing};

    auto process_once = [this](const char *why) {
        // 避免周期性日志刷屏，仅在非周期调用时打印
        bool verbose = !why || std::string(why) != "periodic";
        if (verbose && quic_trace_enabled()) {
            std::cout << "   🔄 调用lsquic_engine_process_conns() [" << (why ? why : "unknown") << "]" << std::endl;
        }
        lsquic_engine_process_conns(mEngine);
        if (lsquic_engine_has_unsent_packets(mEngine)) {
            lsquic_engine_send_unsent_packets(mEngine);
        }
        if (verbose && quic_trace_enabled()) {
            std::cout << "   ✅ lsquic_engine_process_conns()完成 [" << (why ? why : "unknown") << "]" << std::endl;
        }
    };

    // 先处理当前请求
    process_once(reason);

    // 重要：不要在同一个调用栈里紧接着再调用一次 lsquic_engine_process_conns()。
    // 某些 lsquic 版本会在这种“连续 tick”时触发 LSCONN_TICKED 相关断言。
    // 改为异步补跑一次，既能推进握手/队列，也避免断言。
    if (mEngineProcessPending.exchange(false)) {
        auto weak = weak_from_this();
        mProcessor.enqueue([weak]() {
            if (auto self = weak.lock()) {
                self->processEngineOnce("pending");
            }
        });
    }
    return true;
}

// 包发送回调
// 关键：在WebRTC中，QUIC运行在DTLS之上，需要通过DTLS传输层发送数据，而不是直接通过UDP
int QuicTransport::send_packets_out(void *ctx, const struct lsquic_out_spec *specs, unsigned n_specs) {
    if (quic_trace_enabled()) {
        std::cout << "📤 [QUIC] send_packets_out() 回调被调用，包数量: " << n_specs << std::endl;
    }
    
    auto *transport = static_cast<QuicTransport *>(ctx);  // 转换为传输层指针
    if (!transport) {
        std::cerr << "   ❌ 错误: transport指针为空！" << std::endl;
        return -1;
    }

    // 获取下层传输层（DTLS）
    auto lower = transport->getLower();
    if (!lower) {
        std::cerr << "   ❌ 错误: 下层传输层（DTLS）不存在！" << std::endl;
        return -1;
    }

    int sent_count = 0;  // 成功发送的包数量

    // 非 trace 模式下，仅打印一次“首次发包”的摘要（n_specs + 最大包长），便于判断是否真的发出 Client Initial
    if (!quic_trace_enabled() && transport && !transport->mLoggedFirstPacketsOut.exchange(true)) {
        size_t max_len = 0;
        for (unsigned i = 0; i < n_specs; ++i) {
            size_t total_len = 0;
            for (unsigned j = 0; j < specs[i].iovlen; j++) {
                total_len += specs[i].iov[j].iov_len;
            }
            max_len = std::max(max_len, total_len);
        }
        std::cout << "📤 [QUIC] 首次发包: n=" << n_specs << ", max_len=" << max_len << " 字节（trace=off）" << std::endl;
    }

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

        // 使用辅助函数创建binary数据
        auto byteData = make_binary_from_chars(packet_data.data(), packet_data.size());
        auto message = make_message(byteData.begin(), byteData.end(), Message::Type::Binary);  // 创建消息
        
        if (quic_trace_enabled()) {
            std::cout << "   📦 准备发送QUIC包 #" << (i+1) << "，大小: " << total_len << " 字节" << std::endl;
            std::cout << "   🔍 数据包前16字节（十六进制）: ";
            for (size_t j = 0; j < std::min(total_len, size_t(16)); ++j) {
                printf("%02x ", static_cast<unsigned char>(packet_data[j]));
            }
            std::cout << std::endl;
        }
        
        // 关键：通过DTLS传输层发送数据，而不是直接通过UDP socket
        // 在WebRTC中，QUIC运行在DTLS之上，所有数据包都需要通过DTLS传输层发送
        bool sent = lower->send(std::move(message));
        if (sent) {
            sent_count++;
            if (quic_trace_enabled()) {
                std::cout << "   ✅ [QUIC] 包 #" << (i+1) << " 已通过DTLS发送（" << total_len << " 字节）" << std::endl;
            }
        } else {
            std::cerr << "   ❌ [QUIC] 包 #" << (i+1) << " 发送失败（" << total_len << " 字节）" << std::endl;
            // 如果发送失败，返回错误
            return -1;
        }
    }
    
    // 返回实际发送的包数量（LSQUIC 期望返回 sent_count）
    if (quic_trace_enabled()) {
        std::cout << "   ✅ [QUIC] send_packets_out() 完成，成功发送 " << sent_count << "/" << n_specs << " 个包" << std::endl;
    }
    return sent_count;
}

// 握手完成回调
void QuicTransport::on_hsk_done(lsquic_conn_t *c, enum lsquic_hsk_status s) {
    if (quic_trace_enabled()) {
        std::cout << "🔍 [QUIC] on_hsk_done() 回调被调用, status=" << s << std::endl;
    }
    
    auto *connCtx = reinterpret_cast<QuicConnCtx *>(lsquic_conn_get_ctx(c));  // 获取连接上下文
    if (!connCtx || !connCtx->transport) {
        return;
    }
    
    if (s == LSQ_HSK_OK || s == LSQ_HSK_RESUMED_OK) {  // 如果握手成功
        if (quic_trace_enabled()) {
            std::cout << "✅ [QUIC] 握手成功！" << std::endl;
        }
        connCtx->transport->changeState(State::Connected);  // 改变状态为已连接
        
        // 开始定期处理连接，确保发送keepalive
        connCtx->transport->startPeriodicProcessing();

        // 握手成功后可能之前因为“无可用流配额”而延后发送，这里立刻重试发送队列
        connCtx->transport->trySendQueue();
        
        // 在SERVER模式下，主动发送一个PING帧来触发CLIENT端的连接建立
        if (!connCtx->transport->mIsClient && connCtx->conn) {
            connCtx->transport->processEngineOnce("on_hsk_done_ping");
        }
    } else {
        if (quic_trace_enabled()) {
            std::cerr << "❌ [QUIC] 握手失败！状态码: " << s << std::endl;
        }
        connCtx->transport->changeState(State::Failed);  // 改变状态为失败
    }
}

// 开始定期处理连接
void QuicTransport::startPeriodicProcessing() {
    if (mPeriodicProcessingActive.exchange(true)) {
        // 已经在运行，不需要重复启动
        return;
    }
    
    // 立即处理一次
    periodicProcessConnections();
}

// 停止定期处理连接
void QuicTransport::stopPeriodicProcessing() {
    if (!mPeriodicProcessingActive.exchange(false)) {
        // 已经停止，不需要重复停止
        return;
    }
}

// 定期处理连接的函数
void QuicTransport::periodicProcessConnections() {
    // 检查是否应该继续处理
    if (!mPeriodicProcessingActive.load() || !mEngine) {
        return;
    }
    
    // 处理连接，这会触发keepalive（PING帧）的发送
    processEngineOnce("periodic");
    
    // 动态调整定时器间隔 (Pacing 核心逻辑)
    // lsquic_engine_earliest_adv_tick() 返回微秒级绝对时间戳，指示引擎希望下次被处理的时间
    int diff;
    int timeout;
    if (lsquic_engine_earliest_adv_tick(mEngine, &diff)) {
        // diff 是距离现在的微秒数
        // 向上取整到毫秒，且至少等待 1ms 以避免空转
        timeout = (diff + 999) / 1000;
        if (timeout < 1) timeout = 1;
        // 设置上限 200ms，避免过久不处理
        if (timeout > 200) timeout = 200;
    } else {
        // 引擎没有明确建议，使用默认心跳间隔 (100ms)
        timeout = 100;
    }

    // 如果定期处理仍然激活，且状态不是Disconnected或Failed，安排下一次处理
    if (mPeriodicProcessingActive.load()) {
        State currentState = state();
        if (currentState != State::Disconnected && currentState != State::Failed) {
            ThreadPool::Instance().schedule(
                std::chrono::milliseconds(timeout),
                [weak_this = weak_from_this()]() {
                    if (auto locked = weak_this.lock()) {
                        locked->periodicProcessConnections();
                    }
                }
            );
        }
    }
}

} // namespace rtc::impl