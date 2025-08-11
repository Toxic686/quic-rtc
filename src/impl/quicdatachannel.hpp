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

    // 这些函数在基类中不是虚函数，所以不能覆盖
    void close();
    void remoteClose();
    bool outgoing(message_ptr message);
    void incoming(message_ptr message);

    // 这些函数在基类中是虚函数，可以覆盖
    optional<message_variant> receive() override;
    optional<message_variant> peek() override;
    size_t availableAmount() const override;

    // 这些函数在基类中不是虚函数，所以不能覆盖
    optional<uint16_t> stream() const;
    string label() const;
    string protocol() const;
    Reliability reliability() const;

    bool isOpen(void) const;
    bool isClosed(void) const;
    size_t maxMessageSize() const;

    virtual void assignStream(uint16_t stream) override;
    // 添加基类的open函数声明，避免隐藏
    using DataChannel::open;
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