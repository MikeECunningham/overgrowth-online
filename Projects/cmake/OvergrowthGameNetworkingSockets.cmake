# The checked-in GameNetworkingSockets SDK contains Windows binaries only.
# Build the same release from source for standalone Linux networking.
include_guard(GLOBAL)

function(overgrowth_configure_gns)
    if(TARGET Overgrowth::GameNetworkingSockets)
        return()
    endif()

    if(CMAKE_VERSION VERSION_LESS 3.14)
        message(FATAL_ERROR "Building GameNetworkingSockets requires CMake 3.14 or newer")
    endif()

    set(OVERGROWTH_GNS_SOURCE_DIR "" CACHE PATH
        "Optional existing GameNetworkingSockets 1.4.0 source tree (avoids downloading it)")
    set(OVERGROWTH_DEPENDENCY_ROOT "${CMAKE_BINARY_DIR}/Dependencies/sysroot" CACHE PATH
        "Optional extracted Linux development packages, containing usr/include and usr/lib")

    # A user-local SDK is useful on machines without administrative access.
    # Prefer explicitly selected Protobuf paths, then this SDK, then the system.
    set(_sdk "${OVERGROWTH_DEPENDENCY_ROOT}")
    if(EXISTS "${_sdk}/usr/include/google/protobuf/service.h")
        if(NOT Protobuf_INCLUDE_DIR)
            set(Protobuf_INCLUDE_DIR "${_sdk}/usr/include" CACHE PATH "Protobuf headers" FORCE)
        endif()
        find_library(_gns_protobuf_library NAMES protobuf
            PATHS "${_sdk}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}" "${_sdk}/usr/lib" "${_sdk}/usr/lib64"
            NO_DEFAULT_PATH)
        if(NOT Protobuf_LIBRARY AND _gns_protobuf_library)
            set(Protobuf_LIBRARY "${_gns_protobuf_library}" CACHE FILEPATH "Protobuf library" FORCE)
        endif()
        if(NOT Protobuf_PROTOC_EXECUTABLE AND EXISTS "${_sdk}/usr/bin/protoc")
            # Extracted protoc also needs the matching libprotoc shared library.
            # Keep this loader path private to the compiler process.
            get_filename_component(_protobuf_libdir "${_gns_protobuf_library}" DIRECTORY)
            set(_protoc_wrapper "${CMAKE_CURRENT_BINARY_DIR}/gns-tools/protoc")
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gns-tools")
            file(WRITE "${_protoc_wrapper}"
                "#!/bin/sh\nLD_LIBRARY_PATH=\"${_protobuf_libdir}:$LD_LIBRARY_PATH\" exec \"${_sdk}/usr/bin/protoc\" \"$@\"\n")
            execute_process(COMMAND chmod +x "${_protoc_wrapper}" RESULT_VARIABLE _chmod_result)
            if(NOT _chmod_result EQUAL 0)
                message(FATAL_ERROR "Cannot make the extracted Protobuf compiler executable")
            endif()
            set(Protobuf_PROTOC_EXECUTABLE "${_protoc_wrapper}" CACHE FILEPATH "Protobuf compiler" FORCE)
        endif()
    endif()
    find_package(Protobuf REQUIRED)
    find_package(OpenSSL REQUIRED)

    # Keep upstream's generic option names local to this function/subdirectory.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_STATIC_LIB ON)
    set(BUILD_SHARED_LIB OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_TESTS OFF)
    set(BUILD_TOOLS OFF)
    set(USE_STEAMWEBRTC OFF)
    set(USE_CRYPTO OpenSSL)
    set(USE_CRYPTO25519 OpenSSL)

    if(OVERGROWTH_GNS_SOURCE_DIR)
        if(NOT EXISTS "${OVERGROWTH_GNS_SOURCE_DIR}/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_connections.cpp")
            message(FATAL_ERROR "OVERGROWTH_GNS_SOURCE_DIR must point to a GameNetworkingSockets source tree")
        endif()
        set(_gns_source "${OVERGROWTH_GNS_SOURCE_DIR}")
        add_subdirectory("${_gns_source}" "${CMAKE_CURRENT_BINARY_DIR}/gns" EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        # Valve's v1.4.0 tag, matching Libraries/GameNetworkingSockets-1.4.0.
        FetchContent_Declare(overgrowth_gns
            URL "https://codeload.github.com/ValveSoftware/GameNetworkingSockets/tar.gz/c26ff7cb0780a7fc4fec86a94675cbf14c13b8ff"
            URL_HASH SHA256=c0cfd5812c414446a080a632e98d4c5c94c4da8031a4972c8e84610fc86caf27)
        FetchContent_MakeAvailable(overgrowth_gns)
        set(_gns_source "${overgrowth_gns_SOURCE_DIR}")
    endif()

    # v1.4.0 uses GNU's empty-variadic-argument comma elision in its lock
    # assertions. Only this dependency needs GNU language extensions; the game
    # keeps the strict C++ standard selected by the parent project.
    set_property(TARGET GameNetworkingSockets_s PROPERTY CXX_EXTENSIONS ON)

    add_library(overgrowth_gns INTERFACE)
    add_library(Overgrowth::GameNetworkingSockets ALIAS overgrowth_gns)
    target_link_libraries(overgrowth_gns INTERFACE GameNetworkingSockets_s)
    # The game uses <steamnetworkingsockets.h>; upstream exposes <steam/...>.
    target_include_directories(overgrowth_gns INTERFACE "${_gns_source}/include/steam")
    message(STATUS "Standalone networking: GameNetworkingSockets 1.4.0 (Linux source build)")
endfunction()
