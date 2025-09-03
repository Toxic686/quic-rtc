#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <chrono>

using json = nlohmann::json;

class SignalingServer {
private:
    std::map<std::string, std::shared_ptr<rtc::WebSocket>> mClients;
    std::mutex mMutex;
    std::shared_ptr<rtc::WebSocketServer> mServer;
    bool mRunning;
    int mPort;

public:
    SignalingServer(int port = 8080) : mRunning(false), mPort(port) {
        // 创建WebSocket服务器
        rtc::WebSocketServer::Configuration config;
        config.port = mPort;
        config.enableTls = false;  // 禁用TLS，使用普通WebSocket
        
        mServer = std::make_shared<rtc::WebSocketServer>(config);
        
        // 设置连接处理器
        mServer->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
            std::cout << "新客户端连接" << std::endl;
            
            // 生成客户端ID
            std::string clientId = "client_" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            
            // 设置消息处理器
            ws->onMessage([this, ws, clientId](rtc::message_variant msg) {
                if (std::holds_alternative<std::string>(msg)) {
                    handleMessage(ws, std::get<std::string>(msg), clientId);
                }
            });
            
            // 设置关闭处理器
            ws->onClosed([this, clientId]() {
                std::cout << "客户端断开: " << clientId << std::endl;
                std::lock_guard<std::mutex> lock(mMutex);
                mClients.erase(clientId);
            });
            
            // 设置错误处理器
            ws->onError([clientId](std::string error) {
                std::cout << "客户端错误 " << clientId << ": " << error << std::endl;
            });
            
            // 设置打开处理器，在WebSocket完全打开后发送连接确认
            ws->onOpen([this, ws, clientId]() {
                std::cout << "WebSocket连接已打开，发送连接确认给客户端: " << clientId << std::endl;
            
            // 添加到客户端列表
                {
            std::lock_guard<std::mutex> lock(mMutex);
            mClients[clientId] = ws;
                }
                
                // 发送连接确认
                json response = {
                    {"type", "connected"},
                    {"clientId", clientId}
                };
                
                try {
                    ws->send(response.dump());
                    std::cout << "已发送连接确认给客户端: " << clientId << std::endl;
                } catch (const std::exception& e) {
                    std::cout << "发送连接确认失败: " << e.what() << std::endl;
                }
            });
        });
    }
    
    void run() {
        std::cout << "启动信令服务器，端口: " << mServer->port() << std::endl;
        mRunning = true;
        
        // WebSocketServer在构造函数中就已经自动启动
        std::cout << "信令服务器已启动，等待客户端连接..." << std::endl;
        
        // 保持服务器运行
        while (mRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    void stop() {
        mRunning = false;
        if (mServer) {
            mServer->stop();
        }
    }
    
    int getPort() const {
        return mPort;
    }

private:
    void handleMessage(std::shared_ptr<rtc::WebSocket> ws, const std::string& message, const std::string& fromId) {
        try {
            json data = json::parse(message);
            std::string type = data["type"];
            
            std::cout << "收到消息类型: " << type << " 来自: " << fromId << std::endl;
            
            if (type == "offer") {
                handleOffer(ws, data, fromId);
            } else if (type == "answer") {
                handleAnswer(ws, data, fromId);
            } else if (type == "ice-candidate") {
                handleIceCandidate(ws, data, fromId);
            } else if (type == "ping") {
                // 处理ping消息，保持连接活跃
                json pong = {{"type", "pong"}};
                ws->send(pong.dump());
            } else {
                std::cout << "未知消息类型: " << type << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "解析消息失败: " << e.what() << std::endl;
        }
    }
    
    void handleOffer(std::shared_ptr<rtc::WebSocket> ws, const json& data, const std::string& fromId) {
        std::lock_guard<std::mutex> lock(mMutex);
        
        std::cout << "转发offer，当前客户端数: " << mClients.size() << std::endl;
        
        // 转发offer给所有其他客户端
        for (const auto& client : mClients) {
            if (client.first != fromId) {
                try {
                json forward = {
                    {"type", "offer"},
                    {"sdp", data["sdp"]},
                    {"from", fromId}
                };
                client.second->send(forward.dump());
                    std::cout << "已转发offer给客户端: " << client.first << std::endl;
                } catch (const std::exception& e) {
                    std::cout << "转发offer失败给客户端 " << client.first << ": " << e.what() << std::endl;
                }
            }
        }
    }
    
    void handleAnswer(std::shared_ptr<rtc::WebSocket> ws, const json& data, const std::string& fromId) {
        std::lock_guard<std::mutex> lock(mMutex);
        
        std::cout << "转发answer，当前客户端数: " << mClients.size() << std::endl;
        
        // 转发answer给所有其他客户端
        for (const auto& client : mClients) {
            if (client.first != fromId) {
                try {
                json forward = {
                    {"type", "answer"},
                    {"sdp", data["sdp"]},
                    {"from", fromId}
                };
                client.second->send(forward.dump());
                    std::cout << "已转发answer给客户端: " << client.first << std::endl;
                } catch (const std::exception& e) {
                    std::cout << "转发answer失败给客户端 " << client.first << ": " << e.what() << std::endl;
                }
            }
        }
    }
    
    void handleIceCandidate(std::shared_ptr<rtc::WebSocket> ws, const json& data, const std::string& fromId) {
        std::lock_guard<std::mutex> lock(mMutex);
        
        // 转发ICE候选项给所有其他客户端
        for (const auto& client : mClients) {
            if (client.first != fromId) {
                try {
                json forward = {
                    {"type", "ice-candidate"},
                    {"candidate", data["candidate"]},
                    {"sdpMid", data["sdpMid"]},
                    {"sdpMLineIndex", data["sdpMLineIndex"]},
                    {"from", fromId}
                };
                client.second->send(forward.dump());
                } catch (const std::exception& e) {
                    std::cout << "转发ICE候选项失败给客户端 " << client.first << ": " << e.what() << std::endl;
                }
            }
        }
    }
};

int main(int argc, char* argv[]) {
    try {
        // 初始化rtc库
        rtc::InitLogger(rtc::LogLevel::Info);
        
        int port = 8080; // 默认端口
        if (argc > 1) {
            try {
                port = std::stoi(argv[1]);
            } catch (const std::exception& e) {
                std::cerr << "无效的端口参数: " << argv[1] << std::endl;
                std::cerr << "用法: " << argv[0] << " [port]" << std::endl;
                std::cerr << "默认端口: 8080" << std::endl;
                return 1;
            }
        }
        
        std::cout << "启动信令服务器，端口: " << port << std::endl;
        SignalingServer server(port);
        
        // 启动服务器
        server.run();
        
        // 注意：不再使用std::cin.get()，避免阻塞自动化测试
        // 服务器会一直运行直到收到停止信号
        
    } catch (const std::exception& e) {
        std::cerr << "服务器启动失败: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
} 