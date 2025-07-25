#include <iostream>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <cstdlib>
#include <event2/event.h>
#include <sys/types.h>
#include <openssl/ssl.h>
#include "lsquic.h"

struct quic_client_ctx;
struct conn_ctx {
    lsquic_conn_t *conn;
    quic_client_ctx *client_ctx;
};

struct quic_client_ctx {
    int sock_fd;
    lsquic_engine_t *engine;
    conn_ctx *conn_h;
    struct event_base *eb;
    struct event *ev_socket;
    struct event *ev_tick;
};

static void on_tick(evutil_socket_t, short, void *);
static void on_socket(evutil_socket_t, short, void *);

static lsquic_conn_ctx_t *on_new_conn(void *stream_if_ctx, lsquic_conn_t *conn) {
    auto *client_ctx = static_cast<quic_client_ctx *>(stream_if_ctx);
    conn_ctx *conn_h = (conn_ctx *)malloc(sizeof(conn_ctx));
    memset(conn_h, 0, sizeof(conn_ctx));
    conn_h->conn = conn;
    conn_h->client_ctx = client_ctx;
    client_ctx->conn_h = conn_h;
    std::cout << "New connection created!" << std::endl;
    lsquic_conn_make_stream(conn);
    return reinterpret_cast<lsquic_conn_ctx_t *>(conn_h);
}

static void on_conn_closed(lsquic_conn_t *conn) {
    conn_ctx *conn_h = reinterpret_cast<conn_ctx *>(lsquic_conn_get_ctx(conn));
    std::cout << "Connection closed." << std::endl;
    
    // 获取连接状态信息
    char errbuf[256];
    enum LSQUIC_CONN_STATUS status = lsquic_conn_status(conn, errbuf, sizeof(errbuf));
    
    // 将状态值转换为可读的字符串
    const char* status_str = "UNKNOWN";
    switch (status) {
        case LSCONN_ST_HSK_IN_PROGRESS: status_str = "HANDSHAKE_IN_PROGRESS"; break;
        case LSCONN_ST_CONNECTED: status_str = "CONNECTED"; break;
        case LSCONN_ST_HSK_FAILURE: status_str = "HANDSHAKE_FAILURE"; break;
        case LSCONN_ST_GOING_AWAY: status_str = "GOING_AWAY"; break;
        case LSCONN_ST_TIMED_OUT: status_str = "TIMED_OUT"; break;
        case LSCONN_ST_RESET: status_str = "RESET"; break;
        case LSCONN_ST_USER_ABORTED: status_str = "USER_ABORTED"; break;
        case LSCONN_ST_ERROR: status_str = "ERROR"; break;
        case LSCONN_ST_CLOSED: status_str = "CLOSED"; break;
        case LSCONN_ST_PEER_GOING_AWAY: status_str = "PEER_GOING_AWAY"; break;
        case LSCONN_ST_VERNEG_FAILURE: status_str = "VERSION_NEGOTIATION_FAILURE"; break;
    }
    
    std::cout << "Connection status: " << status_str << " (" << status << ")" << std::endl;
    if (strlen(errbuf) > 0) {
        std::cout << "Connection error: " << errbuf << std::endl;
    }
    
    // 检查是否握手成功
    if (status == LSCONN_ST_CONNECTED) {
        std::cout << "SUCCESS: Connection was established and handshake completed!" << std::endl;
    } else if (status == LSCONN_ST_HSK_IN_PROGRESS) {
        std::cout << "WARNING: Handshake was still in progress when connection closed" << std::endl;
    } else if (status == LSCONN_ST_HSK_FAILURE) {
        std::cout << "ERROR: Handshake failed" << std::endl;
    } else if (status == LSCONN_ST_TIMED_OUT) {
        std::cout << "ERROR: Connection timed out" << std::endl;
    } else if (status == LSCONN_ST_RESET) {
        std::cout << "ERROR: Connection was reset by peer" << std::endl;
    }
    
    if (conn_h) {
        conn_h->client_ctx->conn_h = nullptr;
        // 告诉 lsquic 连接上下文已经被清理
        lsquic_conn_set_ctx(conn, nullptr);
        free(conn_h);
    }
    
    // 不要立即退出，给更多时间处理后续包
    std::cout << "Connection closed, but continuing to process packets..." << std::endl;
}

static lsquic_stream_ctx_t *on_new_stream(void *stream_if_ctx, lsquic_stream_t *stream) {
    std::cout << "New stream created, sending 'hello'" << std::endl;
    lsquic_stream_wantwrite(stream, 1);
    return nullptr;
}

static void on_stream_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    unsigned char buf[1024];
    ssize_t nread = lsquic_stream_read(stream, buf, sizeof(buf));
    if (nread > 0) {
        std::cout << "Read from stream: " << std::string(reinterpret_cast<char*>(buf), nread) << std::endl;
        lsquic_stream_shutdown(stream, 0);
    } else if (nread == 0) {
        std::cout << "Stream EOF." << std::endl;
        lsquic_stream_close(stream);
    } else {
        std::cerr << "Error reading from stream." << std::endl;
    }
}

static void on_stream_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    const char *msg = "hello";
    lsquic_stream_write(stream, msg, strlen(msg));
    std::cout << "Wrote 'hello' to stream." << std::endl;
    lsquic_stream_shutdown(stream, 1);
}

static void on_stream_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    std::cout << "Stream closed." << std::endl;
}

static int send_packets_out(void *ctx, const struct lsquic_out_spec *specs, unsigned n_specs) {
    auto *client_ctx = static_cast<quic_client_ctx *>(ctx);
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    for (unsigned i = 0; i < n_specs; ++i) {
        msg.msg_name = (void *)specs[i].dest_sa;
        msg.msg_namelen = sizeof(struct sockaddr_in);
        msg.msg_iov = (struct iovec *)specs[i].iov;
        msg.msg_iovlen = specs[i].iovlen;
        
        // 计算总包长度
        size_t total_len = 0;
        for (unsigned j = 0; j < specs[i].iovlen; j++) {
            total_len += specs[i].iov[j].iov_len;
        }
        
        // 检查包长度，QUIC要求初始包至少1200字节
        if (total_len < 1200) {
            std::cout << "WARNING: Packet too small (" << total_len << " bytes), QUIC requires at least 1200 bytes for initial packets" << std::endl;
            std::cout << "This may cause connection failure" << std::endl;
        }
        
        if (sendmsg(client_ctx->sock_fd, &msg, 0) < 0) {
            std::cerr << "sendmsg failed: " << strerror(errno) << std::endl;
        } else {
            std::cout << "Sent " << specs[i].iovlen << " packets to server (total: " << total_len << " bytes)" << std::endl;
            // 打印发送的包内容（前16字节）
            for (unsigned j = 0; j < specs[i].iovlen; j++) {
                if (specs[i].iov[j].iov_len > 0) {
                    std::cout << "Packet " << j << " first 16 bytes: ";
                    unsigned char* data = (unsigned char*)specs[i].iov[j].iov_base;
                    for (int k = 0; k < std::min(16, (int)specs[i].iov[j].iov_len); k++) {
                        printf("%02x ", data[k]);
                    }
                    std::cout << std::endl;
                }
            }
        }
    }
    return 0;
}

static void on_socket(evutil_socket_t fd, short what, void *arg) {
    quic_client_ctx *client_ctx = (quic_client_ctx *)arg;
    unsigned char buf[4096];
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    ssize_t nread = recvfrom(client_ctx->sock_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&peer_addr, &peer_addr_len);
    if (nread >= 0) {
        std::cout << "Received " << nread << " bytes from server" << std::endl;
        
        // 打印前几个字节用于调试
        if (nread > 0) {
            std::cout << "First 16 bytes: ";
            for (int i = 0; i < std::min(16, (int)nread); i++) {
                printf("%02x ", buf[i]);
            }
            std::cout << std::endl;
        }
        
        // 总是处理接收到的包，即使连接看起来已关闭
        // 因为 QUIC 可能需要处理 Retry 包等
        struct sockaddr_storage local_addr;
        socklen_t local_addr_len = sizeof(local_addr);
        if (getsockname(client_ctx->sock_fd, (struct sockaddr *)&local_addr, &local_addr_len) == 0) {
            lsquic_engine_packet_in(client_ctx->engine, buf, nread,
                                    (struct sockaddr *)&local_addr, (struct sockaddr *)&peer_addr, nullptr, 0);
        } else {
            // 如果获取本地地址失败，使用默认的本地地址
            struct sockaddr_in default_local;
            memset(&default_local, 0, sizeof(default_local));
            default_local.sin_family = AF_INET;
            default_local.sin_addr.s_addr = htonl(INADDR_ANY);
            lsquic_engine_packet_in(client_ctx->engine, buf, nread,
                                    (struct sockaddr *)&default_local, (struct sockaddr *)&peer_addr, nullptr, 0);
        }
    }
    // 重新注册事件
    event_add(client_ctx->ev_socket, nullptr);
}

static void on_tick(evutil_socket_t, short, void *arg) {
    quic_client_ctx *client_ctx = (quic_client_ctx *)arg;
    lsquic_engine_process_conns(client_ctx->engine);
    
    // 检查是否有未发送的数据包
    if (lsquic_engine_has_unsent_packets(client_ctx->engine)) {
        std::cout << "Engine has unsent packets, sending..." << std::endl;
        lsquic_engine_send_unsent_packets(client_ctx->engine);
    }
    
    // 检查连接数量
    unsigned conn_count = lsquic_engine_get_conns_count(client_ctx->engine);
    if (conn_count > 0) {
        std::cout << "Active connections: " << conn_count << std::endl;
    }
    
    // 重新注册定时器
    struct timeval tv = {0, 1000 * 100}; // 100ms
    event_add(client_ctx->ev_tick, &tv);
}
int main() {
    // 尝试不同的 QUIC 服务器
    const char* target_host = "quic.rocks";  // 官方QUIC测试服务器
    const int target_port = 443;
    
    // 其他可用的测试服务器：
    // - quic.rocks:4433 (官方QUIC测试服务器)
    // - quic.rocks:443 (标准HTTPS端口)
    // - www.google.com:443 (Google QUIC)
    // - www.cloudflare.com:443 (Cloudflare QUIC)
    // - quic.aiortc.org:443 (AIO RTC QUIC)
    // - http3-test.litespeedtech.com:443 (LiteSpeed HTTP/3)
    // - nghttp2.org:443 (nghttp2 HTTP/3)
    
    // 如果要测试本地服务器，可以修改为：
    // const char* target_host = "127.0.0.1";
    // const int target_port = 12345;
    
    // 初始化 BoringSSL
    SSL_library_init();
    SSL_load_error_strings();
    
    quic_client_ctx *client_ctx = (quic_client_ctx *)malloc(sizeof(quic_client_ctx));
    memset(client_ctx, 0, sizeof(quic_client_ctx));

    if (lsquic_global_init(LSQUIC_GLOBAL_CLIENT) != 0) {
        std::cerr << "Failed to initialize lsquic library" << std::endl;
        return -1;
    }

    lsquic_set_log_level("debug");
    lsquic_logger_lopt("event=debug,conn=debug,stream=debug,handshake=debug");

    lsquic_stream_if stream_callbacks = {
        .on_new_conn = on_new_conn,
        .on_goaway_received = nullptr,
        .on_conn_closed = on_conn_closed,
        .on_new_stream = on_new_stream,
        .on_read = on_stream_read,
        .on_write = on_stream_write,
        .on_close = on_stream_close,
        .on_dg_write = nullptr,
        .on_datagram = nullptr,
        .on_hsk_done = [](lsquic_conn_t *c, enum lsquic_hsk_status s) {
            std::cout << "=== HANDSHAKE COMPLETED ===" << std::endl;
            std::cout << "Handshake status: ";
            switch (s) {
                case LSQ_HSK_FAIL: 
                    std::cout << "FAIL"; 
                    break;
                case LSQ_HSK_OK: 
                    std::cout << "OK - SUCCESS!"; 
                    break;
                case LSQ_HSK_RESUMED_OK: 
                    std::cout << "RESUMED_OK - SUCCESS!"; 
                    break;
                case LSQ_HSK_RESUMED_FAIL: 
                    std::cout << "RESUMED_FAIL"; 
                    break;
            }
            std::cout << std::endl;
            
            // 获取连接信息
            enum lsquic_version version = lsquic_conn_quic_version(c);
            std::cout << "QUIC version: " << version << std::endl;
            
            // 获取连接信息
            struct lsquic_conn_info info;
            if (lsquic_conn_get_info(c, &info) == 0) {
                std::cout << "Connection info - RTT: " << info.lci_rtt << " us" << std::endl;
                std::cout << "Connection info - Packets sent: " << info.lci_pkts_sent << std::endl;
                std::cout << "Connection info - Packets received: " << info.lci_pkts_rcvd << std::endl;
                std::cout << "Connection info - Bytes sent: " << info.lci_bytes_sent << std::endl;
                std::cout << "Connection info - Bytes received: " << info.lci_bytes_rcvd << std::endl;
            }
            
            // 检查连接状态
            char errbuf[256];
            enum LSQUIC_CONN_STATUS status = lsquic_conn_status(c, errbuf, sizeof(errbuf));
            std::cout << "Connection status after handshake: " << status << std::endl;
            
            if (s == LSQ_HSK_OK || s == LSQ_HSK_RESUMED_OK) {
                std::cout << "SUCCESS: Handshake completed successfully!" << std::endl;
                std::cout << "Connection is now ready for data transfer!" << std::endl;
            }
        },
        .on_new_token = nullptr,
        .on_sess_resume_info = nullptr,
        .on_reset = nullptr,
        .on_conncloseframe_received = [](lsquic_conn_t *c, int app_error, uint64_t error_code, const char *reason, int reason_len) {
            std::cout << "Connection close frame received:" << std::endl;
            std::cout << "  App error: " << app_error << std::endl;
            std::cout << "  Error code: " << error_code << std::endl;
            if (reason_len > 0) {
                std::cout << "  Reason: " << std::string(reason, reason_len) << std::endl;
            }
        },
    };

    lsquic_engine_settings settings;
    memset(&settings, 0, sizeof(settings));
    lsquic_engine_init_settings(&settings, 0);
    
    // 支持多个 QUIC 版本，优先使用最新版本
    settings.es_versions = LSQUIC_DF_VERSIONS;
    
    // 禁用证书验证（仅用于测试）
    settings.es_check_tp_sanity = 0;
    
    // 设置 QUIC 特定参数
    settings.es_max_streams_in = 100;
    
    // 增加握手超时时间
    settings.es_handshake_to = 60 * 1000 * 1000;  // 60秒
    settings.es_idle_conn_to = 120 * 1000 * 1000; // 120秒空闲超时
    settings.es_ping_period = 30 * 1000 * 1000;   // 30秒ping周期
    settings.es_support_tcid0 = 1;  // 支持TCID0
    settings.es_support_nstp = 1;   // 支持NSTP
    settings.es_delayed_acks = 1;   // 启用延迟ACK

    lsquic_engine_api engine_api = {};
    engine_api.ea_settings = &settings;
    engine_api.ea_stream_if = &stream_callbacks;
    engine_api.ea_stream_if_ctx = client_ctx;
    engine_api.ea_packets_out = send_packets_out;
    engine_api.ea_packets_out_ctx = client_ctx;
    engine_api.ea_alpn = "h3,h3-29,h3-27,h3-25,h3-24,h3-23";  // 尝试更多ALPN
    
    // 添加 SSL 上下文获取函数
    engine_api.ea_get_ssl_ctx = [](void *peer_ctx, const struct sockaddr *local) -> struct ssl_ctx_st* {
        std::cout << "Creating SSL context..." << std::endl;
        
        // 使用TLS_method()，和官方示例一致
        SSL_CTX *ctx = SSL_CTX_new(TLS_method());
        if (!ctx) {
            std::cerr << "Failed to create SSL context" << std::endl;
            return nullptr;
        }
        
        // 设置TLS版本为TLS1.3
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
        
        // 设置默认验证路径
        SSL_CTX_set_default_verify_paths(ctx);
        
        // 设置ALPN协议（关键修改）
        const unsigned char alpn_protos[] = {
            8, 'h', '3', '-', '2', '9',    // h3-29
            2, 'h', '3',                   // h3
        };
        SSL_CTX_set_alpn_protos(ctx, alpn_protos, sizeof(alpn_protos));
        
        // 禁用证书验证（仅用于测试）
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        
        std::cout << "SSL context created successfully" << std::endl;
        return ctx;
    };

    client_ctx->engine = lsquic_engine_new(0, &engine_api);
    std::cout << "client_ctx->engine=" << client_ctx->engine << std::endl;
    if (!client_ctx->engine) {
        std::cerr << "Failed to create lsquic engine." << std::endl;
        free(client_ctx);
        return -1;
    }

    client_ctx->sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_ctx->sock_fd < 0) {
        std::cerr << "Failed to create socket." << std::endl;
        lsquic_engine_destroy(client_ctx->engine);
        free(client_ctx);
        return -1;
    }

    struct sockaddr_in remote_addr;
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(target_port);
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    int err = getaddrinfo(target_host, nullptr, &hints, &res);
    if (err != 0 || !res) {
        std::cerr << "DNS lookup failed: " << gai_strerror(err) << std::endl;
        close(client_ctx->sock_fd);
        lsquic_engine_destroy(client_ctx->engine);
        free(client_ctx);
        return -1;
    }
    remote_addr.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    std::cout << "remote_addr.sin_family=" << remote_addr.sin_family << std::endl;
    std::cout << "remote_addr.sin_port=" << ntohs(remote_addr.sin_port) << std::endl;
    char ipbuf[INET_ADDRSTRLEN];
    std::cout << "remote_addr.sin_addr=" << inet_ntop(AF_INET, &remote_addr.sin_addr, ipbuf, sizeof(ipbuf)) << std::endl;

    // 初始化libevent
    client_ctx->eb = event_base_new();
    if (!client_ctx->eb) {
        std::cerr << "Failed to create event_base." << std::endl;
        close(client_ctx->sock_fd);
        lsquic_engine_destroy(client_ctx->engine);
        free(client_ctx);
        return -1;
    }
    // 注册socket事件
    client_ctx->ev_socket = event_new(client_ctx->eb, client_ctx->sock_fd, EV_READ|EV_PERSIST, on_socket, client_ctx);
    event_add(client_ctx->ev_socket, nullptr);
    // 注册定时器事件
    struct timeval tv = {0, 1000 * 100}; // 100ms
    client_ctx->ev_tick = event_new(client_ctx->eb, -1, EV_PERSIST, on_tick, client_ctx);
    event_add(client_ctx->ev_tick, &tv);

    // 设置本地地址（使用任意可用端口）
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = 0;  // 让系统自动分配端口

    // 尝试使用 IETF QUIC v1 (更广泛支持)
    std::cout << "Attempting to connect to " << target_host << ":" << target_port << std::endl;
    std::cout << "Using QUIC version: LSQVER_I002 (latest)" << std::endl;
    
    // 尝试不同的QUIC版本 - 只尝试最广泛支持的版本
    lsquic_conn_t *conn = nullptr;
    enum lsquic_version versions_to_try[] = {LSQVER_I001,LSQVER_I002, LSQVER_ID29, LSQVER_I001, LSQVER_ID27};
    const char* version_names[] = {"LSQVER_I001","LSQVER_I002", "LSQVER_ID29", "LSQVER_I001", "LSQVER_ID27"};
    
    for (int i = 0; i < 5 && !conn; i++) {
        std::cout << "Trying QUIC version: " << version_names[i] << std::endl;
        conn = lsquic_engine_connect(client_ctx->engine, versions_to_try[i],
            (struct sockaddr *)&local_addr, (struct sockaddr *)&remote_addr,
            &client_ctx, nullptr, target_host, 0,
            nullptr, 0, nullptr, 0);
        
        if (conn) {
            std::cout << "Successfully created connection with version: " << version_names[i] << std::endl;
            break;
        } else {
            std::cout << "Failed to create connection with version: " << version_names[i] << std::endl;
        }
    }
    
    if (!conn) {
        std::cerr << "Failed to create connection with any QUIC version!" << std::endl;
        event_base_free(client_ctx->eb);
        close(client_ctx->sock_fd);
        lsquic_engine_destroy(client_ctx->engine);
        free(client_ctx);
        return -1;
    }

    std::cout << "Starting event loop..." << std::endl;
    
    // 设置事件循环超时，避免无限等待
    struct timeval timeout = {60, 0}; // 60秒超时
    event_base_loopexit(client_ctx->eb, &timeout);
    
    event_base_dispatch(client_ctx->eb);
    std::cout << "Event loop finished." << std::endl;
    
    // 检查连接状态
    if (client_ctx->conn_h && client_ctx->conn_h->conn) {
        // 检查连接状态
        enum LSQUIC_CONN_STATUS status = lsquic_conn_status(client_ctx->conn_h->conn, nullptr, 0);
        std::cout << "Connection status: ";
        switch (status) {
            case LSCONN_ST_HSK_IN_PROGRESS:
                std::cout << "HANDSHAKE_IN_PROGRESS";
                break;
            case LSCONN_ST_CONNECTED:
                std::cout << "CONNECTED";
                break;
            case LSCONN_ST_PEER_GOING_AWAY:
                std::cout << "PEER_GOING_AWAY";
                break;
            case LSCONN_ST_GOING_AWAY:
                std::cout << "GOING_AWAY";
                break;
            case LSCONN_ST_CLOSED:
                std::cout << "CLOSED";
                break;
            case LSCONN_ST_ERROR:
                std::cout << "ERROR";
                break;
            default:
                std::cout << "UNKNOWN(" << (int)status << ")";
        }
        std::cout << std::endl;
        
        // 如果连接已建立，尝试发送数据
        if (status == LSCONN_ST_CONNECTED) {
            std::cout << "Connection established! Attempting to send data..." << std::endl;
            
            // 创建流并发送数据
            lsquic_conn_make_stream(client_ctx->conn_h->conn);
            std::cout << "Stream creation requested" << std::endl;
        } else if (status == LSCONN_ST_HSK_IN_PROGRESS) {
            std::cout << "Handshake still in progress..." << std::endl;
        } else {
            std::cout << "Connection not ready, status: " << (int)status << std::endl;
        }
    }
    
    // 等待一段时间，确保所有包都被处理
    std::cout << "Waiting for additional packets..." << std::endl;
    sleep(2);  // 等待2秒
    event_free(client_ctx->ev_socket);
    event_free(client_ctx->ev_tick);
    event_base_free(client_ctx->eb);
    lsquic_engine_destroy(client_ctx->engine);
    close(client_ctx->sock_fd);
    free(client_ctx);
    lsquic_global_cleanup();
    
    // 清理 BoringSSL
    EVP_cleanup();
    
    return 0;
} 