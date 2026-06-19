cmake_minimum_required(VERSION 3.14)
project(TinyWebServer VERSION 1.0.0 LANGUAGES CXX)

# ============================================================
#  C++ 标准
# ============================================================
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
#  编译选项
# ============================================================
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # 生成 compile_commands.json (LSP/IDE)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -Wall -Wextra -Wpedantic)
else()
    add_compile_options(-O3 -DNDEBUG)
endif()

# ============================================================
#  平台检测
# ============================================================
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_definitions(-DCOMPILE_LINUX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_definitions(-DCOMPILE_MACOS)
endif()

# ============================================================
#  MySQL 依赖 (find_package 优先，pkg-config 回退)
# ============================================================
set(MYSQL_FOUND FALSE)

find_package(MySQL)
if(NOT MySQL_FOUND)
    # pkg-config 回退
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MYSQL mysqlclient)
    endif()

    if(NOT MYSQL_FOUND)
        # 手动搜索
        find_path(MYSQL_INCLUDE_DIR mysql/mysql.h
            PATHS /usr/include /usr/local/include /usr/local/mysql/include
        )
        find_library(MYSQL_LIBRARY mysqlclient
            PATHS /usr/lib /usr/local/lib /usr/lib64 /usr/local/mysql/lib
        )
        if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
            set(MYSQL_FOUND TRUE)
            set(MYSQL_INCLUDE_DIRS ${MYSQL_INCLUDE_DIR})
            set(MYSQL_LIBRARIES ${MYSQL_LIBRARY})
        endif()
    endif()
endif()

if(NOT MYSQL_FOUND)
    message(WARNING "MySQL not found — building without MySQL support")
else()
    message(STATUS "Found MySQL: ${MYSQL_INCLUDE_DIRS}")
endif()

# ============================================================
#  源文件
# ============================================================
set(SOURCES
    main.cpp
    code/Server/WebServer.cpp
    code/Server/Epoller.cpp
    code/Http/HttpConn.cpp
    code/Http/HttpRequest.cpp
    code/Http/HttpResponse.cpp
    code/Buffer/Buffer.cpp
    code/Timer/HeapTimer.cpp
    code/Pool/SqlConnPool.cpp
    code/Log/Log.cpp
)

set(HEADERS
    code/Server/WebServer.h
    code/Server/Epoller.h
    code/Http/HttpConn.h
    code/Http/HttpRequest.h
    code/Http/HttpResponse.h
    code/Buffer/Buffer.h
    code/Timer/HeapTimer.h
    code/Pool/ThreadPool.h
    code/Pool/SqlConnPool.h
    code/Pool/SqlConnRAII.h
    code/Log/Log.h
    code/Log/Blockqueue.h
)

# ============================================================
#  可执行文件
# ============================================================
add_executable(TinyWebServer ${SOURCES} ${HEADERS})

target_include_directories(TinyWebServer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/code
)

# pthread (Linux 上一般隐式链接，显式声明更安全)
find_package(Threads REQUIRED)
target_link_libraries(TinyWebServer PRIVATE Threads::Threads)

# MySQL
if(MYSQL_FOUND)
    target_include_directories(TinyWebServer PRIVATE ${MYSQL_INCLUDE_DIRS})
    target_link_libraries(TinyWebServer PRIVATE ${MYSQL_LIBRARIES})
    target_compile_definitions(TinyWebServer PRIVATE HAS_MYSQL)
endif()

# ============================================================
#  安装
# ============================================================
install(TARGETS TinyWebServer DESTINATION bin)

# ============================================================
#  可选: Google Test 单元测试
#  用法: cmake -DBUILD_TESTS=ON ..
# ============================================================
option(BUILD_TESTS "Build unit tests" OFF)

if(BUILD_TESTS)
    enable_testing()

    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        # 自动下载 GTest
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
        )
        FetchContent_MakeAvailable(googletest)
    endif()

    set(TEST_SOURCES_BASE
        tests/test_buffer.cpp
        tests/test_heaptimer.cpp
        tests/test_threadpool.cpp
    )

    set(TEST_SOURCES_MYSQL
        tests/test_httprequest.cpp
    )

    add_executable(TinyWebServer_test ${TEST_SOURCES_BASE}
        code/Buffer/Buffer.cpp
        code/Timer/HeapTimer.cpp
        code/Log/Log.cpp
    )

    target_include_directories(TinyWebServer_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/code
    )

    target_link_libraries(TinyWebServer_test PRIVATE
        Threads::Threads
        GTest::gtest_main
    )

    # HttpRequest 测试依赖 MySQL（源文件包含 mysql.h）
    if(MYSQL_FOUND)
        target_sources(TinyWebServer_test PRIVATE
            ${TEST_SOURCES_MYSQL}
            code/Http/HttpRequest.cpp
        )
        target_include_directories(TinyWebServer_test PRIVATE ${MYSQL_INCLUDE_DIRS})
        target_link_libraries(TinyWebServer_test PRIVATE ${MYSQL_LIBRARIES})
        target_compile_definitions(TinyWebServer_test PRIVATE HAS_MYSQL)
        message(STATUS "  HttpRequest tests: ENABLED (with MySQL)")
    else()
        message(STATUS "  HttpRequest tests: DISABLED (MySQL not found)")
    endif()

    include(GoogleTest)
    gtest_discover_tests(TinyWebServer_test)
endif()

# ============================================================
#  性能测试目标
# ============================================================
add_custom_target(benchmark
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/bench/run_bench.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running benchmark suite..."
)

# ============================================================
#  clang-format (可选)
# ============================================================
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    file(GLOB_RECURSE ALL_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/code/**/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/code/**/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    )
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i ${ALL_SRC}
        COMMENT "Formatting source code..."
    )
endif()

message(STATUS "=== TinyWebServer ${PROJECT_VERSION} configured ===")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard:   ${CMAKE_CXX_STANDARD}")
message(STATUS "  MySQL support:  ${MYSQL_FOUND}")
