# QUIC DataChannel 示例 CMakeLists.txt - 端到端测试版本

cmake_minimum_required(VERSION 3.16)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 静态链接配置 - 解决库版本兼容性问题
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static-libgcc -static-libstdc++")

# 设置兼容性编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_USE_CXX11_ABI=0")

# 禁用不必要的功能以减少IO
set(CMAKE_DISABLE_SOURCE_CHANGES ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)

# 设置编译缓存目录（减少临时文件IO）
set(CMAKE_CACHEFILE_DIR "${CMAKE_BINARY_DIR}/cache")
file(MAKE_DIRECTORY ${CMAKE_CACHEFILE_DIR})

# 针对低性能主机的优化
set(CMAKE_BUILD_PARALLEL_LEVEL 2)  # 减少并行编译线程数
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pipe -O0")  # 使用O0优化，最快编译
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pipe -O0")

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

# 编译所有目标用于端到端测试
add_executable(simple-quic-example
    simple_example.cpp
)

# 设置预编译头文件（如果可用）
if(USE_PRECOMPILED_HEADERS)
    target_precompile_headers(simple-quic-example PRIVATE ${PRECOMPILED_HEADERS})
endif()

target_link_libraries(simple-quic-example
    datachannel
    lsquic
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)

target_include_directories(simple-quic-example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/lsquic/include
)

# 编译选项
target_compile_options(simple-quic-example PRIVATE
    -pipe                    # 减少临时文件IO
    -O0                      # 无优化，最快编译
    -fno-inline             # 禁用内联，减少编译时间
    -fno-omit-frame-pointer # 保留帧指针，减少编译复杂度
)

# 设置编译缓存
set_target_properties(simple-quic-example PROPERTIES
    COMPILE_FLAGS "-H"
    CXX_COMPILER_LAUNCHER "ccache"
)

# 信令服务器
add_executable(signaling-server
    signaling_server.cpp
)

if(USE_PRECOMPILED_HEADERS)
    target_precompile_headers(signaling-server PRIVATE ${PRECOMPILED_HEADERS})
endif()

target_link_libraries(signaling-server
    datachannel
    nlohmann_json
    pthread
)

target_include_directories(signaling-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_compile_options(signaling-server PRIVATE
    -pipe
    -O0
    -fno-inline
    -fno-omit-frame-pointer
)

set_target_properties(signaling-server PROPERTIES
    COMPILE_FLAGS "-H"
    CXX_COMPILER_LAUNCHER "ccache"
)

# WebRTC客户端
add_executable(webrtc-client
    webrtc_client.cpp
)

if(USE_PRECOMPILED_HEADERS)
    target_precompile_headers(webrtc-client PRIVATE ${PRECOMPILED_HEADERS})
endif()

target_link_libraries(webrtc-client
    datachannel
    lsquic
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json
    pthread
)

target_include_directories(webrtc-client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/lsquic/include
)

target_compile_options(webrtc-client PRIVATE
    -pipe
    -O0
    -fno-inline
    -fno-omit-frame-pointer
)

set_target_properties(webrtc-client PROPERTIES
    COMPILE_FLAGS "-H"
    CXX_COMPILER_LAUNCHER "ccache"
)

# 打印配置信息
message(STATUS "QUIC DataChannel Example Configuration (端到端测试版本):")
message(STATUS "  - Simple Example: simple-quic-example")
message(STATUS "  - Signaling Server: signaling-server")
message(STATUS "  - WebRTC Client: webrtc-client")
if(USE_PRECOMPILED_HEADERS)
    message(STATUS "  - 预编译头文件: 启用")
else()
    message(STATUS "  - 预编译头文件: 禁用（头文件未找到）")
endif()
message(STATUS "  - 编译缓存: ccache")
message(STATUS "  - 优化选项: -pipe -O0 -fno-inline (极简优化)")
message(STATUS "  - 并行编译: 2线程 (减少CPU负载)")
message(STATUS "  - 缓存目录: ${CMAKE_CACHEFILE_DIR}") 