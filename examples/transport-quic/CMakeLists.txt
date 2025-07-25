add_executable(transport-quic-demo main.cpp)

# 编译优化：减少IOPS压力
target_compile_options(transport-quic-demo PRIVATE -pipe)
target_compile_options(transport-quic-demo PRIVATE -O1)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVENT REQUIRED libevent)
find_package(ZLIB REQUIRED)

include_directories(${LIBEVENT_INCLUDE_DIRS})

# 直接链接源码target lsquic

target_link_libraries(transport-quic-demo
    PRIVATE
    lsquic
    /home/kratos/boringssl/libssl.a
    /home/kratos/boringssl/libcrypto.a
    ZLIB::ZLIB
    pthread
    ${LIBEVENT_LIBRARIES}
)
target_include_directories(transport-quic-demo PRIVATE /home/kratos/boringssl/include)