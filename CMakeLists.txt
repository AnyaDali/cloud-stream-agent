cmake_minimum_required(VERSION 3.25)

project(cloud_stream_agent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(Threads REQUIRED)
find_path(ASIO_INCLUDE_DIR NAMES asio.hpp REQUIRED)

add_library(cloud_stream_asio INTERFACE)
target_include_directories(cloud_stream_asio INTERFACE "${ASIO_INCLUDE_DIR}")
target_compile_definitions(
    cloud_stream_asio
    INTERFACE
        ASIO_STANDALONE
        ASIO_NO_DEPRECATED
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
)
target_link_libraries(cloud_stream_asio INTERFACE Threads::Threads)
if(WIN32)
    target_link_libraries(cloud_stream_asio INTERFACE ws2_32 mswsock)
endif()

function(cloud_stream_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

add_library(cloud_stream_protocol
    src/protocol/message_header.cpp
    src/protocol/stream_payloads.cpp
)
target_include_directories(cloud_stream_protocol PUBLIC src)
cloud_stream_enable_warnings(cloud_stream_protocol)

add_library(cloud_stream_net src/net/tcp_message_stream.cpp)
target_include_directories(cloud_stream_net PUBLIC src)
target_link_libraries(cloud_stream_net PUBLIC cloud_stream_protocol cloud_stream_asio)
cloud_stream_enable_warnings(cloud_stream_net)

add_executable(stream-server src/server/main.cpp)
target_link_libraries(stream-server PRIVATE cloud_stream_net)
cloud_stream_enable_warnings(stream-server)

add_executable(stream-client src/client/main.cpp)
target_link_libraries(stream-client PRIVATE cloud_stream_net)
cloud_stream_enable_warnings(stream-client)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_executable(protocol-header-test tests/protocol/message_header_test.cpp)
    target_link_libraries(protocol-header-test PRIVATE cloud_stream_protocol)
    cloud_stream_enable_warnings(protocol-header-test)
    add_test(NAME protocol-header-test COMMAND protocol-header-test)

    add_test(
        NAME synthetic-stream-test
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/synthetic_stream_test.py
            $<TARGET_FILE:stream-server>
            $<TARGET_FILE:stream-client>
    )
    set_tests_properties(synthetic-stream-test PROPERTIES TIMEOUT 20)
endif()
