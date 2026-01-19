/**
 * Copyright (c) 2019-2022 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "transport.hpp"

#include <typeinfo>
#include <cstring>
#include <string>
#include <cctype>
#include <cstdlib>

namespace rtc::impl {

namespace {
// 通过环境变量控制 Transport 层的额外调试输出（默认关闭）
// - RTC_TRANSPORT_TRACE=1/true/on/yes : 打开
bool transport_trace_enabled() {
	static const bool enabled = []() -> bool {
		const char *v = std::getenv("RTC_TRANSPORT_TRACE");
		if (!v) return false;
		std::string s(v);
		for (auto &c : s)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s == "1" || s == "true" || s == "on" || s == "yes";
	}();
	return enabled;
}
} // namespace

Transport::Transport(shared_ptr<Transport> lower, state_callback callback)
    : mLower(std::move(lower)), mStateChangeCallback(std::move(callback)) {}

Transport::~Transport() {
	unregisterIncoming();

	if (mLower) {
		mLower->stop();
		mLower.reset();
	}
}

void Transport::registerIncoming() {
	if (mLower) {
		PLOG_VERBOSE << "Registering incoming callback";
		if (transport_trace_enabled()) {
			PLOG_VERBOSE << "Incoming callback registered for " << typeid(*this).name();
		}
		mLower->onRecv(std::bind(&Transport::incoming, this, std::placeholders::_1));
	} else {
		if (transport_trace_enabled()) {
			// 这在构造/重连过程中是常见现象，不应当作为 warning 刷屏
			PLOG_VERBOSE << "registerIncoming() called but no lower transport";
		}
	}
}

void Transport::unregisterIncoming() {
	if (mLower) {
		PLOG_VERBOSE << "Unregistering incoming callback";
		mLower->onRecv(nullptr);
	}
}

Transport::State Transport::state() const { return mState; }

void Transport::onRecv(message_callback callback) { mRecvCallback = std::move(callback); }

void Transport::onStateChange(state_callback callback) {
	mStateChangeCallback = std::move(callback);
}

void Transport::start() { registerIncoming(); }

void Transport::stop() { unregisterIncoming(); }

bool Transport::send(message_ptr message) { return outgoing(message); }

void Transport::recv(message_ptr message) {
	try {
		if (mRecvCallback) {
			mRecvCallback(message);
		} else {
			// 清理/断开阶段，回调被注销但底层可能还有零星包到达：不要刷屏告警
			const auto st = mState.load();
			if (st != State::Disconnected && st != State::Completed && st != State::Failed) {
				PLOG_WARNING << "Transport::recv() called but no callback registered";
			} else if (transport_trace_enabled()) {
				PLOG_VERBOSE << "recv() dropped (no callback, state=" << static_cast<int>(st) << ")";
			}
		}
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void Transport::changeState(State state) {
	try {
		if (mState.exchange(state) != state)
			mStateChangeCallback(state);
	} catch (const std::exception &e) {
		PLOG_WARNING << e.what();
	}
}

void Transport::incoming(message_ptr message) { recv(message); }

bool Transport::outgoing(message_ptr message) {
	if (mLower)
		return mLower->send(message);
	else
		return false;
}

} // namespace rtc::impl
