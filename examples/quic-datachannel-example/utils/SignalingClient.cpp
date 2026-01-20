#include "SignalingClient.hpp"
#include <thread>
#include <chrono>

SignalingClient::SignalingClient(std::string ip, int port)
    : mIp(ip), mPort(port), mConnected(false) {
}

SignalingClient::~SignalingClient() {
    disconnect();
}

void SignalingClient::connect() {
    if (mWebSocket) {
        // Already connected or connecting, maybe close first?
        // For simplicity, assume connect is called when disconnected.
        try { mWebSocket->close(); } catch(...) {}
    }
    mWebSocket = std::make_shared<rtc::WebSocket>();

    mWebSocket->onOpen([this]() {
        std::cout << "WebSocket连接已建立" << std::endl;
    });

    mWebSocket->onMessage([this](rtc::message_variant msg) {
        if (std::holds_alternative<std::string>(msg)) {
            handleMessage(std::get<std::string>(msg));
        }
    });

    mWebSocket->onClosed([this]() {
        std::cout << "WebSocket连接已关闭" << std::endl;
        mConnected = false;
    });

    mWebSocket->onError([this](std::string error) {
        if (mOnErrorCallback) {
            mOnErrorCallback(error);
        } else {
            std::cerr << "WebSocket错误: " << error << std::endl;
        }
    });

    std::string uri = "ws://" + mIp + ":" + std::to_string(mPort);
    std::cout << "连接到信令服务器: " << uri << std::endl;
    mWebSocket->open(uri);
}

void SignalingClient::disconnect() {
    if (mWebSocket) {
        try {
            mWebSocket->close();
        } catch (...) {}
        // We don't reset mWebSocket here immediately to avoid race conditions 
        // if callbacks are running, but libdatachannel handles this usually.
        // For safety, we can reset it, but we need to be careful.
        // Given we are in a destructor or explicit disconnect, we can reset.
        mWebSocket.reset();
    }
    mConnected = false;
}

bool SignalingClient::isConnected() const {
    return mConnected;
}

bool SignalingClient::waitForConnection(int timeoutSeconds) {
    auto start = std::chrono::steady_clock::now();
    while (!mConnected && std::chrono::steady_clock::now() - start < std::chrono::seconds(timeoutSeconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return mConnected;
}

void SignalingClient::sendOffer(const std::string& sdp) {
    if (!mWebSocket) return;
    json msg = {
        {"type", "offer"},
        {"sdp", sdp}
    };
    mWebSocket->send(msg.dump());
}

void SignalingClient::sendAnswer(const std::string& sdp) {
    if (!mWebSocket) return;
    json msg = {
        {"type", "answer"},
        {"sdp", sdp}
    };
    mWebSocket->send(msg.dump());
}

void SignalingClient::sendIceCandidate(const std::string& candidate, const std::string& sdpMid, int sdpMLineIndex) {
    if (!mWebSocket) return;
    json msg = {
        {"type", "ice-candidate"},
        {"candidate", candidate},
        {"sdpMid", sdpMid},
        {"sdpMLineIndex", sdpMLineIndex}
    };
    mWebSocket->send(msg.dump());
}

void SignalingClient::handleMessage(const std::string& message) {
    try {
        json data = json::parse(message);
        std::string type = data["type"];

        std::cout << "收到信令消息: " << type << std::endl;

        if (type == "connected") {
            mClientId = data["clientId"];
            mConnected = true;
            if (mOnConnectedCallback) mOnConnectedCallback(mClientId);
        } else if (type == "offer") {
            if (mOnOfferCallback) mOnOfferCallback(data);
        } else if (type == "answer") {
            if (mOnAnswerCallback) mOnAnswerCallback(data);
        } else if (type == "ice-candidate") {
            if (mOnIceCandidateCallback) mOnIceCandidateCallback(data);
        } else if (type == "pong") {
            std::cout << "收到pong消息" << std::endl;
        } else {
            std::cout << "未知消息类型: " << type << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "解析消息失败: " << e.what() << std::endl;
    }
}

void SignalingClient::onConnected(OnConnectedCallback callback) { mOnConnectedCallback = callback; }
void SignalingClient::onOffer(OnOfferCallback callback) { mOnOfferCallback = callback; }
void SignalingClient::onAnswer(OnAnswerCallback callback) { mOnAnswerCallback = callback; }
void SignalingClient::onIceCandidate(OnIceCandidateCallback callback) { mOnIceCandidateCallback = callback; }
void SignalingClient::onError(OnErrorCallback callback) { mOnErrorCallback = callback; }
