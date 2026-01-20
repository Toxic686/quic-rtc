#pragma once

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <functional>
#include <memory>
#include <iostream>
#include <atomic>

using json = nlohmann::json;

class SignalingClient {
public:
    using OnConnectedCallback = std::function<void(std::string clientId)>;
    using OnOfferCallback = std::function<void(const json& data)>;
    using OnAnswerCallback = std::function<void(const json& data)>;
    using OnIceCandidateCallback = std::function<void(const json& data)>;
    using OnErrorCallback = std::function<void(std::string error)>;

    SignalingClient(std::string ip, int port);
    ~SignalingClient();

    void connect();
    void disconnect();
    bool isConnected() const;
    bool waitForConnection(int timeoutSeconds);

    void sendOffer(const std::string& sdp);
    void sendAnswer(const std::string& sdp);
    void sendIceCandidate(const std::string& candidate, const std::string& sdpMid, int sdpMLineIndex);

    void onConnected(OnConnectedCallback callback);
    void onOffer(OnOfferCallback callback);
    void onAnswer(OnAnswerCallback callback);
    void onIceCandidate(OnIceCandidateCallback callback);
    void onError(OnErrorCallback callback);

private:
    void handleMessage(const std::string& message);

    std::string mIp;
    int mPort;
    std::shared_ptr<rtc::WebSocket> mWebSocket;
    std::atomic<bool> mConnected;
    std::string mClientId;

    OnConnectedCallback mOnConnectedCallback;
    OnOfferCallback mOnOfferCallback;
    OnAnswerCallback mOnAnswerCallback;
    OnIceCandidateCallback mOnIceCandidateCallback;
    OnErrorCallback mOnErrorCallback;
};
