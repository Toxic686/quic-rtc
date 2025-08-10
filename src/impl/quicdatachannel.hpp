/**
 * Copyright (c) 2024 Your Name
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_QUIC_DATA_CHANNEL_H
#define RTC_IMPL_QUIC_DATA_CHANNEL_H

#include "channel.hpp"
#include "common.hpp"
#include "message.hpp"
#include "peerconnection.hpp"
#include "queue.hpp"
#include "reliability.hpp"
#include "quicktransport.hpp"

#include <atomic>
#include <shared_mutex>

namespace rtc::impl {

struct PeerConnection;

struct QuicDataChannel : DataChannel, std::enable_shared_from_this<QuicDataChannel> {
    static bool IsOpenMessage(message_ptr message);

    QuicDataChannel(weak_ptr<PeerConnection> pc, string label, string protocol,
                    Reliability reliability);
    virtual ~QuicDataChannel();

    void close() override;
    void remoteClose() override;
    bool outgoing(message_ptr message) override;
    void incoming(message_ptr message) override;

    optional<message_variant> receive() override;
    optional<message_variant> peek() override;
    size_t availableAmount() const override;

    optional<uint16_t> stream() const override;
    string label() const override;
    string protocol() const override;
    Reliability reliability() const override;

    bool isOpen(void) const override;
    bool isClosed(void) const override;
    size_t maxMessageSize() const override;

    virtual void assignStream(uint16_t stream) override;
    virtual void open(shared_ptr<QuicTransport> transport);
    virtual void processOpenMessage(message_ptr);

protected:
    weak_ptr<QuicTransport> mQuicTransport;

    optional<uint16_t> mStream;
    string mLabel;
    string mProtocol;
    shared_ptr<Reliability> mReliability;

    mutable std::shared_mutex mMutex;

    std::atomic<bool> mIsOpen = false;
    std::atomic<bool> mIsClosed = false;

private:
    Queue<message_ptr> mRecvQueue;
};

struct OutgoingQuicDataChannel final : public QuicDataChannel {
    OutgoingQuicDataChannel(weak_ptr<PeerConnection> pc, string label, string protocol,
                            Reliability reliability);
    ~OutgoingQuicDataChannel();

    void open(shared_ptr<QuicTransport> transport) override;
    void processOpenMessage(message_ptr message) override;
};

struct IncomingQuicDataChannel final : public QuicDataChannel {
    IncomingQuicDataChannel(weak_ptr<PeerConnection> pc, weak_ptr<QuicTransport> transport);
    ~IncomingQuicDataChannel();

    void open(shared_ptr<QuicTransport> transport) override;
    void processOpenMessage(message_ptr message) override;
};

} // namespace rtc::impl

#endif 