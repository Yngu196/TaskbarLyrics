cmake_minimum_required(VERSION 3.20)
project(MoeKoeTaskbarLyrics
    VERSION 0.2.0
    DESCRIPTION "MoeKoeMusic Windows Taskbar Lyrics Plugin"
    LANGUAGES CXX)

# ⚠️ 注意：不使用 vcpkg 的 CMAKE_TOOLCHAIN_FILE
# 使用 vcpkg 工具链会导致 MSVC 工具集不匹配（14.44 vs 14.42）的链接错误
# 因此我们直接指定 vcpkg 安装目录下的头文件和库文件路径

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 启用 Windows 10/11 SDK
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    set(CMAKE_WIN32_EXECUTABLE ON)
endif()

# 查找依赖
find_package(Threads REQUIRED)

# ---- 手动配置 vcpkg 安装的第三方库（绕过 vcpkg 工具链） ----
set(VCPKG_INSTALLED_DIR "D:/vcpkg/installed/x64-windows-142")

# 1. ZLIB
find_library(ZLIB_LIBRARY_RELEASE NAMES z zlib PATHS "${VCPKG_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
find_path(ZLIB_INCLUDE_DIR zlib.h PATHS "${VCPKG_INSTALLED_DIR}/include" NO_DEFAULT_PATH)
if(ZLIB_LIBRARY_RELEASE AND ZLIB_INCLUDE_DIR)
    add_library(ZLIB::ZLIB STATIC IMPORTED)
    set_target_properties(ZLIB::ZLIB PROPERTIES
        IMPORTED_LOCATION "${ZLIB_LIBRARY_RELEASE}"
        INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_INCLUDE_DIR}")
    message(STATUS "Found ZLIB: ${ZLIB_LIBRARY_RELEASE}")
else()
    message(FATAL_ERROR "ZLIB not found in vcpkg installed directory")
endif()

# 2. mbedtls / mbedx509 / mbedcrypto
find_library(MBEDTLS_LIBRARY mbedtls PATHS "${VCPKG_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
find_library(MBEDX509_LIBRARY mbedx509 PATHS "${VCPKG_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
find_library(MBEDCRYPTO_LIBRARY mbedcrypto PATHS "${VCPKG_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
foreach(lib MBEDTLS MBEDX509 MBEDCRYPTO)
    if(NOT ${lib}_LIBRARY)
        message(FATAL_ERROR "${lib} not found in vcpkg installed directory")
    endif()
endforeach()
message(STATUS "Found mbedtls: ${MBEDTLS_LIBRARY}")
message(STATUS "Found mbedx509: ${MBEDX509_LIBRARY}")
message(STATUS "Found mbedcrypto: ${MBEDCRYPTO_LIBRARY}")

# 3. ixwebsocket —— 使用手动用 MSVC 14.42 编译的版本（绕过 vcpkg 的 MSVC 14.44）
set(IXWEBSOCKET_RELEASE_LIB "D:/ixwebsocket-build/Release/ixwebsocket.lib")
set(IXWEBSOCKET_DEBUG_LIB "D:/ixwebsocket-build/Debug/ixwebsocket.lib")

if(EXISTS "${IXWEBSOCKET_RELEASE_LIB}")
    message(STATUS "Using manually built ixwebsocket (MSVC 14.42): ${IXWEBSOCKET_RELEASE_LIB}")
else()
    find_library(IXWEBSOCKET_LIBRARY ixwebsocket PATHS "${VCPKG_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
    set(IXWEBSOCKET_RELEASE_LIB "${IXWEBSOCKET_LIBRARY}")
endif()

if(EXISTS "${IXWEBSOCKET_DEBUG_LIB}")
    message(STATUS "Using manually built ixwebsocket Debug: ${IXWEBSOCKET_DEBUG_LIB}")
else()
    message(WARNING "ixwebsocket Debug lib not found at ${IXWEBSOCKET_DEBUG_LIB}. Debug builds may fail.")
    set(IXWEBSOCKET_DEBUG_LIB "${IXWEBSOCKET_RELEASE_LIB}")
endif()

find_path(IXWEBSOCKET_INCLUDE_DIR ixwebsocket/IXWebSocket.h
    PATHS "${VCPKG_INSTALLED_DIR}/include" NO_DEFAULT_PATH)
if(NOT IXWEBSOCKET_INCLUDE_DIR)
    message(FATAL_ERROR "ixwebsocket include dir not found")
endif()

add_library(ixwebsocket::ixwebsocket STATIC IMPORTED)
set_target_properties(ixwebsocket::ixwebsocket PROPERTIES
    IMPORTED_LOCATION_RELEASE "${IXWEBSOCKET_RELEASE_LIB}"
    IMPORTED_LOCATION_DEBUG "${IXWEBSOCKET_DEBUG_LIB}"
    IMPORTED_LOCATION "${IXWEBSOCKET_RELEASE_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${VCPKG_INSTALLED_DIR}/include"
    INTERFACE_COMPILE_DEFINITIONS "IXWEBSOCKET_USE_TLS;IXWEBSOCKET_USE_MBED_TLS;IXWEBSOCKET_USE_ZLIB"
    INTERFACE_LINK_LIBRARIES
        "${MBEDTLS_LIBRARY};${MBEDX509_LIBRARY};${MBEDCRYPTO_LIBRARY};ZLIB::ZLIB;wsock32;ws2_32;shlwapi;Crypt32"
)

# 4. nlohmann_json（header-only，用 find_path 直接找头文件）
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
    PATHS "${VCPKG_INSTALLED_DIR}/include" NO_DEFAULT_PATH)
if(NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "nlohmann_json not found in vcpkg installed directory")
endif()
add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${NLOHMANN_JSON_INCLUDE_DIR}")
message(STATUS "Found nlohmann_json: ${NLOHMANN_JSON_INCLUDE_DIR}")

# 源文件
set(SOURCES
    src/main.cpp
    src/websocket_client.cpp
    src/http_server.cpp
    src/lyrics_parser.cpp
    src/taskbar_window.cpp
    src/renderer.cpp
    src/config.cpp
    src/tray_icon.cpp
    src/process_monitor.cpp
    src/config_dialog.cpp
    src/native_messaging.cpp
)

# 头文件目录
set(HEADERS
    src/lyrics_data.h
    src/websocket_client.h
    src/http_server.h
    src/lyrics_parser.h
    src/taskbar_window.h
    src/renderer.h
    src/config.h
    src/tray_icon.h
    src/process_monitor.h
    src/config_dialog.h
    src/native_messaging.h
)

# 可执行目标
add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE src)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ixwebsocket::ixwebsocket
    nlohmann_json::nlohmann_json
    Threads::Threads
    d2d1
    dwrite
    windowscodecs
    gdi32
    shell32
    user32
    comctl32
    advapi32
    ole32
    oleaut32
    comdlg32
    shlwapi
    userenv
    bcrypt
)

# 资源
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico)
    target_sources(${PROJECT_NAME} PRIVATE resources/icon.ico)
endif()

# 安装规则
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
install(DIRECTORY resources/ DESTINATION share/MoeKoeTaskbarLyrics OPTIONAL)

# 后构建事件:复制资源到输出目录
if(MSVC)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/resources)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/resources
                $<TARGET_FILE_DIR:${PROJECT_NAME}>/resources
            COMMENT "Copying resources to output directory")
    endif()

    # 复制 EXE 到 MoeKoeMusic 插件目录
    set(PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../MoeKoeMusic/plugins/extensions/moeKoe-taskbar-lyrics")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${PROJECT_NAME}>
            "${PLUGIN_DIR}/MoeKoeTaskbarLyrics.exe"
        COMMENT "Copying EXE to MoeKoeMusic plugin directory")
endif()