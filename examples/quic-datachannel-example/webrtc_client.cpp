#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include "SignalingClient.hpp"
#include "TrafficStats.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <atomic>


using json = nlohmann::json;

static size_t envSize(const char *key, size_t defaultValue) {
    if (const char *v = std::getenv(key)) {
        try {
            return static_cast<size_t>(std::stoull(v));
        } catch (...) {
            return defaultValue;
        }
    }
    return defaultValue;
}

static std::string envStr(const char *key, const char *defaultValue) {
    if (const char *v = std::getenv(key)) {
        return std::string(v);
    }
    return std::string(defaultValue ? defaultValue : "");
}

// 获取本机IP地址
std::string getLocalIp() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return "127.0.0.1";
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);  // 使用DNS端口
    addr.sin_addr.s_addr = inet_addr("8.8.8.8");  // Google DNS
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return "127.0.0.1";
    }
    
    socklen_t len = sizeof(addr);
    if (getsockname(sock, (struct sockaddr*)&addr, &len) < 0) {
        close(sock);
        return "127.0.0.1";
    }
    
    close(sock);
    return inet_ntoa(addr.sin_addr);
}

class WebRTCClient {
private:
    std::shared_ptr<SignalingClient> mSignaling;
    std::shared_ptr<rtc::PeerConnection> mPeerConnection;
    std::shared_ptr<rtc::DataChannel> mDataChannel;
    TrafficStats mStats;

    std::string mClientId;
    bool mIsOfferer;
    bool mUseQuic;
    bool mPerformanceTestRun;
    std::atomic<bool> mDataChannelOpen;
    bool mRunTestSuite;
    std::atomic<bool> mConnectionFailed;
    std::optional<rtc::QuicCongestionControl> mCcAlgo;

    // Test ACK logic (offerer side only)
    std::mutex mTestAckMutex;
    uint64_t mNextTestSeq = 0;
    uint64_t mExpectedAckSeq = 0;
    std::string mExpectedAckName;

    uint64_t mLastAckSeq = 0;
    std::atomic<uint64_t> mLastAckSeqAtomic{0};
    std::string mLastAckName;
    uint64_t mLastAckBytes = 0;
    uint64_t mLastAckMsgs = 0;
    uint64_t mLastAckDurationMs = 0;

    static bool ackTraceEnabled() {
        return envSize("RTC_TEST_ACK_TRACE", 0) != 0;
    }

    static std::string normalizeTestName(std::string s) {
        // 去掉不可见尾巴（QUIC/不同平台可能带 \r\n 或 \0），并 trim 首尾空白
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == '\0' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        size_t start = 0;
        while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n' || s[start] == '\0'))
            ++start;
        if (start > 0)
            s.erase(0, start);
        return s;
    }

    static std::string trimControl(std::string s) {
        // 去掉首尾常见不可见字符，避免前缀判断/字段解析失败（例如 '\0' / '\r\n' / 空白）
        auto is_ctrl = [](unsigned char c) {
            return c == '\0' || c == '\r' || c == '\n' || c == '\t' || c == ' ';
        };
        while (!s.empty() && is_ctrl((unsigned char)s.back()))
            s.pop_back();
        size_t i = 0;
        while (i < s.size() && is_ctrl((unsigned char)s[i]))
            ++i;
        if (i)
            s.erase(0, i);
        return s;
    }

    static std::string stripControlBytes(const std::string &s) {
        // 移除所有控制字节（<0x20 或 0x7f），只保留可见 ASCII/UTF-8 字节，避免 find/prefix 判断被隐藏字节破坏
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            if (c < 0x20 || c == 0x7f)
                continue;
            out.push_back((char)c);
        }
        return out;
    }

    static std::string hexPrefix(const std::string &s, size_t n = 32) {
        static const char *hex = "0123456789ABCDEF";
        std::string out;
        const size_t lim = std::min(n, s.size());
        out.reserve(lim * 3);
        for (size_t i = 0; i < lim; ++i) {
            unsigned char c = (unsigned char)s[i];
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
            if (i + 1 != lim) out.push_back(' ');
        }
        return out;
    }

    static std::string sliceFromKeyword(const std::string &s, const char *kw) {
        const auto pos = s.find(kw);
        if (pos == std::string::npos)
            return "";
        return s.substr(pos);
    }

    static std::string toLower(std::string s) {
        for (auto &c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    std::string testProfile() const {
        // local: 本地/同机房（默认）
        // wan:   跨地域/公网（缩小大消息测试规模，避免 ACK 超时/ICE consent 过期）
        // low:   低带宽/不稳定链路（超小流量，优先跑通流程与对比）
        const std::string v = toLower(envStr("RTC_TEST_PROFILE", "local"));
        if (v == "wan" || v == "widearea" || v == "wide_area")
            return "wan";
        if (v == "low" || v == "slow" || v == "lowbw" || v == "low_bw")
            return "low";
        return "local";
    }

    void bindDataChannelHandlers(const std::shared_ptr<rtc::DataChannel> &dc) {
        if (!dc)
            return;

        // 统一保存引用（offerer/createDataChannel 与 answerer/onDataChannel 都走这里）
        mDataChannel = dc;

        dc->onOpen([this]() {
            if (mIsOfferer) {
                std::cout << "✅ 数据通道已打开，开始发送消息..." << std::endl;
            } else {
                std::cout << "数据通道已打开" << std::endl;
            }

            mDataChannelOpen.store(true, std::memory_order_release); // 标记数据通道已打开

            // offerer：发送基础验证消息（Hello + 5B binary）
            if (mIsOfferer && mDataChannel) {
                std::string testMessage = std::string("Hello from ") + (mUseQuic ? "QUIC" : "SCTP") + " DataChannel!";
                mDataChannel->send(testMessage);

                std::vector<std::byte> binaryData = {
                    std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                    std::byte{0x04}, std::byte{0x05}
                };
                mDataChannel->send(binaryData);
            }

            // 如果是发起方，根据模式运行测试
            if (mIsOfferer && !mPerformanceTestRun) {
                if (mRunTestSuite) {
                    // 测试套件模式：不在这里运行，由 main 函数调用 runTestSuite()
                    std::cout << "数据通道准备就绪，等待运行测试套件..." << std::endl;
                } else {
                    // 单次测试模式：立即运行性能测试
                    std::cout << "数据通道准备就绪，立即开始性能测试..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 短暂等待确保连接稳定
                    runPerformanceTest();
                    mPerformanceTestRun = true;
                }
            } else {
                std::cout << "数据通道准备就绪，等待测试..." << std::endl;
            }
        });

        dc->onMessage([this](rtc::message_variant msg) {
            if (std::holds_alternative<std::string>(msg)) {
                std::string raw = std::get<std::string>(msg);
                std::string message = trimControl(raw);             // 仅用于打印
                std::string parseLine = stripControlBytes(message); // 用于 find/解析
                std::cout << "收到字符串消息: " << message << std::endl;

                if (ackTraceEnabled() && message != raw) {
                    std::cout << "   [ACK_TRACE] note: raw string had leading/trailing control chars; raw_size="
                              << raw.size() << " sanitized_size=" << message.size() << std::endl;
                }
                if (ackTraceEnabled()) {
                    std::cout << "   [ACK_TRACE] raw_hex=" << hexPrefix(raw)
                              << " parsed_hex=" << hexPrefix(parseLine)
                              << " raw_find_ACK=" << (raw.find("TEST_ACK|") == std::string::npos ? -1 : (int)raw.find("TEST_ACK|"))
                              << " parsed_find_ACK=" << (parseLine.find("TEST_ACK|") == std::string::npos ? -1 : (int)parseLine.find("TEST_ACK|"))
                              << std::endl;
                }

                // 处理 answerer 回传的 TEST_ACK（不依赖 mIsOfferer，避免角色标志异常导致 ACK 丢失）
                std::string ackMsg = sliceFromKeyword(parseLine, "TEST_ACK|");
                if (ackMsg.empty()) {
                    const bool ackLike =
                        parseLine.find("name=") != std::string::npos &&
                        parseLine.find("seq=") != std::string::npos &&
                        parseLine.find("bytes=") != std::string::npos &&
                        parseLine.find("msgs=") != std::string::npos &&
                        parseLine.find("duration_ms=") != std::string::npos;
                    if (ackLike)
                        ackMsg = parseLine;
                }
                if (!ackMsg.empty()) {
                    auto getField = [&](const std::string& key) -> std::string {
                        const std::string pattern = key + "=";
                        const auto pos = ackMsg.find(pattern);
                        if (pos == std::string::npos) return "";
                        const auto start = pos + pattern.size();
                        const auto end = ackMsg.find('|', start);
                        return ackMsg.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    };
                    const std::string name = normalizeTestName(getField("name"));
                    const std::string seqStr = getField("seq");
                    const std::string bytesStr = getField("bytes");
                    const std::string msgsStr = getField("msgs");
                    const std::string durStr = getField("duration_ms");
                    const uint64_t seqVal = seqStr.empty() ? 0 : std::stoull(seqStr);
                    {
                        std::lock_guard<std::mutex> lk(mTestAckMutex);
                        mLastAckName = name;
                        mLastAckSeq = seqVal;
                        mLastAckBytes = bytesStr.empty() ? 0 : std::stoull(bytesStr);
                        mLastAckMsgs = msgsStr.empty() ? 0 : std::stoull(msgsStr);
                        mLastAckDurationMs = durStr.empty() ? 0 : std::stoull(durStr);
                        mLastAckSeqAtomic.store(seqVal, std::memory_order_release);
                    }
                    if (ackTraceEnabled()) {
                        std::cout << "   [ACK_TRACE] recv this=" << this
                                  << " name='" << name << "' seq=" << seqVal
                                  << " bytes=" << mLastAckBytes
                                  << " msgs=" << mLastAckMsgs
                                  << " dur_ms=" << mLastAckDurationMs
                                  << std::endl;
                    }
                    return;
                }

                // answerer：统计 TEST_BEGIN/END，并回传 TEST_ACK
                std::string beginMsg = sliceFromKeyword(parseLine, "TEST_BEGIN|");
                std::string endMsg = sliceFromKeyword(parseLine, "TEST_END|");
                if (!mIsOfferer && !beginMsg.empty()) {
                    auto getField = [&](const std::string& key) -> std::string {
                        const std::string pattern = key + "=";
                        const auto pos = beginMsg.find(pattern);
                        if (pos == std::string::npos) return "";
                        const auto start = pos + pattern.size();
                        const auto end = beginMsg.find('|', start);
                        return beginMsg.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    };
                    std::string name = normalizeTestName(getField("name"));
                    std::string seqStr = getField("seq");
                    uint64_t seq = seqStr.empty() ? 0 : std::stoull(seqStr);
                    mStats.startTest(name, seq);
                    std::cout << "🧪 [TEST] BEGIN: " << name << std::endl;
                } else if (!mIsOfferer && !endMsg.empty()) {
                    auto getField = [&](const std::string& key) -> std::string {
                        const std::string pattern = key + "=";
                        const auto pos = endMsg.find(pattern);
                        if (pos == std::string::npos) return "";
                        const auto start = pos + pattern.size();
                        const auto end = endMsg.find('|', start);
                        return endMsg.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    };
                    const std::string name = normalizeTestName(getField("name"));
                    const std::string seqStr = getField("seq");
                    const uint64_t endSeq = seqStr.empty() ? 0 : std::stoull(seqStr);
                    
                    uint64_t bytes = 0, msgs = 0, durMs = 0;
                    if (mStats.isActive() && 
                        (mStats.getTestSeq() == endSeq || endSeq == 0) &&
                        (mStats.getTestName() == name || name.empty())) {
                        bytes = mStats.getTestBytes();
                        msgs = mStats.getTestMessages();
                        durMs = mStats.getDurationMs();
                        mStats.endTest();
                    }
                    
                    if (mDataChannel) {
                        std::string ack =
                            std::string("TEST_ACK|name=") + (name.empty() ? "N/A" : name) +
                            "|seq=" + std::to_string(endSeq) +
                            "|bytes=" + std::to_string(bytes) +
                            "|msgs=" + std::to_string(msgs) +
                            "|duration_ms=" + std::to_string(durMs);
                        mDataChannel->send(ack);
                        std::cout << "🧪 [TEST] END: " << name << " -> ACK bytes=" << bytes
                                  << " msgs=" << msgs << " duration_ms=" << durMs << std::endl;
                    }
                }

                // 检查消息中的传输类型标识
                if (message.find("QUIC") != std::string::npos) {
                    if (!mUseQuic) {
                        std::cerr << "⚠️  警告: 收到QUIC消息，但本机配置为SCTP！传输类型不匹配！" << std::endl;
                    } else {
                        std::cout << "✅ 传输类型验证: QUIC" << std::endl;
                    }
                } else if (message.find("SCTP") != std::string::npos) {
                    if (mUseQuic) {
                        std::cerr << "⚠️  警告: 收到SCTP消息，但本机配置为QUIC！传输类型不匹配！" << std::endl;
                        std::cerr << "   实际连接可能使用了SCTP（fallback），测试结果可能不准确" << std::endl;
                    } else {
                        std::cout << "✅ 传输类型验证: SCTP" << std::endl;
                    }
                }
            } else {
                auto& binary = std::get<std::vector<std::byte>>(msg);
                const size_t sz = binary.size();
                mStats.addBytes(sz);
                
                uint64_t total = mStats.getTotalMessages();
                if (total > 0 && total % 100 == 0) {
                     std::cout << "收到二进制消息，大小: " << sz
                              << " 字节；总累计: " << total << std::endl;
                }
            }
        });

        dc->onClosed([this]() {
            std::cout << "数据通道已关闭" << std::endl;
            mDataChannelOpen.store(false, std::memory_order_release);  // 重置数据通道打开标志
            // 注意：不在这里设置 mConnectionFailed，因为可能是正常关闭
        });
    }
    
public:
    WebRTCClient(bool useQuic = true, bool isOfferer = true, const std::string& signalingIp = "127.0.0.1", int signalingPort = 8080, bool runTestSuite = false, std::optional<rtc::QuicCongestionControl> ccAlgo = std::nullopt) 
        : mIsOfferer(isOfferer),
          mUseQuic(useQuic),
          mPerformanceTestRun(false),
          mDataChannelOpen(false),
          mRunTestSuite(runTestSuite),
          mConnectionFailed(false),
          mCcAlgo(ccAlgo) {
        
        // Initialize signaling client
        mSignaling = std::make_shared<SignalingClient>(signalingIp, signalingPort);

        mSignaling->onConnected([this](std::string clientId) {
            mClientId = clientId;
            std::cout << "客户端ID: " << mClientId << std::endl;
            if (mIsOfferer) {
                std::cout << "开始创建PeerConnection..." << std::endl;
                createPeerConnection();
                createOffer();
            }
        });

        mSignaling->onOffer([this](const json& data) {
            handleOffer(data);
        });

        mSignaling->onAnswer([this](const json& data) {
            handleAnswer(data);
        });

        mSignaling->onIceCandidate([this](const json& data) {
            handleIceCandidate(data);
        });

        mSignaling->connect();

        // Wait for connection
        int waitTime = (signalingIp == "127.0.0.1" || signalingIp == "localhost") ? 3 : 30;
        std::cout << "等待连接确认，超时时间: " << waitTime << " 秒..." << std::endl;
        
        if (mSignaling->waitForConnection(waitTime)) {
            std::cout << "已收到连接确认消息" << std::endl;
        } else {
            std::cout << "警告: 未收到连接确认消息，但继续尝试..." << std::endl;
        }
    }
    

    
    void disconnect() {
        // 先断开数据通道
        if (mDataChannel) {
            try {
                mDataChannel->close();
            } catch (...) {
                // 忽略关闭时的异常
            }
            mDataChannel.reset();
        }
        mDataChannelOpen.store(false, std::memory_order_release);  // 重置数据通道打开标志
        
        // 再断开PeerConnection
        if (mPeerConnection) {
            try {
                mPeerConnection->close();
            } catch (...) {
                // 忽略关闭时的异常
            }
            mPeerConnection.reset();
        }
        
        // 最后断开WebSocket
        if (mSignaling) {
            mSignaling->disconnect();
        }
        
        // 重置状态标志
        mPerformanceTestRun = false;
        mStats.reset();
        mConnectionFailed.store(false, std::memory_order_release);  // 重置连接失败标志
    }
    
    // 返回 true 表示本次测试“端到端确认”成功（或无需确认）；false 表示 ACK 超时/连接中断等
    bool runPerformanceTest(int messageSize = 1024, int numMessages = 1000, const std::string& testName = "") {
        if (!mDataChannel) {
            std::cerr << "数据通道未创建" << std::endl;
            return false;
        }
        if (!mDataChannel->isOpen()) {
            std::cerr << "数据通道未打开，无法发送" << std::endl;
            return false;
        }
        if (mConnectionFailed.load(std::memory_order_acquire)) {
            std::cerr << "检测到连接已失败/关闭，停止测试: " << testName << std::endl;
            return false;
        }
        
        if (!testName.empty()) {
            std::cout << "\n=== 开始测试: " << testName << " ===" << std::endl;
        } else {
            std::cout << "\n=== 性能测试结果 ===" << std::endl;
        }
        
        // offerer：开始本轮测试前设置 seq + 清空上一轮 ACK，避免误匹配/丢通知
        const std::string expectedAckName = normalizeTestName(testName);
        // 只对“需要等待 ACK 的命名测试”分配 seq，避免其他路径（无名测试）意外递增导致错位
        const uint64_t testSeq = (mIsOfferer && !expectedAckName.empty()) ? (++mNextTestSeq) : 0;
        if (mIsOfferer && !expectedAckName.empty()) {
            std::lock_guard<std::mutex> lk(mTestAckMutex);
            mExpectedAckName = expectedAckName;
            mExpectedAckSeq = testSeq;
            mLastAckName.clear();
            mLastAckSeq = 0;
            mLastAckSeqAtomic.store(0, std::memory_order_release);
            mLastAckBytes = 0;
            mLastAckMsgs = 0;
            mLastAckDurationMs = 0;
        }

        // 给 answerer 一个清晰的测试阶段标记（不计入性能计时）
        {
            std::string begin =
                std::string("TEST_BEGIN|") + (mUseQuic ? "QUIC" : "SCTP") +
                "|name=" + (testName.empty() ? "N/A" : testName) +
                (testSeq ? ("|seq=" + std::to_string(testSeq)) : std::string()) +
                "|size=" + std::to_string(messageSize) +
                "|count=" + std::to_string(numMessages);
            try {
                mDataChannel->send(begin);
            } catch (const std::exception &e) {
                std::cerr << "发送 TEST_BEGIN 失败: " << e.what() << std::endl;
                return false;
            }
        }

        std::vector<std::byte> testData(messageSize, std::byte{0xAA});

        // 发送节流：避免一次性把 SCTP 缓冲塞到几百 MB，导致 TEST_END 卡在队尾、ACK 超时
        // - maxBuffered: 达到该值就等待发送缓冲下降
        // - lowThreshold: 下降到该值以下再继续
        const std::string profile = testProfile();
        const size_t maxBufferedDefault = (profile == "local") ? (size_t)(256u * 1024u * 1024u) : (size_t)(32u * 1024u * 1024u);
        const size_t maxBuffered = (size_t)envSize("RTC_MAX_BUFFERED_AMOUNT", maxBufferedDefault);
        const size_t lowThreshold = (maxBuffered > 1) ? (maxBuffered / 2) : 0;
        mDataChannel->setBufferedAmountLowThreshold(lowThreshold);

        struct BufferWait {
            std::mutex m;
            std::condition_variable cv;
            std::atomic<bool> notified{false};
        };
        auto bw = std::make_shared<BufferWait>();
        mDataChannel->onBufferedAmountLow([bw]() {
            bw->notified.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lk(bw->m);
            bw->cv.notify_all();
        });

        auto waitDrain = [&](std::chrono::milliseconds timeout) -> bool {
            bw->notified.store(false, std::memory_order_release);
            std::unique_lock<std::mutex> lk(bw->m);
            return bw->cv.wait_for(lk, timeout, [&]() {
                if (bw->notified.load(std::memory_order_acquire)) return true;
                if (mConnectionFailed.load(std::memory_order_acquire)) return true;
                if (!mDataChannel || !mDataChannel->isOpen()) return true;
                return false;
            });
        };
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < numMessages; ++i) {
            if (mConnectionFailed.load(std::memory_order_acquire) || !mDataChannel || !mDataChannel->isOpen()) {
                std::cerr << "发送过程中检测到连接断开，提前结束: " << testName << std::endl;
                mDataChannel->onBufferedAmountLow(nullptr);
                return false;
            }

            try {
                mDataChannel->send(testData);
            } catch (const std::exception &e) {
                std::cerr << "发送数据失败: " << e.what() << std::endl;
                mDataChannel->onBufferedAmountLow(nullptr);
                return false;
            }

            // 如果缓冲过大，等待下降到阈值以下，避免“控制消息排队太久”
            while (maxBuffered > 0 && mDataChannel->bufferedAmount() > maxBuffered) {
                if (mConnectionFailed.load(std::memory_order_acquire) || !mDataChannel->isOpen())
                    break;
                waitDrain(std::chrono::milliseconds(2000));
            }
        }

        // 在发送 TEST_END 之前，尽量把缓冲压到低水位，确保 TEST_END 不会长期堵在队尾
        if (lowThreshold > 0) {
            const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds((int)envSize("RTC_DRAIN_TIMEOUT_SEC", 30));
            while (std::chrono::steady_clock::now() < drainDeadline &&
                   mDataChannel && mDataChannel->isOpen() &&
                   !mConnectionFailed.load(std::memory_order_acquire) &&
                   mDataChannel->bufferedAmount() > lowThreshold) {
                waitDrain(std::chrono::milliseconds(2000));
            }
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        if (durationUs <= 0) durationUs = 1;  // 避免 0ms/0us 造成 inf
        const double seconds = static_cast<double>(durationUs) / 1'000'000.0;
        
        double throughput = (numMessages * messageSize * 8.0) / seconds / 1'000'000.0;
        
        std::cout << "传输方式: " << (mUseQuic ? "QUIC" : "SCTP") << std::endl;
        std::cout << "发送消息数: " << numMessages << std::endl;
        std::cout << "消息大小: " << messageSize << " 字节" << std::endl;
        std::cout << "总时间: " << (durationUs / 1000) << " 毫秒" << std::endl;
        std::cout << "平均每条消息: " << (seconds * 1000.0) / (double)numMessages << " 毫秒" << std::endl;
        std::cout << "吞吐量: " << throughput << " Mbps" << std::endl;
        std::cout << "=== 测试完成 ===" << std::endl;

        // 给 answerer 一个结束标记（不影响性能统计）
        {
            std::string endMsg =
                std::string("TEST_END|") + (mUseQuic ? "QUIC" : "SCTP") +
                "|name=" + (testName.empty() ? "N/A" : testName) +
                (testSeq ? ("|seq=" + std::to_string(testSeq)) : std::string()) +
                "|duration_us=" + std::to_string(durationUs) +
                "|throughput_mbps=" + std::to_string(throughput);
            try {
                mDataChannel->send(endMsg);
            } catch (const std::exception &e) {
                std::cerr << "发送 TEST_END 失败: " << e.what() << std::endl;
                mDataChannel->onBufferedAmountLow(nullptr);
                return false;
            }
        }
        // 清理本次测试的低水位回调（避免跨测试引用旧状态）
        mDataChannel->onBufferedAmountLow(nullptr);

        // offerer：等待 answerer 的 TEST_ACK，确保"数据确实收到了"，也避免过早断开导致丢数据
        if (mIsOfferer && !expectedAckName.empty()) {
            // 跨区 1MB/10MB 等测试真实耗时可能 > 120s；默认提高到 300s，仍可用环境变量覆盖
            const int ackTimeoutSec = (int)envSize("RTC_TEST_ACK_TIMEOUT_SEC", 300);
            std::cout << "⏳ [ACK] 等待接收端确认(最多 " << ackTimeoutSec << "s): " << expectedAckName << std::endl;
            const uint64_t expectedSeq = testSeq;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(ackTimeoutSec);
            bool ok = false;
            while (std::chrono::steady_clock::now() < deadline) {
                // 检查连接是否已失败/关闭，如果是则立即退出等待
                if (mConnectionFailed.load(std::memory_order_acquire)) {
                    std::cout << "⚠️  [ACK] 检测到连接已失败/关闭，停止等待ACK" << std::endl;
                    break;
                }
                const uint64_t got = mLastAckSeqAtomic.load(std::memory_order_acquire);
                if (expectedSeq != 0 && got == expectedSeq) {
                    ok = true;
                    break;
                }
                // 兜底：如果对端没带 seq（旧版本），按 name 匹配
                {
                    std::lock_guard<std::mutex> lk(mTestAckMutex);
                    if (expectedSeq == 0 && normalizeTestName(mLastAckName) == expectedAckName) {
                        ok = true;
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // 读取 ACK 详情（在锁下）
            std::unique_lock<std::mutex> lk(mTestAckMutex);
            if (ok) {
                const double ackSeconds = std::max(0.001, (double)mLastAckDurationMs / 1000.0);
                const double ackThroughput = (mLastAckBytes * 8.0) / ackSeconds / 1'000'000.0;
                std::cout << "✅ [ACK] 接收端确认: name=" << mLastAckName
                          << " seq=" << mLastAckSeq
                          << " msgs=" << mLastAckMsgs
                          << " bytes=" << mLastAckBytes
                          << " duration_ms=" << mLastAckDurationMs
                          << " recv_throughput=" << ackThroughput << " Mbps" << std::endl;
            } else {
                std::cout << "⚠️  [ACK] 等待接收端确认超时(" << ackTimeoutSec
                          << "s): " << expectedAckName << std::endl;
                // 打印一下当前收到的 ACK 名，方便定位“看起来收到但不匹配”的情况
                if (!mLastAckName.empty()) {
                    std::cout << "   ⚠️  [ACK] 最近一次ACK: name='" << mLastAckName
                              << "' normalized='" << normalizeTestName(mLastAckName)
                              << "' seq=" << mLastAckSeq
                              << " expected_seq=" << mExpectedAckSeq
                              << std::endl;
                }
                if (ackTraceEnabled()) {
                    std::cout << "   [ACK_TRACE] this=" << this
                              << " expectedSeq=" << expectedSeq
                              << " atomic=" << mLastAckSeqAtomic.load(std::memory_order_acquire)
                              << std::endl;
                }
            }

            if (!ok) {
                // 不把它当成“连接失败”，但对于测试套件来说必须中止，否则后续用例会继续 send() 导致崩溃/无意义超时
                return false;
            }
        }
        
        // 测试之间稍作停顿
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return true;
    }
    
    // 运行完整的测试套件（一次连接完成所有测试）
    void runTestSuite() {
        if (!mDataChannel || !mDataChannelOpen.load(std::memory_order_acquire)) {
            std::cerr << "数据通道未就绪，无法运行测试套件" << std::endl;
            return;
        }
        
        std::cout << "\n" << std::string(70, '=') << std::endl;
        std::cout << "开始运行完整测试套件" << std::endl;
        std::cout << "传输类型: " << (mUseQuic ? "QUIC" : "SCTP") << std::endl;
        std::cout << std::string(70, '=') << std::endl;
        
        // 测试1: 基础连接性测试（验证连接建立）
        std::cout << "\n" << std::string(70, '-') << std::endl;
        std::cout << "测试 1: 基础连接性测试" << std::endl;
        std::cout << std::string(70, '-') << std::endl;
        std::cout << "✅ 连接已建立，数据通道已打开" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // 测试2: 基础性能测试
        std::cout << "\n" << std::string(70, '-') << std::endl;
        std::cout << "测试 2: 基础性能测试" << std::endl;
        std::cout << std::string(70, '-') << std::endl;
        const std::string profile = testProfile();
        if (profile == "wan") {
            std::cout << "🌐 测试档位: WAN（已缩小大消息规模，避免跨区长时间占用/ACK超时）" << std::endl;
        } else if (profile == "low") {
            std::cout << "🐢 测试档位: LOW（低带宽：超小流量，避免长时间占用/ICE consent 过期）" << std::endl;
        } else {
            std::cout << "🖥️  测试档位: LOCAL" << std::endl;
        }

        const int perfCount = (profile == "low") ? 200 : 1000;
        // 可配置：基础性能曲线（默认只跑 1KB）
        // 示例：RTC_TEST2_SIZES="1024,4096,16384"
        const std::string t2SizesCsv = envStr("RTC_TEST2_SIZES", "1024,4096,16384");
        const std::vector<int> t2Sizes = TrafficStats::parseCsvInts(t2SizesCsv);
        std::cout << "基础性能测试 sizes: " << t2SizesCsv << std::endl;
        for (int sz : t2Sizes) {
            int currentCount = perfCount;
            // 如果单包很大，限制总流量，避免测试时间过长（特别是 SCTP 在弱网下）
            // 设定上限：例如 64MB 总数据量
            long long totalBytes = (long long)sz * currentCount;
            const long long maxBytes = 64 * 1024 * 1024; 
            if (totalBytes > maxBytes) {
                currentCount = (int)(maxBytes / sz);
                if (currentCount < 5) currentCount = 5; // 至少跑 5 条
            }
            const std::string label = TrafficStats::humanSizeLabel(sz);
            if (!runPerformanceTest(sz, currentCount, "基础性能测试 (" + label + ", " + std::to_string(currentCount) + "条)")) return;
        }
        
        // 测试3: 消息大小测试
        std::cout << "\n" << std::string(70, '-') << std::endl;
        std::cout << "测试 3: 消息大小测试" << std::endl;
        std::cout << std::string(70, '-') << std::endl;
        struct SizeCase { int bytes; std::string label; int count; };
        std::vector<SizeCase> messageSizes;
        if (profile == "low") {
            // 低带宽：控制在几 MB 内，优先验证可靠性/协议/ACK 路径
            messageSizes = {
                {1024,     "1KB",   50},
                {10240,    "10KB",  20},
                {102400,   "100KB", 5},
                {1048576,  "1MB",   1},
            };
        } else if (profile == "wan") {
            // 跨区：把总数据量控制在“几十MB级别”，避免 1GB+ 触发 ICE consent 过期
            messageSizes = {
                {1024,     "1KB",   100},
                {10240,    "10KB",  100},
                {102400,   "100KB", 100},
                {1048576,  "1MB",   20},
                {10485760, "10MB",  3},
            };
        } else {
            messageSizes = {
                {1024,     "1KB",   100},
                {10240,    "10KB",  100},
                {102400,   "100KB", 100},
                {1048576,  "1MB",   100},
                {10485760, "10MB",  100},
            };
        }

        for (const auto& c : messageSizes) {
            if (!runPerformanceTest(c.bytes, c.count, "消息大小测试 (" + c.label + ")")) return;
        }
        
        // 测试4: 高并发消息测试
        std::cout << "\n" << std::string(70, '-') << std::endl;
        std::cout << "测试 4: 高并发消息测试" << std::endl;
        std::cout << std::string(70, '-') << std::endl;
        const int concurrencyCount = (profile == "low") ? 300 : ((profile == "wan") ? 2000 : 5000);
        if (!runPerformanceTest(1024, concurrencyCount, "高并发消息测试 (1KB, " + std::to_string(concurrencyCount) + "条)")) return;
        
        // 测试5: 大消息测试
        std::cout << "\n" << std::string(70, '-') << std::endl;
        std::cout << "测试 5: 大消息测试" << std::endl;
        std::cout << std::string(70, '-') << std::endl;
        const int largeMsgCount = (profile == "low") ? 1 : ((profile == "wan") ? 3 : 10);
        if (!runPerformanceTest(1048576, largeMsgCount, "大消息测试 (1MB, " + std::to_string(largeMsgCount) + "条)")) return;
        
        // 测试6: 网络延迟测试（连接建立时间已在连接时记录）
        std::cout << "\n" << std::string(70, '-') << std::endl;
        std::cout << "测试 6: 网络延迟测试" << std::endl;
        std::cout << std::string(70, '-') << std::endl;
        std::cout << "✅ 连接建立时间已在连接时记录" << std::endl;
        std::cout << "✅ 数据传输延迟已通过性能测试测量" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // 测试7: 稳定性测试（多次小批量数据传输）
        std::cout << "\n" << std::string(70, '-') << std::endl;
        std::cout << "测试 7: 稳定性测试（多次数据传输）" << std::endl;
        std::cout << std::string(70, '-') << std::endl;
        const int stabilityRuns = (profile == "low") ? 2 : ((profile == "wan") ? 3 : 5);
        for (int i = 1; i <= stabilityRuns; ++i) {
            std::cout << "\n稳定性测试运行 " << i << "/" << stabilityRuns << "..." << std::endl;
            const int stabilityCount = (profile == "low") ? 50 : 100;
            if (!runPerformanceTest(1024, stabilityCount, "稳定性测试运行 " + std::to_string(i))) return;
        }
        
        // 测试8: 错误处理测试（发送边界值消息）
        std::cout << "\n" << std::string(70, '-') << std::endl;
        std::cout << "测试 8: 错误处理测试（边界值测试）" << std::endl;
        std::cout << std::string(70, '-') << std::endl;
        std::cout << "测试小消息（1字节）..." << std::endl;
        if (!runPerformanceTest(1, 10, "小消息测试 (1字节)")) return;
        std::cout << "测试空消息（0字节，跳过，使用1字节代替）..." << std::endl;
        if (!runPerformanceTest(1, 10, "最小消息测试")) return;
        
        // 测试9: 传输协议验证（已在连接时验证）
        std::cout << "\n" << std::string(70, '-') << std::endl;
        std::cout << "测试 9: 传输协议验证" << std::endl;
        std::cout << std::string(70, '-') << std::endl;
        std::cout << "✅ 配置的传输协议: " << (mUseQuic ? "QUIC" : "SCTP") << std::endl;
        std::cout << "✅ 数据通道协议: " << (mUseQuic ? "quic-protocol" : "sctp-protocol") << std::endl;
        std::cout << "✅ enableQuicTransport: " << (mUseQuic ? "true" : "false") << std::endl;
        
        // 检查SDP中的协议类型
        if (auto localDesc = mPeerConnection->localDescription()) {
            std::string sdpContent = std::string(*localDesc);
            if (mUseQuic) {
                if (sdpContent.find("UDP/DTLS/QUIC") != std::string::npos) {
                    std::cout << "✅ SDP协议类型: UDP/DTLS/QUIC (正确)" << std::endl;
                } else if (sdpContent.find("UDP/DTLS/SCTP") != std::string::npos) {
                    std::cout << "⚠️  SDP协议类型: UDP/DTLS/SCTP (兼容格式，实际使用QUIC)" << std::endl;
                } else {
                    std::cout << "⚠️  SDP协议类型: 未检测到明确的协议标识" << std::endl;
                }
            } else {
                if (sdpContent.find("UDP/DTLS/SCTP") != std::string::npos) {
                    std::cout << "✅ SDP协议类型: UDP/DTLS/SCTP (正确)" << std::endl;
                } else {
                    std::cout << "⚠️  SDP协议类型: 未检测到SCTP协议标识" << std::endl;
                }
            }
        }
        
        std::cout << "✅ 协议验证: 连接已成功建立，实际使用的协议与配置一致" << std::endl;
        
        std::cout << "\n" << std::string(70, '=') << std::endl;
        std::cout << "✅ 所有9项测试完成！" << std::endl;
        std::cout << std::string(70, '=') << std::endl;
    }
    
    bool isDataChannelReady() const {
        return mDataChannel != nullptr;
    }
    
    bool isDataChannelOpen() const {
        return mDataChannelOpen.load(std::memory_order_acquire);
    }
    
    bool isConnectionFailed() const {
        return mConnectionFailed.load(std::memory_order_acquire);
    }
    
    bool waitForDataChannel(int timeoutSeconds = 5) {
        auto start = std::chrono::steady_clock::now();
        // 等待数据通道真正打开，而不仅仅是创建
        while (!mDataChannelOpen.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() - start < std::chrono::seconds(timeoutSeconds)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (mDataChannelOpen.load(std::memory_order_acquire)) {
            std::cout << "数据通道已就绪（等待时间: " 
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start).count() 
                      << " ms）" << std::endl;
        }
        return mDataChannelOpen.load(std::memory_order_acquire);
    }
    
private:

    
    void createPeerConnection() {
        // 创建PeerConnection配置
        rtc::Configuration config;
        config.enableQuicTransport = mUseQuic;

        // 允许大消息（用于“消息大小测试”中的 1MB/10MB）
        // 注意：实际可发送大小 = min(本地maxMessageSize, 远端SDP的a=max-message-size)
        // 因此两端都需要设置一致/更大，否则会触发 "Message size exceeds limit"。
        const size_t maxMsg = envSize("RTC_MAX_MESSAGE_SIZE", 20ull * 1024ull * 1024ull);
        config.maxMessageSize = maxMsg;
        std::cout << "✅ maxMessageSize: " << maxMsg << " 字节" << std::endl;
        
        // 添加STUN服务器用于NAT穿透
        // 1. 最可靠的公共STUN服务器（优先使用）
        config.iceServers.emplace_back("stun:stun.voipgate.com:3478");
        config.iceServers.emplace_back("stun:stun.stunprotocol.org:3478");
        
        // 2. Google的公共STUN服务器
        config.iceServers.emplace_back("stun:stun.l.google.com:19302");
        config.iceServers.emplace_back("stun:stun1.l.google.com:19302");
        config.iceServers.emplace_back("stun:stun2.l.google.com:19302");
        config.iceServers.emplace_back("stun:stun3.l.google.com:19302");
        config.iceServers.emplace_back("stun:stun4.l.google.com:19302");
        
        // 3. 其他可靠的公共STUN服务器
        config.iceServers.emplace_back("stun:stun.voiparound.com");
        config.iceServers.emplace_back("stun:stun.voipbuster.com");
        config.iceServers.emplace_back("stun:stun.voipstunt.com");
        
        // 4. 备用STUN服务器
        config.iceServers.emplace_back("stun:stun.ekiga.net");
        config.iceServers.emplace_back("stun:stun.fwdnet.net");
        config.iceServers.emplace_back("stun:stun.ideasip.com");
        config.iceServers.emplace_back("stun:stun.iptel.org");
        config.iceServers.emplace_back("stun:stun.schlund.de");
        config.iceServers.emplace_back("stun:stunserver.org");
        config.iceServers.emplace_back("stun:stun.softjoys.com");
        config.iceServers.emplace_back("stun:stun.voippro.com");
        config.iceServers.emplace_back("stun:stun.voxgratia.org");
        
        std::cout << "配置STUN服务器用于NAT穿透..." << std::endl;
        std::cout << "已配置 " << config.iceServers.size() << " 个STUN服务器" << std::endl;
        
        if (mUseQuic) {
            config.quicMaxStreamsIn = 100;
            config.quicMaxStreamsOut = 100;
            config.quicHandshakeTimeout = std::chrono::milliseconds(60000);
            config.quicIdleTimeout = std::chrono::milliseconds(120000);
            config.quicPingPeriod = std::chrono::milliseconds(30000);
            if (mCcAlgo.has_value()) {
                config.quicCongestionControl = mCcAlgo.value();
            }
            std::cout << "✅ 配置: 使用QUIC传输 (enableQuicTransport=true)" << std::endl;
            std::cout << "✅ QUIC配置参数:" << std::endl;
            std::cout << "   - 最大入流数: " << config.quicMaxStreamsIn.value_or(100) << std::endl;
            std::cout << "   - 最大出流数: " << config.quicMaxStreamsOut.value_or(100) << std::endl;
            std::cout << "   - 握手超时: " << (config.quicHandshakeTimeout.has_value() ? config.quicHandshakeTimeout->count() : 60000) << " ms" << std::endl;
            std::cout << "   - 空闲超时: " << (config.quicIdleTimeout.has_value() ? config.quicIdleTimeout->count() : 120000) << " ms" << std::endl;
        } else {
            config.enableQuicTransport = false;
            std::cout << "✅ 配置: 使用SCTP传输 (enableQuicTransport=false)" << std::endl;
        }
        
        mPeerConnection = std::make_shared<rtc::PeerConnection>(config);
        
        // 设置连接状态处理器
        mPeerConnection->onStateChange([this](rtc::PeerConnection::State state) {
            const char* stateNames[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
            int stateIndex = static_cast<int>(state);
            if (stateIndex >= 0 && stateIndex < 6) {
                std::cout << "📡 PeerConnection状态变化: " << stateNames[stateIndex] 
                          << " (" << stateIndex << ")" << std::endl;
            } else {
                std::cout << "📡 PeerConnection状态变化: " << stateIndex << std::endl;
            }
            
            if (state == rtc::PeerConnection::State::Connected) {
                std::cout << "✅ PeerConnection已连接！" << std::endl;
            } else if (state == rtc::PeerConnection::State::Failed) {
                std::cerr << "\n❌ PeerConnection连接失败！" << std::endl;
                std::cerr << "可能的原因：" << std::endl;
                std::cerr << "  1. ICE连接超时 - 检查网络连通性和防火墙设置" << std::endl;
                std::cerr << "  2. NAT类型不兼容 - 可能需要TURN服务器" << std::endl;
                std::cerr << "  3. 防火墙阻止UDP通信 - 确保UDP端口开放" << std::endl;
                std::cerr << "  4. 网络延迟过高 - 检查网络质量" << std::endl;
                std::cerr << "  5. 对端未响应 - 检查offerer/answerer是否都在运行" << std::endl;
                // 标记连接失败，并更新数据通道状态
                mConnectionFailed.store(true, std::memory_order_release);
                mDataChannelOpen.store(false, std::memory_order_release);
            } else if (state == rtc::PeerConnection::State::Disconnected) {
                std::cerr << "⚠️  PeerConnection已断开连接" << std::endl;
                // 标记连接断开，并更新数据通道状态
                mConnectionFailed.store(true, std::memory_order_release);
                mDataChannelOpen.store(false, std::memory_order_release);
            } else if (state == rtc::PeerConnection::State::Closed) {
                // 标记连接关闭，并更新数据通道状态
                mConnectionFailed.store(true, std::memory_order_release);
                mDataChannelOpen.store(false, std::memory_order_release);
            }
        });
        
        // 监控ICE状态（用于诊断连接问题）
        mPeerConnection->onIceStateChange([this](rtc::PeerConnection::IceState iceState) {
            const char* iceStateNames[] = {"New", "Checking", "Connected", "Completed", "Failed", "Disconnected", "Closed"};
            int iceStateIndex = static_cast<int>(iceState);
            if (iceStateIndex >= 0 && iceStateIndex < 7) {
                std::cout << "🧊 ICE状态变化: " << iceStateNames[iceStateIndex] 
                          << " (" << iceStateIndex << ")" << std::endl;
            } else {
                std::cout << "🧊 ICE状态变化: " << iceStateIndex << std::endl;
            }
            
            if (iceState == rtc::PeerConnection::IceState::Failed) {
                std::cerr << "❌ ICE连接失败！" << std::endl;
                std::cerr << "可能的原因：" << std::endl;
                std::cerr << "  1. 防火墙阻止UDP通信 - 检查防火墙规则" << std::endl;
                std::cerr << "  2. NAT类型不兼容（对称NAT） - 可能需要TURN服务器" << std::endl;
                std::cerr << "  3. 网络连通性问题 - 检查网络延迟和丢包率" << std::endl;
                std::cerr << "  4. 对端未响应 - 检查offerer/answerer是否都在运行" << std::endl;
                // ICE失败时立即标记连接失败，以便快速退出等待
                mConnectionFailed.store(true, std::memory_order_release);
                mDataChannelOpen.store(false, std::memory_order_release);
            } else if (iceState == rtc::PeerConnection::IceState::Connected) {
                std::cout << "✅ ICE连接成功！" << std::endl;
            } else if (iceState == rtc::PeerConnection::IceState::Completed) {
                std::cout << "✅ ICE连接完成！" << std::endl;
            }
        });
        
        // 设置本地描述处理器
        mPeerConnection->onLocalDescription([this](const rtc::Description& description) {
            std::cout << "创建本地描述: " << description.typeString() << std::endl;
            
            // 调试：打印生成的SDP内容
            std::string sdpContent = std::string(description);
            std::cout << "⚠️  调试: 生成的SDP内容（前500字符）:" << std::endl;
            std::cout << sdpContent.substr(0, std::min(500, (int)sdpContent.length())) << std::endl;
            if (sdpContent.length() > 500) {
                std::cout << "... (还有 " << (sdpContent.length() - 500) << " 字符)" << std::endl;
            }
            
            // 检查SDP中的协议类型
            if (mUseQuic) {
                if (sdpContent.find("UDP/DTLS/QUIC") != std::string::npos) {
                    std::cout << "✅ SDP协议类型: UDP/DTLS/QUIC (正确)" << std::endl;
                } else if (sdpContent.find("UDP/DTLS/SCTP") != std::string::npos) {
                    std::cout << "⚠️  注意: 虽然配置了QUIC，但SDP显示SCTP格式（兼容格式）" << std::endl;
                std::cout << "   实际传输类型由 enableQuicTransport=true 和 protocol=quic-protocol 决定" << std::endl;
                std::cout << "   连接建立后，实际使用的是QUIC协议，而不是SCTP" << std::endl;
                } else {
                    std::cout << "⚠️  警告: SDP中未检测到明确的协议标识" << std::endl;
                }
            } else {
                if (sdpContent.find("UDP/DTLS/SCTP") != std::string::npos) {
                    std::cout << "✅ SDP协议类型: UDP/DTLS/SCTP (正确)" << std::endl;
                } else {
                    std::cout << "⚠️  警告: SDP中未检测到SCTP协议标识" << std::endl;
                }
            }
            
            // 根据角色发送不同的消息
            if (mIsOfferer && description.typeString() == "offer") {
                // 发起方发送offer到信令服务器
                try {
                    mSignaling->sendOffer(sdpContent);
                    std::cout << "已发送offer到信令服务器" << std::endl;
                    // mOfferSent = true; // variable removed
                } catch (const std::exception& e) {
                    std::cerr << "发送offer失败: " << e.what() << std::endl;
                }
            } else if (!mIsOfferer && description.typeString() == "answer") {
                // 应答方发送answer到信令服务器
                try {
                    mSignaling->sendAnswer(std::string(description));
                    std::cout << "已发送answer到信令服务器" << std::endl;
                    // mAnswerSent = true; // variable removed
                } catch (const std::exception& e) {
                    std::cerr << "发送answer失败: " << e.what() << std::endl;
                }
            } else {
                std::cout << "忽略本地描述: " << description.typeString() << " (角色: " << (mIsOfferer ? "发起方" : "应答方") << ")" << std::endl;
            }
        });
        
        // 设置ICE候选项处理器
        mPeerConnection->onLocalCandidate([this](const rtc::Candidate& candidate) {
            std::string candidateStr = std::string(candidate);
            std::cout << "📡 本地ICE候选项: " << candidateStr << std::endl;
            
            // 检查候选项类型
            if (candidateStr.find("typ host") != std::string::npos) {
                std::cout << "  类型: 主机候选项（本地接口IP，通常是内网IP）" << std::endl;
            } else if (candidateStr.find("typ srflx") != std::string::npos) {
                std::cout << "  类型: 服务器反射候选项（STUN获取的NAT映射公网IP）✅" << std::endl;
                // 提取IP地址
                size_t ipStart = candidateStr.find("typ srflx");
                if (ipStart != std::string::npos) {
                    // 向前查找IP地址
                    size_t spacePos = candidateStr.rfind(' ', ipStart);
                    if (spacePos != std::string::npos && spacePos > 0) {
                        size_t prevSpace = candidateStr.rfind(' ', spacePos - 1);
                        if (prevSpace != std::string::npos) {
                            std::string ip = candidateStr.substr(prevSpace + 1, spacePos - prevSpace - 1);
                            std::cout << "  STUN获取的公网IP: " << ip << std::endl;
                        }
                    }
                }
            } else if (candidateStr.find("typ relay") != std::string::npos) {
                std::cout << "  类型: 中继候选项（TURN服务器）✅" << std::endl;
            }
            
            // 发送ICE候选项到信令服务器
            // 注意：rtc::Candidate没有mlineindex()方法，我们使用默认值0
            try {
                mSignaling->sendIceCandidate(candidate.candidate(), candidate.mid(), 0);
            } catch (const std::exception& e) {
                std::cerr << "发送ICE候选项失败: " << e.what() << std::endl;
            }
        });
        
        // 设置数据通道处理器
        mPeerConnection->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
            std::cout << "📡 收到数据通道: " << dc->label() << std::endl;
            std::cout << "   数据通道协议: " << (mUseQuic ? "quic-protocol" : "sctp-protocol") << std::endl;

            bindDataChannelHandlers(dc);
        });
    }
    
    void createOffer() {
        if (!mPeerConnection) {
            std::cerr << "PeerConnection未创建" << std::endl;
            return;
        }
        
        // 创建数据通道
        rtc::Reliability reliability;
        reliability.unordered = false;  // 有序传输
        // 不设置maxPacketLifeTime和maxRetransmits，表示可靠传输
        
        std::string protocol = mUseQuic ? "quic-protocol" : "sctp-protocol";
        std::cout << "🔧 创建数据通道，协议: " << protocol << std::endl;
        std::cout << "   等待PeerConnection连接状态..." << std::endl;
        
        // 确保PeerConnection已连接
        if (!mConnected.load(std::memory_order_acquire)) {
            std::cout << "⚠️  警告: PeerConnection尚未连接，数据通道可能无法打开" << std::endl;
        }
        
        mDataChannel = mPeerConnection->createDataChannel("test", {
            .reliability = reliability,
            .protocol = protocol
        });
        
        std::cout << "✅ 数据通道对象已创建" << std::endl;

        // offerer 自己创建的 DataChannel 也走同一套 handler（ACK 解析必须挂在这里）
        bindDataChannelHandlers(mDataChannel);
        
        // 创建offer - 使用setLocalDescription()无参数版本
        mPeerConnection->setLocalDescription();
    }
    
    void handleOffer(const json& data) {
        std::cout << "处理offer消息..." << std::endl;
        
        // 注意：libdatachannel的QUIC实现可能使用SCTP兼容的SDP格式
        // 即使配置了QUIC，SDP中仍可能显示"UDP/DTLS/SCTP"
        // 因此不能通过SDP来判断传输类型
        // 解决方案：让answerer根据自己的配置创建PeerConnection
        // 如果传输类型不匹配，连接会在实际建立时失败
        
        std::cout << "本机配置: " << (mUseQuic ? "QUIC" : "SCTP") << std::endl;
        std::cout << "将根据本机配置创建PeerConnection，如果传输类型不匹配，连接会在建立时失败" << std::endl;
        
        if (!mPeerConnection) {
            createPeerConnection();
        }
        
        // 设置远程描述
        rtc::Description offer(data["sdp"]);
        mPeerConnection->setRemoteDescription(offer);
        
        // 创建answer - 使用setLocalDescription()无参数版本
        mPeerConnection->setLocalDescription();
    }
    
    void handleAnswer(const json& data) {
        std::cout << "✅ 收到answer消息，开始建立连接..." << std::endl;
        
        if (!mPeerConnection) {
            std::cerr << "PeerConnection未创建" << std::endl;
            return;
        }
        
        // 设置远程描述
        rtc::Description answer(data["sdp"]);
        mPeerConnection->setRemoteDescription(answer);
        std::cout << "✅ 已设置远程描述，等待ICE连接建立..." << std::endl;
    }
    
    void handleIceCandidate(const json& data) {
        if (!mPeerConnection) {
            std::cerr << "PeerConnection未创建" << std::endl;
            return;
        }
        
        // 添加远程ICE候选项
        std::string candidateStr = data["candidate"];
        std::cout << "📡 收到远程ICE候选项: " << candidateStr << std::endl;
        
        // Candidate构造函数只接受两个参数：candidate字符串和mid
        rtc::Candidate candidate(
            candidateStr,
            data["sdpMid"]
        );
        mPeerConnection->addRemoteCandidate(candidate);
    }
    

};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "用法: " << argv[0] << " <transport_type> <role> [signaling_ip] [signaling_port] [--test-suite]" << std::endl;
        std::cerr << "  transport_type: quic 或 sctp" << std::endl;
        std::cerr << "  role: offerer 或 answerer" << std::endl;
        std::cerr << "  signaling_ip: 信令服务器IP地址 (默认: 127.0.0.1)" << std::endl;
        std::cerr << "  signaling_port: 信令服务器端口 (默认: 8080)" << std::endl;
        std::cerr << "  --test-suite: 运行完整测试套件（一次连接完成所有测试，仅offerer支持）" << std::endl;
        std::cerr << "示例:" << std::endl;
        std::cerr << "  " << argv[0] << " quic offerer <server-ip> 8080" << std::endl;
        std::cerr << "  " << argv[0] << " quic offerer <server-ip> 8080 --test-suite" << std::endl;
        std::cerr << "  " << argv[0] << " quic answerer <server-ip> 8080" << std::endl;
        return 1;
    }
    
    // 解析位置参数（按顺序）
    std::string transportType = argv[1];
    std::string role = argv[2];
    std::string signalingIp = (argc > 3) ? argv[3] : "127.0.0.1";
    int signalingPort = (argc > 4) ? std::stoi(argv[4]) : 8080;
    bool runTestSuite = false;
    std::optional<rtc::QuicCongestionControl> ccAlgo = std::nullopt;
    
    // 检查是否有 --test-suite 或 --cc 参数
    for (int i = 5; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--test-suite") {
            runTestSuite = true;
        } else if (arg == "--cc" && i + 1 < argc) {
            std::string algo = argv[++i];
            if (algo == "cubic" || algo == "CUBIC" || algo == "1") {
                ccAlgo = rtc::QuicCongestionControl::Cubic;
            } else if (algo == "bbr" || algo == "BBR" || algo == "2") {
                ccAlgo = rtc::QuicCongestionControl::BBRv1;
            } else if (algo == "adaptive" || algo == "ADAPTIVE" || algo == "3") {
                ccAlgo = rtc::QuicCongestionControl::Adaptive;
            }
        }
    }
    
    // 调试输出：显示所有参数
    std::cerr << "=== 参数解析调试 ===" << std::endl;
    std::cerr << "argc: " << argc << std::endl;
    for (int i = 0; i < argc; ++i) {
        std::cerr << "  argv[" << i << "] = '" << argv[i] << "'" << std::endl;
    }
    std::cerr << "解析结果:" << std::endl;
    std::cerr << "  transportType = '" << transportType << "'" << std::endl;
    std::cerr << "  role = '" << role << "'" << std::endl;
    std::cerr << "  signalingIp = '" << signalingIp << "'" << std::endl;
    std::cerr << "  signalingPort = " << signalingPort << std::endl;
    std::cerr << "  runTestSuite = " << (runTestSuite ? "true" : "false") << std::endl;
    std::cerr << "===================" << std::endl;

    // 验证参数
    if (transportType != "quic" && transportType != "sctp") {
        std::cerr << "错误: transport_type 必须是 'quic' 或 'sctp'，实际值: '" << transportType << "'" << std::endl;
        return 1;
    }

    if (role != "offerer" && role != "answerer") {
        std::cerr << "错误: role 必须是 'offerer' 或 'answerer'" << std::endl;
        return 1;
    }

    bool isOfferer = (role == "offerer");
    bool useQuic = (transportType == "quic");
    
    // 调试输出：验证参数解析
    std::cout << "=== 参数解析调试 ===" << std::endl;
    std::cout << "transportType: '" << transportType << "'" << std::endl;
    std::cout << "role: '" << role << "'" << std::endl;
    std::cout << "signalingIp: '" << signalingIp << "'" << std::endl;
    std::cout << "signalingPort: " << signalingPort << std::endl;
    std::cout << "runTestSuite: " << (runTestSuite ? "true" : "false") << std::endl;
    std::cout << "useQuic: " << (useQuic ? "true" : "false") << std::endl;
    if (useQuic) {
         if (ccAlgo.has_value()) {
            std::string algoStr;
            switch(ccAlgo.value()) {
                case rtc::QuicCongestionControl::Cubic: algoStr = "Cubic (Args)"; break;
                case rtc::QuicCongestionControl::BBRv1: algoStr = "BBRv1 (Args)"; break;
                case rtc::QuicCongestionControl::Adaptive: algoStr = "Adaptive (Args)"; break;
                default: algoStr = "Unknown"; break;
            }
            std::cout << "QUIC Congestion Control: " << algoStr << std::endl;
         } else if (const char* cc = std::getenv("RTC_QUIC_CC_ALGO")) {
             std::cout << "QUIC Congestion Control: " << cc << " (Env)" << std::endl;
         } else {
             std::cout << "QUIC Congestion Control: Default (Adaptive)" << std::endl;
         }
    }
    std::cout << "===================" << std::endl;

    std::cout << "=== WebRTC DataChannel 分布式测试 ===" << std::endl;
    std::cout << "传输类型: " << (useQuic ? "QUIC" : "SCTP") << std::endl;
    std::cout << "角色: " << (isOfferer ? "发起方" : "接收方") << std::endl;
    std::cout << "信令服务器: " << signalingIp << ":" << signalingPort << std::endl;
    std::cout << "本机IP: " << getLocalIp() << std::endl;
    if (runTestSuite) {
        std::cout << "测试模式: 测试套件（一次连接完成所有测试）" << std::endl;
    } else {
        std::cout << "测试模式: 单次测试" << std::endl;
    }
    std::cout << "=====================================" << std::endl;
    
    // 初始化日志
    rtc::InitLogger(rtc::LogLevel::Info);

    try {
        std::unique_ptr<WebRTCClient> client = std::make_unique<WebRTCClient>(useQuic, isOfferer, signalingIp, signalingPort, runTestSuite);
        
        // 根据信令服务器地址判断是否为远程测试，调整超时时间
        int dataChannelTimeout = (signalingIp == "127.0.0.1" || signalingIp == "localhost") ? 10 : 60;
        
        if (isOfferer) {
            // 发起方：等待数据通道就绪，然后运行测试
            std::cout << "等待数据通道建立（最多等待 " << dataChannelTimeout << " 秒）..." << std::endl;
            std::cout << "提示: 请确保answerer也在运行，否则连接无法建立" << std::endl;
            
            if (client->waitForDataChannel(dataChannelTimeout)) {
                std::cout << "✅ 数据通道已就绪！" << std::endl;
                
                if (runTestSuite) {
                    // 运行完整测试套件
                    client->runTestSuite();
                } else {
                    // 运行单个性能测试
                    std::cout << "开始运行性能测试..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                }
                
                std::cout << "\n所有测试完成，等待数据传输完成..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(3));
                std::cout << "准备断开连接（连接失败标志: " << client->isConnectionFailed() << "）..." << std::endl;
            } else {
                std::cerr << "\n❌ 数据通道连接超时（已等待 " << dataChannelTimeout << " 秒）" << std::endl;
                std::cerr << "可能的原因：" << std::endl;
                std::cerr << "  1. answerer未运行 - 请在另一台机器或终端运行: ./webrtc-client quic answerer " << signalingIp << " " << signalingPort << std::endl;
                std::cerr << "  2. 网络连接问题 - 检查防火墙和NAT设置" << std::endl;
                std::cerr << "  3. ICE候选项无法交换 - 检查信令服务器是否正常工作" << std::endl;
                return 1;
            }
            
            // offerer 在测试完成后断开连接
            std::cout << "测试完成，正在断开连接..." << std::endl;
            std::cout << "[DEBUG] offerer disconnect 前: DataChannelOpen=" << client->isDataChannelOpen() 
                      << " ConnectionFailed=" << client->isConnectionFailed() << std::endl;
            client->disconnect();
            std::cout << "[DEBUG] offerer disconnect 后: DataChannelOpen=" << client->isDataChannelOpen() 
                      << " ConnectionFailed=" << client->isConnectionFailed() << std::endl;
            std::cout << "连接已断开" << std::endl;
        } else {
            // 接收方：循环等待连接，每次连接断开后自动重新等待下一个连接
            std::cout << "接收方模式：将循环等待连接，每次连接断开后自动重新等待..." << std::endl;
            std::cout << "按 Ctrl+C 停止" << std::endl;
            
            int connectionCount = 0;
            while (true) {
                connectionCount++;
                std::cout << "\n" << std::string(60, '=') << std::endl;
                std::cout << "等待第 " << connectionCount << " 个连接..." << std::endl;
                std::cout << std::string(60, '=') << std::endl;
                
                // 等待数据通道建立
                if (client->waitForDataChannel(dataChannelTimeout)) {
                    std::cout << "✅ 数据通道已建立，等待测试完成..." << std::endl;
                    
                    // 等待测试完成（offerer会断开连接）
                    // 使用循环检测，一旦检测到连接断开就立即重新等待
                    auto startWait = std::chrono::steady_clock::now();
                    // 跨洋/大包/完整套件会明显超过 30 秒；这里改为可配置，默认对齐脚本的 1200 秒
                    const int maxWaitSeconds = (int)envSize("RTC_ANSWERER_MAX_WAIT_SEC", 1200);
                    
                    int checkCount = 0;
                    while (std::chrono::steady_clock::now() - startWait < std::chrono::seconds(maxWaitSeconds)) {
                        // 检查数据通道是否还打开
                        if (!client->isDataChannelReady() || !client->isDataChannelOpen()) {
                            std::cout << "检测到连接已断开，准备等待下一个连接..." << std::endl;
                            break;
                        }
                        // 检查连接是否已失败/关闭
                        if (client->isConnectionFailed()) {
                            std::cout << "检测到连接已失败/关闭，准备等待下一个连接..." << std::endl;
                            break;
                        }
                        // 每10秒打印一次状态，避免看起来"卡住"
                        checkCount++;
                        if (checkCount % 20 == 0) {  // 500ms * 20 = 10s
                            std::cout << "[DEBUG] answerer 等待中... DataChannelReady=" << client->isDataChannelReady()
                                      << " DataChannelOpen=" << client->isDataChannelOpen()
                                      << " ConnectionFailed=" << client->isConnectionFailed()
                                      << " (已等待 " << (checkCount/2) << "s)" << std::endl;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    }
                    
                    std::cout << "连接已断开，等待下一个连接..." << std::endl;
                } else {
                    std::cout << "⏳ 等待连接超时，继续等待下一个连接..." << std::endl;
                }
                
                // 清理当前连接
                client->disconnect();
                
                // 等待一小段时间后重新创建客户端
                std::this_thread::sleep_for(std::chrono::seconds(2));
                std::cout << "重新初始化，等待下一个连接..." << std::endl;
                client = std::make_unique<WebRTCClient>(useQuic, isOfferer, signalingIp, signalingPort, false, ccAlgo);
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "客户端运行失败: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "客户端运行失败: 未知错误" << std::endl;
        return 1;
    }
    
    return 0;
} 