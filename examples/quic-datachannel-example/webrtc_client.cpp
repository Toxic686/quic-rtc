#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <memory>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using json = nlohmann::json;

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
    std::shared_ptr<rtc::WebSocket> mWebSocket;
    std::shared_ptr<rtc::PeerConnection> mPeerConnection;
    std::shared_ptr<rtc::DataChannel> mDataChannel;
    std::string mClientId;
    bool mIsOfferer;
    bool mUseQuic;
    bool mConnected;
    bool mOfferSent;
    bool mAnswerSent;
    bool mPerformanceTestRun;  // 新增：防止重复运行性能测试
    
public:
    WebRTCClient(bool useQuic = true, bool isOfferer = true, const std::string& signalingIp = "127.0.0.1", int signalingPort = 8080) 
        : mIsOfferer(isOfferer), mUseQuic(useQuic), mConnected(false), mOfferSent(false), mAnswerSent(false), mPerformanceTestRun(false) {
        
        // 创建WebSocket客户端
        mWebSocket = std::make_shared<rtc::WebSocket>();
        
        // 设置WebSocket事件处理器
        mWebSocket->onOpen([this]() {
            std::cout << "WebSocket连接已建立" << std::endl;
            // 连接建立后等待服务器发送连接确认
        });
        
        mWebSocket->onMessage([this](rtc::message_variant msg) {
            if (std::holds_alternative<std::string>(msg)) {
                handleWebSocketMessage(std::get<std::string>(msg));
            }
        });
        
        mWebSocket->onClosed([]() {
            std::cout << "WebSocket连接已关闭" << std::endl;
        });
        
        mWebSocket->onError([](std::string error) {
            std::cerr << "WebSocket错误: " << error << std::endl;
        });
    
        // 自动连接到信令服务器
        std::string uri = "ws://" + signalingIp + ":" + std::to_string(signalingPort);
        std::cout << "连接到信令服务器: " << uri << std::endl;
        mWebSocket->open(uri);
        
        // 等待连接确认，本地测试使用较短时间，远程测试使用较长时间
        int waitTime = (signalingIp == "127.0.0.1" || signalingIp == "localhost") ? 3 : 10;
        std::cout << "等待连接确认，超时时间: " << waitTime << " 秒..." << std::endl;
        
        auto start = std::chrono::steady_clock::now();
        while (!mConnected && std::chrono::steady_clock::now() - start < std::chrono::seconds(waitTime)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (!mConnected) {
            std::cout << "警告: 未收到连接确认消息，但继续尝试..." << std::endl;
        } else {
            std::cout << "已收到连接确认消息" << std::endl;
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
        if (mWebSocket) {
            try {
            mWebSocket->close();
            } catch (...) {
                // 忽略关闭时的异常
            }
            mWebSocket.reset();
        }
    }
    
    void runPerformanceTest() {
        if (!mDataChannel) {
            std::cerr << "数据通道未创建" << std::endl;
            return;
        }
        
        const int numMessages = 1000;
        const int messageSize = 1024; // 1KB
        
        std::vector<std::byte> testData(messageSize, std::byte{0xAA});
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < numMessages; ++i) {
            mDataChannel->send(testData);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << "\n=== 性能测试结果 ===" << std::endl;
        std::cout << "传输方式: " << (mUseQuic ? "QUIC" : "SCTP") << std::endl;
        std::cout << "发送消息数: " << numMessages << std::endl;
        std::cout << "消息大小: " << messageSize << " 字节" << std::endl;
        std::cout << "总时间: " << duration.count() << " 毫秒" << std::endl;
        std::cout << "平均每条消息: " << duration.count() / (double)numMessages << " 毫秒" << std::endl;
        std::cout << "吞吐量: " << (numMessages * messageSize * 8.0) / (duration.count() / 1000.0) / 1000000.0 << " Mbps" << std::endl;
    }
    
    bool isDataChannelReady() const {
        return mDataChannel != nullptr;
    }
    
    bool waitForDataChannel(int timeoutSeconds = 5) {
        auto start = std::chrono::steady_clock::now();
        while (!mDataChannel && std::chrono::steady_clock::now() - start < std::chrono::seconds(timeoutSeconds)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return mDataChannel != nullptr;
    }
    
private:
    void handleWebSocketMessage(const std::string& message) {
        try {
            json data = json::parse(message);
            std::string type = data["type"];
            
            std::cout << "收到信令消息: " << type << std::endl;
            
            if (type == "connected") {
                mClientId = data["clientId"];
                mConnected = true;
                std::cout << "客户端ID: " << mClientId << std::endl;
                
                // 连接确认后，如果是发起方则开始创建连接
                if (mIsOfferer) {
                    std::cout << "开始创建PeerConnection..." << std::endl;
                    createPeerConnection();
                    createOffer();
                }
            } else if (type == "offer") {
                handleOffer(data);
            } else if (type == "answer") {
                handleAnswer(data);
            } else if (type == "ice-candidate") {
                handleIceCandidate(data);
            } else if (type == "pong") {
                // 处理pong消息，保持连接活跃
                std::cout << "收到pong消息" << std::endl;
            } else {
                std::cout << "未知消息类型: " << type << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "解析消息失败: " << e.what() << std::endl;
        }
    }
    
    void createPeerConnection() {
        // 创建PeerConnection配置
        rtc::Configuration config;
        config.enableQuicTransport = mUseQuic;
        
        if (mUseQuic) {
            config.quicMaxStreamsIn = 100;
            config.quicMaxStreamsOut = 100;
            config.quicHandshakeTimeout = std::chrono::milliseconds(60000);
            config.quicIdleTimeout = std::chrono::milliseconds(120000);
            config.quicPingPeriod = std::chrono::milliseconds(30000);
            std::cout << "使用QUIC传输" << std::endl;
        } else {
            std::cout << "使用SCTP传输" << std::endl;
        }
        
        mPeerConnection = std::make_shared<rtc::PeerConnection>(config);
        
        // 设置连接状态处理器
        mPeerConnection->onStateChange([](rtc::PeerConnection::State state) {
            std::cout << "PeerConnection状态变化: " << static_cast<int>(state) << std::endl;
        });
        
        // 设置本地描述处理器
        mPeerConnection->onLocalDescription([this](const rtc::Description& description) {
            std::cout << "创建本地描述: " << description.typeString() << std::endl;
            
            // 根据角色发送不同的消息
            if (mIsOfferer && description.typeString() == "offer") {
                // 发起方发送offer到信令服务器
                json offerMsg = {
                    {"type", "offer"},
                    {"sdp", description}
                };
                
                try {
                    mWebSocket->send(offerMsg.dump());
                    std::cout << "已发送offer到信令服务器" << std::endl;
                    mOfferSent = true;
                } catch (const std::exception& e) {
                    std::cerr << "发送offer失败: " << e.what() << std::endl;
                }
            } else if (!mIsOfferer && description.typeString() == "answer") {
                // 应答方发送answer到信令服务器
                json answerMsg = {
                    {"type", "answer"},
                    {"sdp", description}
                };
                
                try {
                    mWebSocket->send(answerMsg.dump());
                    std::cout << "已发送answer到信令服务器" << std::endl;
                    mAnswerSent = true;
                } catch (const std::exception& e) {
                    std::cerr << "发送answer失败: " << e.what() << std::endl;
                }
            } else {
                std::cout << "忽略本地描述: " << description.typeString() << " (角色: " << (mIsOfferer ? "发起方" : "应答方") << ")" << std::endl;
            }
        });
        
        // 设置ICE候选项处理器
        mPeerConnection->onLocalCandidate([this](const rtc::Candidate& candidate) {
            std::cout << "本地ICE候选项: " << candidate << std::endl;
            
            // 发送ICE候选项到信令服务器
            // 注意：rtc::Candidate没有mlineindex()方法，我们使用默认值0
            json iceMsg = {
                {"type", "ice-candidate"},
                {"candidate", candidate.candidate()},
                {"sdpMid", candidate.mid()},
                {"sdpMLineIndex", 0}  // 使用默认值，因为rtc::Candidate没有mlineindex()方法
            };
            
            try {
            mWebSocket->send(iceMsg.dump());
            } catch (const std::exception& e) {
                std::cerr << "发送ICE候选项失败: " << e.what() << std::endl;
            }
        });
        
        // 设置数据通道处理器
        mPeerConnection->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
            std::cout << "收到数据通道: " << dc->label() << std::endl;
            
            dc->onOpen([this]() {
                std::cout << "数据通道已打开" << std::endl;
                if (!mPerformanceTestRun) {
                    std::cout << "数据通道已打开，开始运行性能测试..." << std::endl;
                    runPerformanceTest();
                    mPerformanceTestRun = true; // 设置标志，防止重复运行
                }
            });
            
            dc->onMessage([](rtc::message_variant msg) {
                if (std::holds_alternative<std::string>(msg)) {
                    std::cout << "收到字符串消息: " << std::get<std::string>(msg) << std::endl;
                } else {
                    auto& binary = std::get<std::vector<std::byte>>(msg);
                    std::cout << "收到二进制消息，大小: " << binary.size() << " 字节" << std::endl;
                }
            });
            
            dc->onClosed([]() {
                std::cout << "数据通道已关闭" << std::endl;
            });
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
        
        mDataChannel = mPeerConnection->createDataChannel("test", {
            .reliability = reliability,
            .protocol = mUseQuic ? "quic-protocol" : "sctp-protocol"
        });
        
        mDataChannel->onOpen([this]() {
            std::cout << "数据通道已打开，开始发送消息..." << std::endl;
            
            // 发送测试消息
            std::string testMessage = std::string("Hello from ") + (mUseQuic ? "QUIC" : "SCTP") + " DataChannel!";
            mDataChannel->send(testMessage);
            
            // 发送二进制消息
            std::vector<std::byte> binaryData = {
                std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, 
                std::byte{0x04}, std::byte{0x05}
            };
            mDataChannel->send(binaryData);
            
            // 如果是发起方，立即运行性能测试
            if (mIsOfferer && !mPerformanceTestRun) {
                std::cout << "数据通道准备就绪，立即开始性能测试..." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 短暂等待确保连接稳定
                runPerformanceTest();
                mPerformanceTestRun = true;
            } else {
                std::cout << "数据通道准备就绪，等待性能测试..." << std::endl;
            }
        });
        
        mDataChannel->onMessage([](rtc::message_variant msg) {
            if (std::holds_alternative<std::string>(msg)) {
                std::cout << "收到字符串消息: " << std::get<std::string>(msg) << std::endl;
            } else {
                auto& binary = std::get<std::vector<std::byte>>(msg);
                std::cout << "收到二进制消息，大小: " << binary.size() << " 字节" << std::endl;
            }
        });
        
        // 创建offer - 使用setLocalDescription()无参数版本
        mPeerConnection->setLocalDescription();
    }
    
    void handleOffer(const json& data) {
        std::cout << "处理offer消息..." << std::endl;
        
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
        std::cout << "处理answer消息..." << std::endl;
        
        if (!mPeerConnection) {
            std::cerr << "PeerConnection未创建" << std::endl;
            return;
        }
        
        // 设置远程描述
        rtc::Description answer(data["sdp"]);
        mPeerConnection->setRemoteDescription(answer);
    }
    
    void handleIceCandidate(const json& data) {
        if (!mPeerConnection) {
            std::cerr << "PeerConnection未创建" << std::endl;
            return;
        }
        
        // 添加远程ICE候选项
        // Candidate构造函数只接受两个参数：candidate字符串和mid
        rtc::Candidate candidate(
            data["candidate"],
            data["sdpMid"]
        );
        mPeerConnection->addRemoteCandidate(candidate);
    }
    

};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "用法: " << argv[0] << " <transport_type> <role> [signaling_ip] [signaling_port]" << std::endl;
        std::cerr << "  transport_type: quic 或 sctp" << std::endl;
        std::cerr << "  role: offerer 或 answerer" << std::endl;
        std::cerr << "  signaling_ip: 信令服务器IP地址 (默认: 127.0.0.1)" << std::endl;
        std::cerr << "  signaling_port: 信令服务器端口 (默认: 8080)" << std::endl;
        std::cerr << "示例:" << std::endl;
        std::cerr << "  " << argv[0] << " quic offerer 47.115.151.32 8080" << std::endl;
        std::cerr << "  " << argv[0] << " quic answerer 47.115.151.32 8080" << std::endl;
        return 1;
    }
    
    std::string transportType = argv[1];
    std::string role = argv[2];
    std::string signalingIp = (argc > 3) ? argv[3] : "127.0.0.1";
    int signalingPort = (argc > 4) ? std::stoi(argv[4]) : 8080;

    // 验证参数
    if (transportType != "quic" && transportType != "sctp") {
        std::cerr << "错误: transport_type 必须是 'quic' 或 'sctp'" << std::endl;
        return 1;
    }

    if (role != "offerer" && role != "answerer") {
        std::cerr << "错误: role 必须是 'offerer' 或 'answerer'" << std::endl;
        return 1;
    }

    bool isOfferer = (role == "offerer");
    bool useQuic = (transportType == "quic");

    std::cout << "=== WebRTC DataChannel 分布式测试 ===" << std::endl;
    std::cout << "传输类型: " << (useQuic ? "QUIC" : "SCTP") << std::endl;
    std::cout << "角色: " << (isOfferer ? "发起方" : "接收方") << std::endl;
    std::cout << "信令服务器: " << signalingIp << ":" << signalingPort << std::endl;
    std::cout << "本机IP: " << getLocalIp() << std::endl;
    std::cout << "=====================================" << std::endl;
    
    // 初始化日志
    rtc::InitLogger(rtc::LogLevel::Info);

    try {
        std::unique_ptr<WebRTCClient> client = std::make_unique<WebRTCClient>(useQuic, isOfferer, signalingIp, signalingPort);
        
        if (isOfferer) {
            // 发起方：等待数据通道就绪，然后运行性能测试
            if (client->waitForDataChannel(10)) {
                std::cout << "数据通道已就绪，等待性能测试完成..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(5));
            } else {
                std::cerr << "数据通道连接超时" << std::endl;
                return 1;
            }
        } else {
            // 接收方：等待数据通道就绪，然后等待测试完成
            if (client->waitForDataChannel(10)) {
                std::cout << "数据通道已就绪，等待测试完成..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(10));
            } else {
                std::cerr << "数据通道连接超时" << std::endl;
                return 1;
            }
        }

        std::cout << "测试完成，正在断开连接..." << std::endl;
        client->disconnect();
        std::cout << "连接已断开" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "客户端运行失败: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "客户端运行失败: 未知错误" << std::endl;
        return 1;
    }
    
    return 0;
} 