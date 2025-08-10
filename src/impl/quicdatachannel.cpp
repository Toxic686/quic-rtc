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

#include "message.hpp"
#include "reliability.hpp"

namespace rtc::impl {

QuicDataChannel::QuicDataChannel(weak_ptr<PeerConnection> pc, string label, string protocol,
                                 Reliability reliability)
    : DataChannel(std::move(pc), std::move(label), std::move(protocol), std::move(reliability)) {}

QuicDataChannel::~QuicDataChannel() = default;

bool QuicDataChannel::IsOpenMessage(message_ptr message) {
    // Check if this is a QUIC data channel open message
    // This is a simplified check - in practice you'd need to parse the message format
    return message && message->size() > 0;
}

void QuicDataChannel::close() {
    std::lock_guard<std::shared_mutex> lock(mMutex);
    if (!mIsClosed) {
        mIsClosed = true;
        mIsOpen = false;
        
        if (auto transport = mQuicTransport.lock()) {
            if (mStream) {
                transport->closeStream(*mStream);
            }
        }
    }
}

void QuicDataChannel::remoteClose() {
    std::lock_guard<std::shared_mutex> lock(mMutex);
    if (!mIsClosed) {
        mIsClosed = true;
        mIsOpen = false;
    }
}

bool QuicDataChannel::outgoing(message_ptr message) {
    std::shared_lock<std::shared_mutex> lock(mMutex);
    if (!mIsOpen || mIsClosed) {
        return false;
    }

    if (auto transport = mQuicTransport.lock()) {
        return transport->send(std::move(message));
    }
    return false;
}

void QuicDataChannel::incoming(message_ptr message) {
    if (!message) return;

    std::lock_guard<std::shared_mutex> lock(mMutex);
    if (!mIsClosed) {
        mRecvQueue.push(std::move(message));
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
    return DataChannel::stream();
}

string QuicDataChannel::label() const {
    return DataChannel::label();
}

string QuicDataChannel::protocol() const {
    return DataChannel::protocol();
}

Reliability QuicDataChannel::reliability() const {
    return DataChannel::reliability();
}

bool QuicDataChannel::isOpen() const {
    return DataChannel::isOpen();
}

bool QuicDataChannel::isClosed() const {
    return DataChannel::isClosed();
}

size_t QuicDataChannel::maxMessageSize() const {
    return DataChannel::maxMessageSize();
}

void QuicDataChannel::assignStream(uint16_t stream) {
    std::lock_guard<std::shared_mutex> lock(mMutex);
    mStream = stream;
}

void QuicDataChannel::open(shared_ptr<QuicTransport> transport) {
    std::lock_guard<std::shared_mutex> lock(mMutex);
    mQuicTransport = transport;
    mIsOpen = true;
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
    
    // Send open message through QUIC stream
    if (transport) {
        // Create open message for QUIC data channel
        // This would be similar to SCTP but adapted for QUIC
        std::string openMsg = "QUIC_DATA_CHANNEL_OPEN:" + mLabel + ":" + mProtocol;
        auto message = make_message(openMsg.begin(), openMsg.end(), Message::Type::String);
        transport->send(std::move(message));
    }
}

void OutgoingQuicDataChannel::processOpenMessage(message_ptr message) {
    // Outgoing channels don't process open messages from remote
    // They only send them
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
    // Parse the open message to extract label and protocol
    if (IsOpenMessage(message)) {
        std::string data(reinterpret_cast<const char*>(message->data()), message->size());
        
        // Parse "QUIC_DATA_CHANNEL_OPEN:label:protocol" format
        size_t pos = data.find("QUIC_DATA_CHANNEL_OPEN:");
        if (pos != std::string::npos) {
            std::string remaining = data.substr(pos + 23); // length of "QUIC_DATA_CHANNEL_OPEN:"
            size_t colonPos = remaining.find(':');
            if (colonPos != std::string::npos) {
                mLabel = remaining.substr(0, colonPos);
                mProtocol = remaining.substr(colonPos + 1);
            }
        }
        
        if (auto transport = mQuicTransport.lock()) {
            open(transport);
        }
    }
}

} // namespace rtc::impl 