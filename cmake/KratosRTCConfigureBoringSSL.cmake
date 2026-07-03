include(FetchContent)

set(KRATOSRTC_BORINGSSL_GIT_REPOSITORY
    "https://github.com/google/boringssl.git"
    CACHE STRING "BoringSSL git repository used when fetching automatically")
set(KRATOSRTC_BORINGSSL_GIT_TAG
    "9fc1c33e9c21439ce5f87855a6591a9324e569fd"
    CACHE STRING "Pinned BoringSSL commit used for reproducible QUIC builds")
option(KRATOSRTC_FETCH_BORINGSSL
    "Fetch the pinned BoringSSL revision automatically when ENABLE_QUIC is ON" ON)
option(KRATOSRTC_BORINGSSL_BUILD_FROM_SOURCE
    "Build BORINGSSL_DIR with add_subdirectory when it contains BoringSSL sources" ON)
set(BORINGSSL_DIR "" CACHE PATH "Path to a local BoringSSL source or build directory")
set(BORINGSSL_LIB "" CACHE PATH "Path to prebuilt BoringSSL libraries")

function(_kratosrtc_set_openssl_target target_name link_target include_dir)
    if(TARGET "${target_name}")
        return()
    endif()

    add_library("${target_name}" INTERFACE IMPORTED)
    set_target_properties("${target_name}" PROPERTIES
        INTERFACE_LINK_LIBRARIES "${link_target}"
        INTERFACE_INCLUDE_DIRECTORIES "${include_dir}")
endfunction()

function(_kratosrtc_find_boringssl_library output_var library_name)
    set(_search_roots
        "${BORINGSSL_LIB}"
        "${BORINGSSL_DIR}"
        "${BORINGSSL_DIR}/${library_name}"
        "${BORINGSSL_DIR}/build"
        "${BORINGSSL_DIR}/build/${library_name}"
        "${BORINGSSL_DIR}/build/ssl"
        "${BORINGSSL_DIR}/build/crypto")

    find_library(_library
        NAMES "${library_name}" "lib${library_name}.a"
        PATHS ${_search_roots}
        PATH_SUFFIXES "" Debug Release RelWithDebInfo MinSizeRel
        NO_DEFAULT_PATH)

    if(NOT _library)
        message(FATAL_ERROR
            "Could not find BoringSSL ${library_name} library. "
            "Provide BORINGSSL_DIR, BORINGSSL_LIB, or enable KRATOSRTC_FETCH_BORINGSSL.")
    endif()

    set("${output_var}" "${_library}" PARENT_SCOPE)
endfunction()

function(_kratosrtc_use_boringssl_targets include_dir ssl_target crypto_target)
    if(NOT EXISTS "${include_dir}/openssl/ssl.h")
        message(FATAL_ERROR "BoringSSL include directory is invalid: ${include_dir}")
    endif()

    _kratosrtc_set_openssl_target(OpenSSL::SSL "${ssl_target}" "${include_dir}")
    _kratosrtc_set_openssl_target(OpenSSL::Crypto "${crypto_target}" "${include_dir}")

    set(BORINGSSL_INCLUDE "${include_dir}" CACHE PATH "BoringSSL include directory" FORCE)
    set(BORINGSSL_INCLUDE_DIR "${include_dir}" CACHE PATH "BoringSSL include directory" FORCE)
    set(BORINGSSL_LIB_ssl "${ssl_target}" CACHE STRING "BoringSSL ssl target or library" FORCE)
    set(BORINGSSL_LIB_crypto "${crypto_target}" CACHE STRING "BoringSSL crypto target or library" FORCE)

    # libdatachannel still consumes OpenSSL-compatible target names. In QUIC builds these are
    # intentionally backed by BoringSSL so that lsquic and DTLS do not mix crypto providers.
    set(OPENSSL_INCLUDE_DIR "${include_dir}" CACHE PATH "OpenSSL-compatible include directory" FORCE)
    set(OPENSSL_SSL_LIBRARY "${ssl_target}" CACHE STRING "OpenSSL-compatible SSL target" FORCE)
    set(OPENSSL_CRYPTO_LIBRARY "${crypto_target}" CACHE STRING "OpenSSL-compatible Crypto target" FORCE)
endfunction()

function(_kratosrtc_import_prebuilt_boringssl include_dir)
    _kratosrtc_find_boringssl_library(_ssl_library ssl)
    _kratosrtc_find_boringssl_library(_crypto_library crypto)

    if(NOT TARGET BoringSSL::SSL)
        add_library(BoringSSL::SSL UNKNOWN IMPORTED)
        set_target_properties(BoringSSL::SSL PROPERTIES
            IMPORTED_LOCATION "${_ssl_library}"
            INTERFACE_INCLUDE_DIRECTORIES "${include_dir}")
    endif()

    if(NOT TARGET BoringSSL::Crypto)
        add_library(BoringSSL::Crypto UNKNOWN IMPORTED)
        set_target_properties(BoringSSL::Crypto PROPERTIES
            IMPORTED_LOCATION "${_crypto_library}"
            INTERFACE_INCLUDE_DIRECTORIES "${include_dir}")
    endif()

    _kratosrtc_use_boringssl_targets("${include_dir}" BoringSSL::SSL BoringSSL::Crypto)
endfunction()

function(kratosrtc_configure_boringssl)
    if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
        message(STATUS "Using existing OpenSSL-compatible targets for QUIC crypto")
        return()
    endif()

    if(BORINGSSL_DIR)
        set(_include_dir "${BORINGSSL_DIR}/include")
        if(KRATOSRTC_BORINGSSL_BUILD_FROM_SOURCE AND EXISTS "${BORINGSSL_DIR}/CMakeLists.txt")
            message(STATUS "Using local BoringSSL source: ${BORINGSSL_DIR}")
            add_subdirectory("${BORINGSSL_DIR}" "${CMAKE_BINARY_DIR}/_deps/boringssl-local-build" EXCLUDE_FROM_ALL)
            if(NOT TARGET ssl OR NOT TARGET crypto)
                message(FATAL_ERROR "Local BoringSSL source did not define ssl and crypto targets")
            endif()
            _kratosrtc_use_boringssl_targets("${_include_dir}" ssl crypto)
        else()
            message(STATUS "Using prebuilt BoringSSL from: ${BORINGSSL_DIR}")
            _kratosrtc_import_prebuilt_boringssl("${_include_dir}")
        endif()
        return()
    endif()

    if(NOT KRATOSRTC_FETCH_BORINGSSL)
        message(FATAL_ERROR
            "ENABLE_QUIC requires BoringSSL. Set BORINGSSL_DIR or enable KRATOSRTC_FETCH_BORINGSSL.")
    endif()

    message(STATUS "Fetching pinned BoringSSL revision: ${KRATOSRTC_BORINGSSL_GIT_TAG}")
    FetchContent_Declare(
        boringssl
        GIT_REPOSITORY "${KRATOSRTC_BORINGSSL_GIT_REPOSITORY}"
        GIT_TAG        "${KRATOSRTC_BORINGSSL_GIT_TAG}"
        GIT_SHALLOW    FALSE)
    FetchContent_MakeAvailable(boringssl)

    if(NOT TARGET ssl OR NOT TARGET crypto)
        message(FATAL_ERROR "Fetched BoringSSL did not define ssl and crypto targets")
    endif()

    _kratosrtc_use_boringssl_targets("${boringssl_SOURCE_DIR}/include" ssl crypto)
endfunction()
