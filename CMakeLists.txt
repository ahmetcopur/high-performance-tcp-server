cmake_minimum_required(VERSION 3.20...3.31)
project(tcpserver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
        "Debug, Release, or RelWithDebInfo" FORCE)
endif()

option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ENABLE_ASAN "Build with AddressSanitizer and UBSan" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR
        "ENABLE_ASAN and ENABLE_TSAN cannot be combined. Each runtime "
        "replaces malloc and reserves its own shadow memory, so a binary "
        "can only carry one of them.")
endif()

# Sanitizer flags are global so that GoogleTest is instrumented as well.
# UBSan only prints by default, so make it abort like ASan does.
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined
                        -fno-sanitize-recover=undefined
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)

    # On Linux, UBSan's vptr check opens a pipe to probe whether the
    # object's memory is readable. With no descriptors free the probe
    # fails and the check reports an error that is not there, which kills
    # the server in the descriptor exhaustion test.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_compile_options(-fno-sanitize=vptr)
    endif()
elseif(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# Warnings are attached to our own targets only, not to GoogleTest.
add_library(tcpserver_warnings INTERFACE)
target_compile_options(tcpserver_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
if(WARNINGS_AS_ERRORS)
    target_compile_options(tcpserver_warnings INTERFACE -Werror)
endif()

add_library(tcpserver STATIC
    src/Buffer.cpp
    src/Config.cpp
    src/Connection.cpp
    src/EventLoop.cpp
    src/FileDescriptor.cpp
    src/PollPoller.cpp
    src/Protocol.cpp
    src/RequestHandler.cpp
    src/TcpServer.cpp
    src/ThreadPool.cpp
    src/TimerQueue.cpp
    src/Waker.cpp)
# epoll exists only on Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(tcpserver PRIVATE src/EpollPoller.cpp)
endif()

target_include_directories(tcpserver PUBLIC include)
target_link_libraries(tcpserver
    PUBLIC Threads::Threads
    PRIVATE tcpserver_warnings)

add_executable(server src/main.cpp)
target_link_libraries(server PRIVATE tcpserver tcpserver_warnings)

include(CTest)
if(BUILD_TESTING)
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    include(GoogleTest)

    foreach(name
            test_buffer
            test_config
            test_connection
            test_file_descriptor
            test_histogram
            test_poller
            test_protocol
            test_request_handler
            test_threadpool
            test_timer_queue
            test_waker)
        add_executable(${name} tests/${name}.cpp)
        target_link_libraries(${name} PRIVATE
            tcpserver tcpserver_warnings GTest::gtest_main)
        # Discovery runs the binary to list its tests. Under a sanitizer
        # on a busy machine that can take longer than the default 5 s.
        gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60)
    endforeach()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    # The histogram belongs to the load generator, not to the server.
    target_include_directories(test_histogram PRIVATE benchmarks/loadgen)

    # The whole integration suite once per poller the platform has.
    set(pollers poll)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND pollers epoll)
    endif()

    foreach(poller ${pollers})
        add_test(NAME integration_${poller}
            COMMAND Python3::Interpreter
                    ${CMAKE_SOURCE_DIR}/tests/test_server.py
                    $<TARGET_FILE:server>
                    --poller ${poller})

        # The tests stop the server with SIGKILL, so a TSan report would
        # never reach the exit code. Fail on the report text instead.
        # Both runs use the same port, so they must not overlap.
        set_tests_properties(integration_${poller} PROPERTIES
            TIMEOUT 300
            RESOURCE_LOCK tcp_port_18080
            FAIL_REGULAR_EXPRESSION
            "ThreadSanitizer|AddressSanitizer|LeakSanitizer|runtime error:")
    endforeach()
endif()
