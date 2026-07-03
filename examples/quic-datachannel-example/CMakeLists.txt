# QUIC DataChannel 示例 CMakeLists.txt - 端到端测试版本

cmake_minimum_required(VERSION 3.16)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置兼容性编译选项 - 使用 C++11 ABI（与库保持一致）
# 注意：库使用默认的 C++11 ABI，所以这里不应该禁用
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_USE_CXX11_ABI=0")

# 禁用不必要的功能以减少IO
set(CMAKE_DISABLE_SOURCE_CHANGES ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)

# 设置编译缓存目录（减少临时文件IO）
set(KRATOSRTC_EXAMPLE_CACHE_DIR "${CMAKE_BINARY_DIR}/cache")
file(MAKE_DIRECTORY ${KRATOSRTC_EXAMPLE_CACHE_DIR})

# 检查头文件路径是否存在
# 从examples/quic-datachannel-example目录到项目根目录的路径
set(HEADER_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
set(RTC_HEADER "${HEADER_BASE_DIR}/rtc/rtc.hpp")
set(PEERCONNECTION_HEADER "${HEADER_BASE_DIR}/rtc/peerconnection.hpp")
set(DATACHANNEL_HEADER "${HEADER_BASE_DIR}/rtc/datachannel.hpp")

# 检查头文件是否存在
if(EXISTS ${RTC_HEADER} AND EXISTS ${PEERCONNECTION_HEADER} AND EXISTS ${DATACHANNEL_HEADER})
    message(STATUS "找到头文件，启用预编译头文件")
    message(STATUS "头文件路径: ${HEADER_BASE_DIR}")
    set(PRECOMPILED_HEADERS
        ${RTC_HEADER}
        ${PEERCONNECTION_HEADER}
        ${DATACHANNEL_HEADER}
    )
    set(USE_PRECOMPILED_HEADERS TRUE)
else()
    message(WARNING "未找到预编译头文件，禁用预编译头文件功能")
    message(STATUS "期望的头文件路径:")
    message(STATUS "  ${RTC_HEADER}")
    message(STATUS "  ${PEERCONNECTION_HEADER}")
    message(STATUS "  ${DATACHANNEL_HEADER}")
    message(STATUS "当前目录: ${CMAKE_CURRENT_SOURCE_DIR}")
    message(STATUS "头文件基础目录: ${HEADER_BASE_DIR}")
    set(USE_PRECOMPILED_HEADERS FALSE)
endif()



# 信令服务器
add_executable(signaling-server
    signaling_server.cpp
)

if(USE_PRECOMPILED_HEADERS)
    target_precompile_headers(signaling-server PRIVATE ${PRECOMPILED_HEADERS})
endif()

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
if(NOT TARGET OpenSSL::SSL OR NOT TARGET OpenSSL::Crypto)
    find_package(OpenSSL REQUIRED)
endif()

target_link_libraries(signaling-server
    PRIVATE
    datachannel
    nlohmann_json::nlohmann_json
    Threads::Threads
)

target_include_directories(signaling-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

# WebRTC客户端
add_executable(webrtc-client
    webrtc_client.cpp
    utils/SignalingClient.cpp
    utils/TrafficStats.cpp
)

if(USE_PRECOMPILED_HEADERS)
    target_precompile_headers(webrtc-client PRIVATE ${PRECOMPILED_HEADERS})
endif()

# 确保 datachannel 库被构建
add_dependencies(webrtc-client datachannel)

target_link_libraries(webrtc-client
    PRIVATE
    datachannel
    lsquic
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    nlohmann_json::nlohmann_json
    Threads::Threads
)

target_include_directories(webrtc-client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/lsquic/include
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)

# 打印配置信息
message(STATUS "QUIC DataChannel Example Configuration (端到端测试版本):")
message(STATUS "  - Signaling Server: signaling-server")
message(STATUS "  - WebRTC Client: webrtc-client")
if(USE_PRECOMPILED_HEADERS)
    message(STATUS "  - 预编译头文件: 启用")
else()
    message(STATUS "  - 预编译头文件: 禁用（头文件未找到）")
endif()
message(STATUS "  - 缓存目录: ${KRATOSRTC_EXAMPLE_CACHE_DIR}")
