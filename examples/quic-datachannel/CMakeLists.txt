cmake_minimum_required(VERSION 3.16)

project(quic-datachannel-example)

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)

# Find libdatachannel
find_package(LibDataChannel REQUIRED)

# Find lsquic
find_package(PkgConfig REQUIRED)
pkg_check_modules(LSQUIC REQUIRED liblsquic)

# Create executable
add_executable(quic-datachannel main.cpp)

# Link libraries
target_link_libraries(quic-datachannel 
    LibDataChannel::LibDataChannel
    OpenSSL::SSL
    OpenSSL::Crypto
    ${LSQUIC_LIBRARIES}
)

# Include directories
target_include_directories(quic-datachannel PRIVATE
    ${LSQUIC_INCLUDE_DIRS}
)

# Compiler flags
target_compile_options(quic-datachannel PRIVATE
    ${LSQUIC_CFLAGS_OTHER}
)

# Linker flags
target_link_options(quic-datachannel PRIVATE
    ${LSQUIC_LDFLAGS}
) 