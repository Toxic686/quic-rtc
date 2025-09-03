#include <rtc/rtc.hpp>
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "=== 简化QUIC DataChannel示例 ===" << std::endl;
    
    // 初始化rtc库
    rtc::InitLogger(rtc::LogLevel::Debug);
    
    // 创建配置
    rtc::Configuration config;
    config.enableQuicTransport = true;
    config.quicMaxStreamsIn = 100;
    config.quicMaxStreamsOut = 100;
    config.quicHandshakeTimeout = std::chrono::milliseconds(60000);
    config.quicIdleTimeout = std::chrono::milliseconds(120000);
    config.quicPingPeriod = std::chrono::milliseconds(30000);
    
    std::cout << "创建QUIC PeerConnection..." << std::endl;
    
    // 创建PeerConnection
    auto pc = std::make_shared<rtc::PeerConnection>(config);
    
    // 设置数据通道回调
    pc->onDataChannel([](std::shared_ptr<rtc::DataChannel> dc) {
        std::cout << "收到数据通道: " << dc->label() << std::endl;
        
        dc->onOpen([]() {
            std::cout << "数据通道已打开" << std::endl;
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
    
    // 创建数据通道
    std::cout << "创建QUIC数据通道..." << std::endl;
    
    // 配置可靠性（可靠传输）
    rtc::Reliability reliability;
    reliability.unordered = false;  // 有序传输
    // 不设置maxPacketLifeTime和maxRetransmits，表示可靠传输
    
    auto dc = pc->createDataChannel("test", {
        .reliability = reliability,
        .protocol = "quic-protocol"
    });
    
    dc->onOpen([dc = dc]() {
        std::cout << "QUIC数据通道已打开，开始发送消息..." << std::endl;
        
        // 发送字符串消息
        std::string testMessage = "Hello from QUIC DataChannel!";
        dc->send(testMessage);
        std::cout << "已发送字符串消息: " << testMessage << std::endl;
        
        // 发送二进制消息
        std::vector<std::byte> binaryData = {
            std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, 
            std::byte{0x04}, std::byte{0x05}
        };
        dc->send(binaryData);
        std::cout << "已发送二进制消息，大小: " << binaryData.size() << " 字节" << std::endl;
        
        // 性能测试
        const int numMessages = 100;
        const int messageSize = 1024; // 1KB
        
        std::vector<std::byte> testData(messageSize, std::byte{0xAA});
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < numMessages; ++i) {
            dc->send(testData);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << "\n=== 性能测试结果 ===" << std::endl;
        std::cout << "传输方式: QUIC" << std::endl;
        std::cout << "发送消息数: " << numMessages << std::endl;
        std::cout << "消息大小: " << messageSize << " 字节" << std::endl;
        std::cout << "总时间: " << duration.count() << " 毫秒" << std::endl;
        std::cout << "平均每条消息: " << duration.count() / (double)numMessages << " 毫秒" << std::endl;
        std::cout << "吞吐量: " << (numMessages * messageSize * 8.0) / (duration.count() / 1000.0) / 1000000.0 << " Mbps" << std::endl;
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
        std::cout << "QUIC数据通道已关闭" << std::endl;
    });
    
    std::cout << "QUIC DataChannel示例已启动" << std::endl;
    std::cout << "注意：这是一个简化的示例，没有真实的网络连接" << std::endl;
    std::cout << "要测试真实的网络连接，请使用完整的端到端示例" << std::endl;
    
    // 等待一段时间
    std::this_thread::sleep_for(std::chrono::seconds(5));
    
    std::cout << "示例完成" << std::endl;
    
    return 0;
} 