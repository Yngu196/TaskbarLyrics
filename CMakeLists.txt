cmake_minimum_required(VERSION 3.20)

project(MoeKoeTaskbarLyrics
    VERSION 0.3.8
    DESCRIPTION "MoeKoeMusic Windows Taskbar Lyrics Plugin"
    LANGUAGES CXX)

# ⚠️ 注意：不使用 vcpkg 的 CMAKE_TOOLCHAIN_FILE
# 使用 vcpkg 工具链会导致 MSVC 工具集不匹配（14.44 vs 14.42）的链接错误
# 因此直接指定 vcpkg 安装目录下的头文件和库文件路径

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
if(NOT DEFINED VCPKG_INSTALLED_DIR)
    # 优先使用环境变量
    if(DEFINED ENV{VCPKG_INSTALLED_DIR})
        set(VCPKG_INSTALLED_DIR "$ENV{VCPKG_INSTALLED_DIR}")
    elseif(DEFINED ENV{VCPKG_INSTALLATION_ROOT})
        # 尝试 x64-windows 和 x64-windows-142 两个 triplet
        if(EXISTS "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows-142")
            set(VCPKG_INSTALLED_DIR "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows-142")
        else()
            set(VCPKG_INSTALLED_DIR "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows")
        endif()
    else()
        # 自动搜索常见安装路径
        find_path(_VCPKG_CANDIDATE
            NAMES "include" "lib"
            PATHS
                "D:/vcpkg/installed/x64-windows-142"
                "D:/vcpkg/installed/x64-windows"
                "C:/vcpkg/installed/x64-windows-142"
                "C:/vcpkg/installed/x64-windows"
            NO_DEFAULT_PATH)
        if(_VCPKG_CANDIDATE)
            set(VCPKG_INSTALLED_DIR "${_VCPKG_CANDIDATE}")
        endif()
    endif()
endif()

# 如果仍未找到，给出明确提示而非直接报错
if(NOT EXISTS "${VCPKG_INSTALLED_DIR}")
    message(FATAL_ERROR
        "VCPKG_INSTALLED_DIR does not exist: ${VCPKG_INSTALLED_DIR}\n"
        "Please set it via one of:\n"
        "  cmake -DVCPKG_INSTALLED_DIR=<path>\n"
        "  set VCPKG_INSTALLED_DIR=<path>  (environment variable)\n"
        "  set VCPKG_INSTALLATION_ROOT=<vcpkg_root>  (environment variable)")
endif()

message(STATUS "Using VCPKG_INSTALLED_DIR: ${VCPKG_INSTALLED_DIR}")

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

# 3. ixwebsocket —— 使用手动用 MSVC 编译的版本（绕过 vcpkg 的 MSVC 不匹配）
if(NOT DEFINED IXWEBSOCKET_BUILD_DIR)
    if(DEFINED ENV{IXWEBSOCKET_BUILD_DIR})
        set(IXWEBSOCKET_BUILD_DIR "$ENV{IXWEBSOCKET_BUILD_DIR}")
    else()
        set(IXWEBSOCKET_BUILD_DIR "${VCPKG_INSTALLED_DIR}")
    endif()
endif()

set(IXWEBSOCKET_RELEASE_LIB "${IXWEBSOCKET_BUILD_DIR}/Release/ixwebsocket.lib")
set(IXWEBSOCKET_DEBUG_LIB "${IXWEBSOCKET_BUILD_DIR}/Debug/ixwebsocket.lib")

if(EXISTS "${IXWEBSOCKET_RELEASE_LIB}")
    message(STATUS "Using manually built ixwebsocket (MSVC 14.42): ${IXWEBSOCKET_RELEASE_LIB}")
else()
    find_library(IXWEBSOCKET_LIBRARY ixwebsocket PATHS "${VCPKG_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
    set(IXWEBSOCKET_RELEASE_LIB "${IXWEBSOCKET_LIBRARY}")
endif()

if(EXISTS "${IXWEBSOCKET_DEBUG_LIB}")
    message(STATUS "Using manually built ixwebsocket Debug: ${IXWEBSOCKET_DEBUG_LIB}")
else()
    message(WARNING "ixwebsocket Debug lib not found at ${IXWEBSOCKET_DEBUG_LIB}")
    message(WARNING "  -> Forcing Release runtime (/MD) to match ixwebsocket.lib")
    set(IXWEBSOCKET_DEBUG_LIB "${IXWEBSOCKET_RELEASE_LIB}")
    # Debug 库不存在时，强制项目使用 Release 运行时库以匹配 ixwebsocket
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
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

# 5. nlohmann_json（header-only，用 find_path 直接找头文件）
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
    PATHS "${VCPKG_INSTALLED_DIR}/include" NO_DEFAULT_PATH)
if(!NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "nlohmann_json not found in vcpkg installed directory")
endif()
add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${NLOHMANN_JSON_INCLUDE_DIR}")
message(STATUS "Found nlohmann_json: ${NLOHMANN_JSON_INCLUDE_DIR}")

# 6. WebView2 SDK（NuGet 包或系统安装）
# 优先使用 CMake 变量指定的路径（CI 环境），再回退到环境变量和常见安装路径
# NuGet 包路径使用 $ENV{USERPROFILE} 避免硬编码用户名
find_path(WEBVIEW2_INCLUDE_DIR WebView2.h
    HINTS
        "${WEBVIEW2_INCLUDE_DIR}"
    PATHS
        "$ENV{WEBVIEW2_INCLUDE_DIR}"
        "$ENV{LOCALAPPDATA}/Microsoft/Edge WebView2"
        "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2"
        "C:/Program Files (x86)/Microsoft/Edge WebView2"
        "C:/Program Files/Microsoft/Edge WebView2"
    PATH_SUFFIXES "build/native/include"
    NO_DEFAULT_PATH)

# 如果找到 NuGet 包根目录但没有子版本目录，尝试自动查找
if(NOT WEBVIEW2_INCLUDE_DIR AND EXISTS "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2")
    file(GLOB _WEBVIEW2_VERSIONS "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2/*")
    if(_WEBVIEW2_VERSIONS)
        list(SORT _WEBVIEW2_VERSIONS ORDER DESCENDING)
        list(GET _WEBVIEW2_VERSIONS 0 _WEBVIEW2_LATEST)
        if(EXISTS "${_WEBVIEW2_LATEST}/build/native/include/WebView2.h")
            set(WEBVIEW2_INCLUDE_DIR "${_WEBVIEW2_LATEST}/build/native/include")
        endif()
    endif()
endif()

if(WEBVIEW2_INCLUDE_DIR)
    message(STATUS "Found WebView2: ${WEBVIEW2_INCLUDE_DIR}")
else()
    message(WARNING "WebView2 SDK headers not found - SettingsWindow will not compile")
endif()

# 7. WebView2 Loader 库（运行时动态链接）
# CI 通过 -DWEBVIEW2_LOADER_DIR 传入目录，本地回退到常见安装路径
# 注意：NuGet 包中的导入库名为 WebView2Loader.dll.lib（非 WebView2Loader.lib）
find_library(WEBVIEW2_LOADER_LIB
    NAMES WebView2Loader.dll.lib WebView2Loader
    HINTS
        "${WEBVIEW2_LOADER_DIR}"
    PATHS
        "$ENV{WEBVIEW2_LOADER_DIR}"
        "$ENV{LOCALAPPDATA}/Microsoft/Edge WebView2"
        "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2"
        "C:/Program Files (x86)/Microsoft/Edge WebView2"
    PATH_SUFFIXES "build/native/x64"
    NO_DEFAULT_PATH)

# 同样尝试自动查找 NuGet 版本目录
if(NOT WEBVIEW2_LOADER_LIB AND EXISTS "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2")
    file(GLOB _WEBVIEW2_VERSIONS "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2/*")
    if(_WEBVIEW2_VERSIONS)
        list(SORT _WEBVIEW2_VERSIONS ORDER DESCENDING)
        list(GET _WEBVIEW2_VERSIONS 0 _WEBVIEW2_LATEST)
        find_library(WEBVIEW2_LOADER_LIB
            NAMES WebView2Loader.dll.lib WebView2Loader
            PATHS "${_WEBVIEW2_LATEST}/build/native/x64"
            NO_DEFAULT_PATH)
    endif()
endif()

if(WEBVIEW2_LOADER_LIB)
    message(STATUS "Found WebView2Loader.lib: ${WEBVIEW2_LOADER_LIB}")
else()
    message(WARNING "WebView2Loader.lib NOT found - linking will fail if SettingsWindow is used")
endif()

# 源文件
set(SOURCES
    src/main.cpp
    src/logger.cpp
    src/websocket_client.cpp
    src/http_server.cpp
    src/settings_window.cpp
    src/lyrics_parser.cpp
    src/taskbar_window.cpp
    src/renderer.cpp
    src/config.cpp
    src/tray_icon.cpp
    src/process_monitor.cpp
    src/config_dialog.cpp
    src/api_enabler.cpp
    src/native_messaging.cpp
    src/app_icon.rc
)

# 头文件目录
set(HEADERS
    src/logger.h
    src/lyrics_data.h
    src/websocket_client.h
    src/http_server.h
    src/settings_window.h
    src/lyrics_parser.h
    src/taskbar_window.h
    src/renderer.h
    src/config.h
    src/tray_icon.h
    src/process_monitor.h
    src/config_dialog.h
    src/api_enabler.h
)

# 可执行目标
add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${NLOHMANN_JSON_INCLUDE_DIR}
    ${WEBVIEW2_INCLUDE_DIR}
)

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
    ${WEBVIEW2_LOADER_LIB}
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

    # 复制运行时 DLL 到输出目录
    # 1) WebView2Loader.dll（WebView2 运行时依赖）
    if(WEBVIEW2_LOADER_LIB)
        get_filename_component(WEBVIEW2_DLL_DIR "${WEBVIEW2_LOADER_LIB}" DIRECTORY)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WEBVIEW2_DLL_DIR}/WebView2Loader.dll"
                $<TARGET_FILE_DIR:${PROJECT_NAME}>/WebView2Loader.dll
            COMMENT "Copying WebView2Loader.dll to output directory")
    else()
        # 回退：尝试常见安装路径
        find_file(WEBVIEW2_LOADER_DLL WebView2Loader.dll
            PATHS
                "$ENV{WEBVIEW2_LOADER_DIR}"
                "$ENV{LOCALAPPDATA}/Microsoft/Edge WebView2"
                "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2"
                "C:/Program Files (x86)/Microsoft/Edge WebView2"
            PATH_SUFFIXES "build/native/x64"
            NO_DEFAULT_PATH)
        if(WEBVIEW2_LOADER_DLL)
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${WEBVIEW2_LOADER_DLL}"
                    $<TARGET_FILE_DIR:${PROJECT_NAME}>/WebView2Loader.dll
                COMMENT "Copying WebView2Loader.dll to output directory")
        endif()
    endif()

    # 2) z.dll（zlib 运行时依赖）
    find_file(ZLIB_RUNTIME_DLL z.dll PATHS "${VCPKG_INSTALLED_DIR}/bin" NO_DEFAULT_PATH)
    if(ZLIB_RUNTIME_DLL)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ZLIB_RUNTIME_DLL}"
                $<TARGET_FILE_DIR:${PROJECT_NAME}>/z.dll
            COMMENT "Copying z.dll to output directory")
    endif()
endif()