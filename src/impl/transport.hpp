/**
 * Copyright (c) 2019-2022 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_TRANSPORT_H
#define RTC_IMPL_TRANSPORT_H

#include "common.hpp"
#include "init.hpp"
#include "internals.hpp"
#include "message.hpp"

#include <atomic>
#include <functional>
#include <memory>

namespace rtc::impl {

class Transport {
public:
	enum class State {
		Disconnected, // 断开连接状态 - 初始状态或连接失败后的状态
		Connecting, // 正在连接状态 - 正在进行连接建立过程
		Connected, // 已连接状态 - 连接成功建立，可以传输数据
		Completed, // 已完成状态 - 数据传输完成，连接正常关闭
		Failed  // 失败状态 - 连接建立失败或传输过程中出现错误
	};
	// 状态变化回调函数类型
	using state_callback = std::function<void(State state)>; 

	/**
	 * 构造函数
	 * @param lower 下层传输层对象的智能指针
	 * @param callback 状态变化回调函数
	*/
	Transport(shared_ptr<Transport> lower = nullptr, state_callback callback = nullptr);
	/**
	 * 析构函数
	 */
	virtual ~Transport();
	/**
	 * 注册接收消息回调函数 - 向下层传输层注册数据接收回调
	 */
	void registerIncoming();
	/**
	 * 取消注册接收消息回调函数 - 取消向下层传输层注册的数据接收回调
	 */
	void unregisterIncoming();
	/**
	 * 获取当前传输层状态
	 * @return 当前传输层状态
	 */
	State state() const;

	/**
	 * 注册接收消息回调函数 - 设置上层的数据接收回调函数
	 * @param callback 接收消息回调函数
	 */
	void onRecv(message_callback callback);
	/**
	 * 注册状态变化回调函数 - 设置状态变化时的回调函数
	 */
	void onStateChange(state_callback callback);

	/**
	 * 启动传输层 - 开始接收和发送数据
	 */
	virtual void start();
	/**
	 * 停止传输层 - 停止接收和发送数据
	 */
	virtual void stop();
	/**	
	 * 发送消息 - 发送消息到传输层
	 * @param message 要发送的消息
	 * @return 发送成功返回true，否则返回false
	 */
	virtual bool send(message_ptr message);

	/**
	 * 获取下层传输层对象的智能指针
	 * @return 下层传输层对象的智能指针
	 */
	shared_ptr<Transport> getLower() const { return mLower; }

protected:

	/**
	 * 接收消息 - 接收消息从传输层
	 * @param message 接收到的消息
	 */
	void recv(message_ptr message);
	/**
	 * 改变传输层状态 - 改变传输层状态
	 * @param state 新的传输层状态
	 */
	void changeState(State state);
	/**
	 * 处理接收消息 - 从传输层接收消息
	 * @param message 接收到的消息
	 */
	virtual void incoming(message_ptr message);
	/**
	 * 处理发送消息 - 发送消息到传输层
	 */
	virtual bool outgoing(message_ptr message);

private:
	/**
	 * 初始化令牌 - 初始化令牌
	 */
	const init_token mInitToken = Init::Instance().token();

	/**
	 * 下层传输层对象的智能指针
	 */
	shared_ptr<Transport> mLower;
	/**
	 * 状态变化回调函数
	 */
	synchronized_callback<State> mStateChangeCallback;
	/**
	 * 接收消息回调函数
	 */
	synchronized_callback<message_ptr> mRecvCallback;
	/**
	 * 传输层状态
	 */
	std::atomic<State> mState = State::Disconnected;
};

} // namespace rtc::impl

#endif
