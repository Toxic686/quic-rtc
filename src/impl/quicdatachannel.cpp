/**
 * Copyright (c) 2024 Your Name
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "quicdatachannel.hpp"
#include "peerconnection.hpp"
#include "quicktransport.hpp"
#include "utils.hpp"
#include "message.hpp"
#include "reliability.hpp"

#include <cstring>
#include <algorithm>
#include <chrono>
#include <arpa/inet.h>

#include "message.hpp"
#include "reliability.hpp"

namespace rtc::impl {

QuicDataChannel::QuicDataChannel(weak_ptr<PeerConnection> pc, string label, string protocol,
                                 Reliability reliability)
    : DataChannel(std::move(pc), std::move(label), std::move(protocol), std::move(reliability)) {
    // 继承 DataChannel 已保存一份可靠性/label/protocol，这里为本类成员再拷贝一份，避免空指针
    mReliability = std::make_shared<Reliability>(DataChannel::reliability());
    mLabel = DataChannel::label();
    mProtocol = DataChannel::protocol();
}

QuicDataChannel::~QuicDataChannel() = default;

namespace {
// 与 DataChannel 控制报文保持一致
constexpr uint8_t kMsgOpen = 0x03;
constexpr uint8_t kMsgAck  = 0x02;
constexpr uint8_t kChannelReliable = 0x00;
constexpr uint8_t kChannelPartialReliableRexmit = 0x01;
constexpr uint8_t kChannelPartialReliableTimed  = 0x02;
using std::chrono::milliseconds;

#pragma pack(push, 1)
struct QuicOpenMessage {
    uint8_t type = kMsgOpen;
    uint8_t channelType;
    uint16_t priority;
    uint32_t reliabilityParameter;
    uint16_t labelLength;
    uint16_t protocolLength;
    // 后续紧跟 label 和 protocol 字节
};
struct QuicAckMessage {
    uint8_t type = kMsgAck;
};
#pragma pack(pop)
} // namespace

bool QuicDataChannel::IsOpenMessage(message_ptr message) {
    // 与 DataChannel::IsOpenMessage 对齐：控制报文且首字节为 OPEN
    return message && message->type == Message::Control && !message->empty()
           && reinterpret_cast<const uint8_t *>(message->data())[0] == kMsgOpen;
}

void QuicDataChannel::close() {
    std::lock_guard<std::shared_mutex> lock(mMutex);
    if (!mIsClosed.exchange(true)) {
        mIsOpen = false;
        if (auto transport = mQuicTransport.lock()) {
            if (mStream) {
                transport->closeStream(*mStream);
            }
        }
        triggerClosed();
    }
    resetCallbacks();
}

void QuicDataChannel::remoteClose() {
    close();
}

bool QuicDataChannel::outgoing(message_ptr message) {
    std::shared_lock<std::shared_mutex> lock(mMutex);
    if (!message || !mStream || mIsClosed) {
        return false;
    }
    // 在握手 ACK 前强制有序发送，与 SCTP DataChannel 行为一致
    message->reliability = mIsOpen ? mReliability : nullptr;
    message->stream = *mStream;

    if (auto transport = mQuicTransport.lock()) {
        return transport->send(std::move(message));
    }
    return false;
}

void QuicDataChannel::incoming(message_ptr message) {
    if (!message || mIsClosed)
        return;

    switch (message->type) {
    case Message::Control: {
        if (message->size() == 0)
            break; // ignore
        auto raw = reinterpret_cast<const uint8_t *>(message->data());
        switch (raw[0]) {
        case kMsgOpen:
            processOpenMessage(message);
            break;
        case kMsgAck:
            if (!mIsOpen.exchange(true)) {
                triggerOpen();
            }
            break;
        default:
            break;
        }
        break;
    }
    case Message::Reset:
        remoteClose();
        break;
    case Message::String:
    case Message::Binary:
        mRecvQueue.push(message);
        triggerAvailable(mRecvQueue.size());
        break;
    default:
        break;
    }
}

optional<message_variant> QuicDataChannel::receive() {
    std::lock_guard<std::shared_mutex> lock(mMutex);
    if (auto message = mRecvQueue.pop()) {
        if ((*message)->type == Message::Type::String) {
            std::string str(reinterpret_cast<const char*>((*message)->data()), (*message)->size());
            return message_variant{std::move(str)};
        } else {
            // For binary data, create a copy of the binary data
            binary data((*message)->begin(), (*message)->end());
            return message_variant{std::move(data)};
        }
    }
    return std::nullopt;
}

optional<message_variant> QuicDataChannel::peek() {
    std::shared_lock<std::shared_mutex> lock(mMutex);
    if (auto message = mRecvQueue.peek()) {
        if ((*message)->type == Message::Type::String) {
            std::string str(reinterpret_cast<const char*>((*message)->data()), (*message)->size());
            return message_variant{std::move(str)};
        } else {
            // For binary data, create a copy of the binary data
            binary data((*message)->begin(), (*message)->end());
            return message_variant{std::move(data)};
        }
    }
    return std::nullopt;
}

size_t QuicDataChannel::availableAmount() const {
    std::shared_lock<std::shared_mutex> lock(mMutex);
    return mRecvQueue.size();
}

// 这些方法现在从DataChannel继承，使用基类实现
optional<uint16_t> QuicDataChannel::stream() const {
    std::shared_lock<std::shared_mutex> lock(mMutex);
    return mStream;
}

string QuicDataChannel::label() const {
    std::shared_lock<std::shared_mutex> lock(mMutex);
    return mLabel;
}

string QuicDataChannel::protocol() const {
    std::shared_lock<std::shared_mutex> lock(mMutex);
    return mProtocol;
}

Reliability QuicDataChannel::reliability() const {
    std::shared_lock<std::shared_mutex> lock(mMutex);
    return mReliability ? *mReliability : Reliability{};
}

bool QuicDataChannel::isOpen() const {
    return !mIsClosed && mIsOpen;
}

bool QuicDataChannel::isClosed() const {
    return mIsClosed;
}

size_t QuicDataChannel::maxMessageSize() const {
    auto pc = mPeerConnection.lock();
    return pc ? pc->remoteMaxMessageSize() : DEFAULT_REMOTE_MAX_MESSAGE_SIZE;
}

void QuicDataChannel::assignStream(uint16_t stream) {
    std::lock_guard<std::shared_mutex> lock(mMutex);
    mStream = stream;
}

void QuicDataChannel::open(shared_ptr<QuicTransport> transport) {
    std::lock_guard<std::shared_mutex> lock(mMutex);
    mQuicTransport = transport;
    mIsClosed = false;
}

void QuicDataChannel::processOpenMessage(message_ptr message) {
    // Process QUIC data channel open message
    // This would parse the message and set up the channel accordingly
    if (IsOpenMessage(message)) {
        open(mQuicTransport.lock());
    }
}

// OutgoingQuicDataChannel implementation
OutgoingQuicDataChannel::OutgoingQuicDataChannel(weak_ptr<PeerConnection> pc, string label,
                                                 string protocol, Reliability reliability)
    : QuicDataChannel(std::move(pc), std::move(label), std::move(protocol), std::move(reliability)) {}

OutgoingQuicDataChannel::~OutgoingQuicDataChannel() = default;

void OutgoingQuicDataChannel::open(shared_ptr<QuicTransport> transport) {
    QuicDataChannel::open(transport);
    
    if (!transport) {
        std::cerr << "   ❌ 错误: transport指针为空！" << std::endl;
        return;
    }
    if (!mStream.has_value())
        throw std::runtime_error("DataChannel has no stream assigned");

    // 关键：只发送一次 OPEN（PeerConnection 可能会在 ACK 前多次调用 open()）
    if (mOpenSent.exchange(true)) {
        return;
    }

    uint8_t channelType;
    uint32_t reliabilityParameter;
    if (mReliability->maxPacketLifeTime) {
        channelType = kChannelPartialReliableTimed;
        reliabilityParameter = utils::to_uint32(mReliability->maxPacketLifeTime->count());
    } else if (mReliability->maxRetransmits) {
        channelType = kChannelPartialReliableRexmit;
        reliabilityParameter = utils::to_uint32(*mReliability->maxRetransmits);
    } else {
        switch (mReliability->typeDeprecated) {
        case Reliability::Type::Rexmit:
            channelType = kChannelPartialReliableRexmit;
            reliabilityParameter = utils::to_uint32(std::max(std::get<int>(mReliability->rexmit), 0));
            break;
        case Reliability::Type::Timed:
            channelType = kChannelPartialReliableTimed;
            reliabilityParameter = utils::to_uint32(std::get<milliseconds>(mReliability->rexmit).count());
            break;
        default:
            channelType = kChannelReliable;
            reliabilityParameter = 0;
            break;
        }
    }
    if (mReliability->unordered)
        channelType |= 0x80;

    const size_t len = sizeof(QuicOpenMessage) + mLabel.size() + mProtocol.size();
    binary buffer(len, byte(0));
    auto &open = *reinterpret_cast<QuicOpenMessage *>(buffer.data());
    open.type = kMsgOpen;
    open.channelType = channelType;
    open.priority = htons(0);
    open.reliabilityParameter = htonl(reliabilityParameter);
    open.labelLength = htons(utils::to_uint16(mLabel.size()));
    open.protocolLength = htons(utils::to_uint16(mProtocol.size()));

    auto end = reinterpret_cast<char *>(buffer.data() + sizeof(QuicOpenMessage));
    std::copy(mLabel.begin(), mLabel.end(), end);
    std::copy(mProtocol.begin(), mProtocol.end(), end + mLabel.size());

    auto message = make_message(buffer.begin(), buffer.end(), Message::Control, mStream.value());
    std::cout << "📤 [QUIC DataChannel] 数据通道打开，准备发送打开消息，stream=" << *mStream << std::endl;
    bool sent = transport->send(std::move(message));
    std::cout << "   " << (sent ? "✅" : "❌") << " 发送结果: " << (sent ? "成功" : "失败") << std::endl;
}

void OutgoingQuicDataChannel::processOpenMessage(message_ptr message) {
    // Outgoing channels don't process open messages from remote
    // They only send them
    (void)message; // 标记参数为已使用，避免警告
}

// IncomingQuicDataChannel implementation
IncomingQuicDataChannel::IncomingQuicDataChannel(weak_ptr<PeerConnection> pc,
                                                 weak_ptr<QuicTransport> transport)
    : QuicDataChannel(std::move(pc), "", "", Reliability()) {
    mQuicTransport = transport;
}

IncomingQuicDataChannel::~IncomingQuicDataChannel() = default;

void IncomingQuicDataChannel::open(shared_ptr<QuicTransport> transport) {
    QuicDataChannel::open(transport);
}

void IncomingQuicDataChannel::processOpenMessage(message_ptr message) {
    if (!message || message->size() < sizeof(QuicOpenMessage))
        throw std::invalid_argument("DataChannel open message too small");

    QuicOpenMessage openMsg = *reinterpret_cast<const QuicOpenMessage *>(message->data());
    openMsg.priority = ntohs(openMsg.priority);
    openMsg.reliabilityParameter = ntohl(openMsg.reliabilityParameter);
    openMsg.labelLength = ntohs(openMsg.labelLength);
    openMsg.protocolLength = ntohs(openMsg.protocolLength);

    if (message->size() < sizeof(QuicOpenMessage) + size_t(openMsg.labelLength + openMsg.protocolLength))
        throw std::invalid_argument("DataChannel open message truncated");

    auto end = reinterpret_cast<const char *>(message->data() + sizeof(QuicOpenMessage));
    mLabel.assign(end, openMsg.labelLength);
    mProtocol.assign(end + openMsg.labelLength, openMsg.protocolLength);

    mReliability->unordered = (openMsg.channelType & 0x80) != 0;
    mReliability->maxPacketLifeTime.reset();
    mReliability->maxRetransmits.reset();
    switch (openMsg.channelType & 0x7F) {
    case kChannelPartialReliableRexmit:
        mReliability->maxRetransmits.emplace(openMsg.reliabilityParameter);
        break;
    case kChannelPartialReliableTimed:
        mReliability->maxPacketLifeTime.emplace(milliseconds(openMsg.reliabilityParameter));
        break;
    default:
        break;
    }
    // Deprecated兼容
    switch (openMsg.channelType & 0x7F) {
    case kChannelPartialReliableRexmit:
        mReliability->typeDeprecated = Reliability::Type::Rexmit;
        mReliability->rexmit = int(openMsg.reliabilityParameter);
        break;
    case kChannelPartialReliableTimed:
        mReliability->typeDeprecated = Reliability::Type::Timed;
        mReliability->rexmit = milliseconds(openMsg.reliabilityParameter);
        break;
    default:
        mReliability->typeDeprecated = Reliability::Type::Reliable;
        mReliability->rexmit = int(0);
    }

    // 发送 ACK
    binary buffer(sizeof(QuicAckMessage), byte(0));
    auto &ack = *reinterpret_cast<QuicAckMessage *>(buffer.data());
    ack.type = kMsgAck;

    if (auto transport = mQuicTransport.lock()) {
        const bool sent = transport->send(make_message(buffer.begin(), buffer.end(), Message::Control, message->stream));
        if (!sent) {
            // 如果 ACK 发送失败，不要宣告 open，否则对端永远等不到 ACK
            std::cerr << "   ❌ [QUIC DataChannel] ACK发送失败，stream=" << message->stream << std::endl;
            return;
        }
        this->open(transport);
        if (!mIsOpen.exchange(true)) {
            triggerOpen();
        }
    }
}

} // namespace rtc::impl 