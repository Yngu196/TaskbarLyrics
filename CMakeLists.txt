cmake_minimum_required(VERSION 3.20)

project(MoeKoeTaskbarLyrics
    VERSION 1.0.1
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
    # /await /winrt 已移至主目标 target_compile_options，避免 FetchContent 引入的
    # Catch2 等子项目继承已废弃的 /await 标志（MSVC 14.51+ 会报 C2338 static_assert）
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX UNICODE _UNICODE)
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
        set(_VCPKG_BASE "$ENV{VCPKG_INSTALLATION_ROOT}")
        foreach(_TRIPLET x64-windows-142 x64-windows)
            if(EXISTS "${_VCPKG_BASE}/installed/${_TRIPLET}/include" AND EXISTS "${_VCPKG_BASE}/installed/${_TRIPLET}/lib")
                set(VCPKG_INSTALLED_DIR "${_VCPKG_BASE}/installed/${_TRIPLET}")
                break()
            endif()
        endforeach()
    elseif(DEFINED ENV{VCPKG_ROOT})
        set(_VCPKG_BASE "$ENV{VCPKG_ROOT}")
        foreach(_TRIPLET x64-windows-142 x64-windows)
            if(EXISTS "${_VCPKG_BASE}/installed/${_TRIPLET}/include" AND EXISTS "${_VCPKG_BASE}/installed/${_TRIPLET}/lib")
                set(VCPKG_INSTALLED_DIR "${_VCPKG_BASE}/installed/${_TRIPLET}")
                break()
            endif()
        endforeach()
    endif()

    # 硬编码路径作为最后 fallback（非标准环境需自行配置环境变量）
    if(NOT DEFINED VCPKG_INSTALLED_DIR)
        foreach(_CANDIDATE
                "D:/vcpkg/installed/x64-windows-142"
                "D:/vcpkg/installed/x64-windows"
                "C:/vcpkg/installed/x64-windows-142"
                "C:/vcpkg/installed/x64-windows")
            if(EXISTS "${_CANDIDATE}/include" AND EXISTS "${_CANDIDATE}/lib")
                set(VCPKG_INSTALLED_DIR "${_CANDIDATE}")
                break()
            endif()
        endforeach()
    endif()
endif()

# 如果仍未找到，给出明确提示而非直接报错
if(NOT EXISTS "${VCPKG_INSTALLED_DIR}")
    message(FATAL_ERROR
        "VCPKG_INSTALLED_DIR does not exist: ${VCPKG_INSTALLED_DIR}\n"
        "Please set it via one of:\n"
        "  cmake -DVCPKG_INSTALLED_DIR=<path>\n"
        "  set VCPKG_INSTALLED_DIR=<path>        (environment variable)\n"
        "  set VCPKG_INSTALLATION_ROOT=<root>    (environment variable)\n"
        "  set VCPKG_ROOT=<root>                 (environment variable)")
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

# 3. ixwebsocket —— 使用 add_subdirectory 从源码编译（确保编译器版本一致）
# 从环境变量推导 vcpkg buildtrees 路径
set(_IXWS_VCPKG_ROOTS)
if(DEFINED ENV{VCPKG_ROOT})
    list(APPEND _IXWS_VCPKG_ROOTS "$ENV{VCPKG_ROOT}")
endif()
if(DEFINED ENV{VCPKG_INSTALLATION_ROOT})
    list(APPEND _IXWS_VCPKG_ROOTS "$ENV{VCPKG_INSTALLATION_ROOT}")
endif()
if(DEFINED VCPKG_INSTALLED_DIR)
    cmake_path(GET VCPKG_INSTALLED_DIR PARENT_PATH _ixws_installed_parent)
    cmake_path(GET _ixws_installed_parent PARENT_PATH _ixws_root_derived)
    if(_ixws_root_derived)
        list(APPEND _IXWS_VCPKG_ROOTS "${_ixws_root_derived}")
    endif()
endif()
list(REMOVE_DUPLICATES _IXWS_VCPKG_ROOTS)
set(_IXWS_BUILDTREE_PATHS)
foreach(_root IN LISTS _IXWS_VCPKG_ROOTS)
    list(APPEND _IXWS_BUILDTREE_PATHS "${_root}/buildtrees/ixwebsocket")
endforeach()

set(IXWEBSOCKET_SOURCE_DIR "" CACHE PATH "ixwebsocket source directory (optional, for source build)")
if(NOT IXWEBSOCKET_SOURCE_DIR)
    # 注意：third_party/ixwebsocket 的目录布局为 ixwebsocket/IXWebSocket.h（无 src/ 前缀），
    # vcpkg buildtrees 的布局为 src/ixwebsocket/IXWebSocket.h。
    # 直接用 EXISTS 检查路径，避免 find_path 对含子目录的 NAMES 处理不一致。
    set(_IXWS_THIRDPARTY "${CMAKE_SOURCE_DIR}/third_party/ixwebsocket")
    if(EXISTS "${_IXWS_THIRDPARTY}/ixwebsocket/IXWebSocket.h" AND EXISTS "${_IXWS_THIRDPARTY}/CMakeLists.txt")
        set(IXWEBSOCKET_SOURCE_DIR "${_IXWS_THIRDPARTY}" CACHE PATH "ixwebsocket source directory" FORCE)
    else()
        foreach(_root IN LISTS _IXWS_VCPKG_ROOTS)
            set(_candidate "${_root}/buildtrees/ixwebsocket/src")
            if(EXISTS "${_candidate}/ixwebsocket/IXWebSocket.h" AND EXISTS "${_candidate}/CMakeLists.txt")
                set(IXWEBSOCKET_SOURCE_DIR "${_candidate}" CACHE PATH "ixwebsocket source directory" FORCE)
                break()
            endif()
            # buildtrees 下可能直接是 src 子目录外的布局
            set(_candidate "${_root}/buildtrees/ixwebsocket")
            if(EXISTS "${_candidate}/ixwebsocket/IXWebSocket.h" AND EXISTS "${_candidate}/CMakeLists.txt")
                set(IXWEBSOCKET_SOURCE_DIR "${_candidate}" CACHE PATH "ixwebsocket source directory" FORCE)
                break()
            endif()
        endforeach()
    endif()
endif()
if(EXISTS "${IXWEBSOCKET_SOURCE_DIR}/CMakeLists.txt")
    message(STATUS "Building ixwebsocket from source: ${IXWEBSOCKET_SOURCE_DIR}")
    set(IXWEBSOCKET_USE_TLS ON CACHE INTERNAL "")
    set(IXWEBSOCKET_USE_MBED_TLS ON CACHE INTERNAL "")
    set(IXWEBSOCKET_USE_ZLIB ON CACHE INTERNAL "")
    set(IXWEBSOCKET_STATIC ON CACHE INTERNAL "")
    add_subdirectory("${IXWEBSOCKET_SOURCE_DIR}" "third_party/ixwebsocket_build")
else()
    # Fallback: use pre-built lib
    # ixwebsocket 可能仅安装在某个 triplet（如 x64-windows）而未安装在选定的
    # VCPKG_INSTALLED_DIR（如 x64-windows-142）中，因此搜索时需覆盖多个 triplet。
    set(_IXWS_INSTALLED_CANDIDATES "${VCPKG_INSTALLED_DIR}")
    foreach(_root IN LISTS _IXWS_VCPKG_ROOTS)
        foreach(_TRIPLET x64-windows-142 x64-windows)
            list(APPEND _IXWS_INSTALLED_CANDIDATES "${_root}/installed/${_TRIPLET}")
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES _IXWS_INSTALLED_CANDIDATES)

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
        message(STATUS "Using manually built ixwebsocket: ${IXWEBSOCKET_RELEASE_LIB}")
    else()
        find_library(IXWEBSOCKET_LIBRARY ixwebsocket
            PATHS ${_IXWS_INSTALLED_CANDIDATES}
            PATH_SUFFIXES lib
            NO_DEFAULT_PATH)
        set(IXWEBSOCKET_RELEASE_LIB "${IXWEBSOCKET_LIBRARY}")
    endif()

    if(EXISTS "${IXWEBSOCKET_DEBUG_LIB}")
        message(STATUS "Using manually built ixwebsocket Debug: ${IXWEBSOCKET_DEBUG_LIB}")
    else()
        message(STATUS "ixwebsocket Debug library not found - using Release for both configurations")
        set(IXWEBSOCKET_DEBUG_LIB "${IXWEBSOCKET_RELEASE_LIB}")
    endif()

    find_path(IXWEBSOCKET_INCLUDE_DIR ixwebsocket/IXWebSocket.h
        PATHS ${_IXWS_INSTALLED_CANDIDATES}
        PATH_SUFFIXES include
        NO_DEFAULT_PATH)
    if(NOT IXWEBSOCKET_INCLUDE_DIR)
        message(FATAL_ERROR "ixwebsocket include dir not found")
    endif()
    message(STATUS "Found ixwebsocket include: ${IXWEBSOCKET_INCLUDE_DIR}")
    message(STATUS "Found ixwebsocket lib: ${IXWEBSOCKET_RELEASE_LIB}")

    add_library(ixwebsocket::ixwebsocket STATIC IMPORTED)
    set_target_properties(ixwebsocket::ixwebsocket PROPERTIES
        IMPORTED_LOCATION_RELEASE "${IXWEBSOCKET_RELEASE_LIB}"
        IMPORTED_LOCATION_DEBUG "${IXWEBSOCKET_DEBUG_LIB}"
        IMPORTED_LOCATION "${IXWEBSOCKET_RELEASE_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${IXWEBSOCKET_INCLUDE_DIR}"
        INTERFACE_COMPILE_DEFINITIONS "IXWEBSOCKET_USE_TLS;IXWEBSOCKET_USE_MBED_TLS;IXWEBSOCKET_USE_ZLIB"
        INTERFACE_LINK_LIBRARIES
            "${MBEDTLS_LIBRARY};${MBEDX509_LIBRARY};${MBEDCRYPTO_LIBRARY};ZLIB::ZLIB;wsock32;ws2_32;shlwapi;Crypt32"
    )
endif()

# 5. kissfft（实数 FFT 库，用于频谱分析）
find_library(KISSFFT_LIBRARY kissfft-float PATHS "${VCPKG_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
find_path(KISSFFT_INCLUDE_DIR kiss_fft.h PATHS "${VCPKG_INSTALLED_DIR}/include/kissfft" NO_DEFAULT_PATH)
if(KISSFFT_LIBRARY AND KISSFFT_INCLUDE_DIR)
    add_library(kissfft::kissfft STATIC IMPORTED)
    set_target_properties(kissfft::kissfft PROPERTIES
        IMPORTED_LOCATION "${KISSFFT_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${KISSFFT_INCLUDE_DIR}")
    message(STATUS "Found kissfft: ${KISSFFT_LIBRARY}")
else()
    message(FATAL_ERROR "kissfft not found in vcpkg installed directory")
endif()

# 6. nlohmann_json（header-only，用 find_path 直接找头文件）
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
    PATHS "${VCPKG_INSTALLED_DIR}/include" NO_DEFAULT_PATH)
if(!NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "nlohmann_json not found in vcpkg installed directory")
endif()
add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${NLOHMANN_JSON_INCLUDE_DIR}")
message(STATUS "Found nlohmann_json: ${NLOHMANN_JSON_INCLUDE_DIR}")

# 7. cpp-httplib（header-only，用于替代手写 HTTP 解析）
find_path(HTTPLIB_INCLUDE_DIR httplib.h
    PATHS "${CMAKE_SOURCE_DIR}/third_party/httplib" NO_DEFAULT_PATH)
if(NOT HTTPLIB_INCLUDE_DIR)
    message(FATAL_ERROR "httplib not found in third_party/httplib")
endif()
add_library(httplib::httplib INTERFACE IMPORTED)
set_target_properties(httplib::httplib PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${HTTPLIB_INCLUDE_DIR}")
message(STATUS "Found httplib: ${HTTPLIB_INCLUDE_DIR}")

# ──── 源文件（按模块分组） ────

# core/: 应用核心 — 入口、上下文、消息循环、崩溃处理、常量
set(SOURCES
    src/core/main.cpp
    src/core/app_context.cpp
    src/core/crash_handler.cpp
    src/core/message_window.cpp
)
set(HEADERS
    src/core/app_context.h
    src/core/crash_handler.h
    src/core/message_window.h
    src/core/constants.h
)

# config/: 配置管理
list(APPEND SOURCES
    src/config/config.cpp
)
list(APPEND HEADERS
    src/config/config.h
)

# util/: 工具 — 日志、进程监控
list(APPEND SOURCES
    src/util/logger.cpp
    src/util/process_monitor.cpp
)
list(APPEND HEADERS
    src/util/logger.h
    src/util/process_monitor.h
)

# net/: 网络通信 — WebSocket、HTTP、Native Messaging、API
list(APPEND SOURCES
    src/net/websocket_client.cpp
    src/net/http_server.cpp
    src/net/native_messaging.cpp
    src/net/api_enabler.cpp
)
list(APPEND HEADERS
    src/net/websocket_client.h
    src/net/http_server.h
    src/net/native_messaging.h
    src/net/api_enabler.h
)

# lyrics/: 歌词处理 — 数据模型、LRC/KRC 解析
list(APPEND SOURCES
    src/lyrics/lyrics_parser.cpp
    src/lyrics/krc_parser.cpp
)
list(APPEND HEADERS
    src/lyrics/lyrics_data.h
    src/lyrics/lyrics_parser.h
    src/lyrics/krc_parser.h
)

# render/: 渲染引擎 — D2D/DWrite 管线、特效、频谱采集
list(APPEND SOURCES
    src/render/renderer.cpp
    src/render/lyric_renderer.cpp
    src/render/card_renderer.cpp
    src/render/marquee_engine.cpp
    src/render/spring_animation.cpp
    src/render/spectrum_capture.cpp
)
list(APPEND HEADERS
    src/render/renderer.h
    src/render/renderer_utils.h
    src/render/spectrum_capture.h
)

# taskbar/: 任务栏窗口 — 嵌入、定位、全屏检测、Shell 伴侣
list(APPEND SOURCES
    src/taskbar/taskbar_window.cpp
    src/taskbar/taskbar_embedder.cpp
    src/taskbar/taskbar_geometry.cpp
    src/taskbar/fullscreen_detector.cpp
    src/taskbar/shell_companion.cpp
)
list(APPEND HEADERS
    src/taskbar/taskbar_window.h
    src/taskbar/taskbar_embedder.h
    src/taskbar/taskbar_geometry.h
    src/taskbar/fullscreen_detector.h
    src/taskbar/shell_companion.h
)

# ui/: 用户界面 — 设置窗口、托盘图标、颜色选择器、配置对话框
list(APPEND SOURCES
    src/ui/d2d_settings_window.cpp
    src/ui/tray_icon.cpp
    src/ui/tray_commands.cpp
    src/ui/config_dialog.cpp
    src/ui/color_picker.cpp
)
list(APPEND HEADERS
    src/ui/d2d_settings_window.h
    src/ui/color_picker.h
    src/ui/color_utils.h
    src/ui/settings_draw_utils.h
    src/ui/tray_icon.h
    src/ui/tray_commands.h
    src/ui/config_dialog.h
)

# 资源文件
list(APPEND SOURCES
    src/app_icon.rc
)

# 可执行目标
add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/third_party
    ${NLOHMANN_JSON_INCLUDE_DIR}
    ${KISSFFT_INCLUDE_DIR}
    ${HTTPLIB_INCLUDE_DIR}
)

target_compile_options(${PROJECT_NAME} PRIVATE /await /winrt)  # C++/WinRT 支持 (SMTC API)，仅主项目生效
target_compile_definitions(${PROJECT_NAME} PRIVATE KISSFFT_STATIC)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ixwebsocket::ixwebsocket
    nlohmann_json::nlohmann_json
    kissfft::kissfft
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
    winhttp          # iTunes Strategy HTTP 请求
    windowsapp        # C++/WinRT 运行时 (SMTC)
    dwmapi            # DWM 圆角窗口 (D2D 设置界面)
    UIAutomationCore   # UI Automation: 获取任务栏子窗口几何信息
)

# 输出到项目根目录下的 moeKoe-taskbar-lyrics 文件夹
# 使用 _DEBUG/_RELEASE 两变量指向同一目录，避免 MSVC 多配置生成器追加 /Debug 子目录
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/moeKoe-taskbar-lyrics"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/moeKoe-taskbar-lyrics"
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
    # 1) z.dll（zlib 运行时依赖）
    find_file(ZLIB_RUNTIME_DLL z.dll PATHS "${VCPKG_INSTALLED_DIR}/bin" NO_DEFAULT_PATH)
    if(ZLIB_RUNTIME_DLL)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ZLIB_RUNTIME_DLL}"
                $<TARGET_FILE_DIR:${PROJECT_NAME}>/z.dll
            COMMENT "Copying z.dll to output directory")
    endif()

    # 2) kissfft-float.dll（kissfft 运行时依赖）
    # vcpkg classic mode 构建为 DLL，需要复制到输出目录
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<IF:$<CONFIG:Debug>,${VCPKG_INSTALLED_DIR}/debug/bin/kissfft-float.dll,${VCPKG_INSTALLED_DIR}/bin/kissfft-float.dll>
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/kissfft-float.dll
        COMMENT "Copying kissfft-float.dll to output directory")

    # 3) VC++ Redist CRT DLL（msvcp140 / vcruntime140 / vcruntime140_1）
    #    发布包中必须附带，避免用户机器缺失 VC Redist 导致启动失败
    #    从编译器路径向上推导到 VC 根目录：
    #      cl.exe: .../VC/Tools/MSVC/<ver>/bin/Hostx64/x64/cl.exe
    #      Redist:  .../VC/Redist/MSVC/<ver>/x64/Microsoft.VC143.CRT/*.dll
    cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH _crt_path)          # .../bin/Hostx64/x64
    cmake_path(GET _crt_path PARENT_PATH _crt_path)                   # .../bin/Hostx64
    cmake_path(GET _crt_path PARENT_PATH _crt_path)                   # .../bin
    cmake_path(GET _crt_path PARENT_PATH _msvc_ver_path)              # .../Tools/MSVC/<ver>
    cmake_path(GET _msvc_ver_path FILENAME _msvc_ver)
    cmake_path(GET _msvc_ver_path PARENT_PATH _crt_path)              # .../Tools/MSVC
    cmake_path(GET _crt_path PARENT_PATH _crt_path)                   # .../Tools
    cmake_path(GET _crt_path PARENT_PATH _vc_root)                    # .../VC

    set(_CRT_SRC_DIR "${_vc_root}/Redist/MSVC/${_msvc_ver}/x64/Microsoft.VC143.CRT")

    if(NOT EXISTS "${_CRT_SRC_DIR}")
        # 备选：某些 VS 版本的 CRT 放在 onecore 子目录下
        set(_CRT_SRC_DIR "${_vc_root}/Redist/MSVC/${_msvc_ver}/onecore/x64/Microsoft.VC143.CRT")
    endif()

    if(NOT EXISTS "${_CRT_SRC_DIR}")
        message(WARNING "Cannot locate VC++ CRT DLL directory. "
                        "Expected: ${_vc_root}/Redist/MSVC/${_msvc_ver}/x64/Microsoft.VC143.CRT. "
                        "Please manually copy msvcp140.dll, vcruntime140.dll, vcruntime140_1.dll to output.")
    else()
        message(STATUS "Copying VC++ CRT DLLs from: ${_CRT_SRC_DIR}")
        foreach(_CRT_DLL msvcp140.dll vcruntime140.dll vcruntime140_1.dll)
            if(EXISTS "${_CRT_SRC_DIR}/${_CRT_DLL}")
                add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_CRT_SRC_DIR}/${_CRT_DLL}"
                        $<TARGET_FILE_DIR:${PROJECT_NAME}>/${_CRT_DLL}
                    COMMENT "Copying ${_CRT_DLL} to output directory")
            endif()
        endforeach()
    endif()

endif()

# ──── 单元测试 ────
option(BUILD_TESTING "Build unit tests" ON)

if(BUILD_TESTING)
    enable_testing()

    # 使用 FetchContent 获取 Catch2 v3（header + 轻量编译）
    include(FetchContent)
    find_package(Git QUIET)  # 用于预检 Catch2 仓库可达性

    # Catch2 源码获取策略（按优先级）：
    #   1. 用户通过 -DCATCH2_SOURCE_DIR=<path> 显式指定本地源码目录
    #   2. 复用其他 build 目录已下载的源码（避免重复下载）
    #   3. 从 GitHub/gitee 克隆（网络受限环境下可能失败 → 优雅降级跳过测试）
    set(CATCH2_SOURCE_DIR "" CACHE PATH "Catch2 source directory (optional, to avoid git download)")

    # 检查本地已有的 Catch2 源码（复用 build/_deps 或 third_party 下的下载）
    if(NOT CATCH2_SOURCE_DIR)
        foreach(_candidate
                "${CMAKE_SOURCE_DIR}/build/_deps/catch2-src"
                "${CMAKE_SOURCE_DIR}/third_party/catch2")
            if(EXISTS "${_candidate}/CMakeLists.txt" AND EXISTS "${_candidate}/src/catch2/catch_all.hpp")
                set(CATCH2_SOURCE_DIR "${_candidate}")
                message(STATUS "Reusing local Catch2 source: ${CATCH2_SOURCE_DIR}")
                break()
            endif()
        endforeach()
    endif()

    # 最小化 Catch2 编译：只构建需要的组件
    set(CATCH_BUILD_TESTING OFF CACHE INTERNAL "")
    set(CATCH_BUILD_EXAMPLES OFF CACHE INTERNAL "")
    set(CATCH_BUILD_EXTRA_TESTS OFF CACHE INTERNAL "")
    set(CATCH_INSTALL_DOCS OFF CACHE INTERNAL "")
    set(CATCH_INSTALL_EXTRAS OFF CACHE INTERNAL "")

    set(_catch2_ready FALSE)

    if(CATCH2_SOURCE_DIR AND EXISTS "${CATCH2_SOURCE_DIR}/CMakeLists.txt")
        # 使用本地源码目录（无需网络下载）
        FetchContent_Declare(Catch2 SOURCE_DIR "${CATCH2_SOURCE_DIR}")
        FetchContent_MakeAvailable(Catch2)
        set(_catch2_ready TRUE)
    else()
        # 从 Git 克隆；GitHub 不通时尝试 gitee 镜像
        if(GIT_FOUND)
            foreach(_repo
                    "https://github.com/catchorg/Catch2.git"
                    "https://gitee.com/mirrors/Catch2.git")
                message(STATUS "Trying to fetch Catch2 from: ${_repo}")
                # FetchContent_Declare 不会立即下载，MakeAvailable 才触发
                FetchContent_Declare(Catch2
                    GIT_REPOSITORY "${_repo}"
                    GIT_TAG        v3.7.1
                    GIT_SHALLOW    TRUE)
                # 用 execute_process 预检 git 可达性，避免 MakeAvailable 中断配置
                execute_process(
                    COMMAND ${GIT_EXECUTABLE} ls-remote --heads "${_repo}" v3.7.1
                    RESULT_VARIABLE _git_probe_result
                    OUTPUT_QUIET ERROR_QUIET TIMEOUT 15)
                if(_git_probe_result EQUAL 0)
                    FetchContent_MakeAvailable(Catch2)
                    set(_catch2_ready TRUE)
                    break()
                else()
                    message(STATUS "Catch2 repo unreachable: ${_repo}")
                endif()
            endforeach()
        else()
            message(STATUS "Git not found - cannot fetch Catch2 from remote")
        endif()

        if(NOT _catch2_ready)
            set(BUILD_TESTING OFF CACHE BOOL "Build unit tests" FORCE)
            message(WARNING
                "Catch2 download failed (network unavailable or git not found). Skipping unit tests.\n"
                "To enable tests:\n"
                "  1. Pre-download Catch2 v3.7.1 and set -DCATCH2_SOURCE_DIR=<path>\n"
                "  2. Or place it at third_party/catch2/\n"
                "  3. Or ensure network access to GitHub/gitee")
        endif()
    endif()
endif()

if(BUILD_TESTING AND _catch2_ready)
    # 测试目标：lyrics_parser 核心逻辑
    add_executable(test_lyrics_parser
        tests/test_lyrics_parser.cpp
        src/lyrics/lyrics_parser.cpp
    )

    target_include_directories(test_lyrics_parser PRIVATE
        src
        ${CMAKE_SOURCE_DIR}/third_party
    )

    if(MSVC)
        target_compile_options(test_lyrics_parser PRIVATE
            /W4 /permissive- /utf-8 /EHsc
        )
        target_compile_definitions(test_lyrics_parser PRIVATE
            _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX UNICODE _UNICODE
        )
        # 测试可执行文件必须是 Console 应用（而非 WinMain 入口）
        set_target_properties(test_lyrics_parser PROPERTIES
            WIN32_EXECUTABLE OFF
        )
    endif()

    target_link_libraries(test_lyrics_parser PRIVATE
        Catch2::Catch2
        Threads::Threads
    )

    # 注册 CTest
    include(CTest)
    add_test(NAME LyricsParser COMMAND $<TARGET_FILE:test_lyrics_parser> -s)

    # 测试目标：krc_parser KRC 歌词解析逻辑
    add_executable(test_krc_parser
        tests/test_krc_parser.cpp
        src/lyrics/krc_parser.cpp
        src/util/logger.cpp
    )

    target_include_directories(test_krc_parser PRIVATE
        src
        ${CMAKE_SOURCE_DIR}/third_party
        ${NLOHMANN_JSON_INCLUDE_DIR}
    )

    if(MSVC)
        target_compile_options(test_krc_parser PRIVATE
            /W4 /permissive- /utf-8 /EHsc
        )
        target_compile_definitions(test_krc_parser PRIVATE
            _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX UNICODE _UNICODE
        )
        set_target_properties(test_krc_parser PROPERTIES
            WIN32_EXECUTABLE OFF
        )
    endif()

    target_link_libraries(test_krc_parser PRIVATE
        Catch2::Catch2
        Threads::Threads
    )

    add_test(NAME KrcParser COMMAND $<TARGET_FILE:test_krc_parser> -s)

    # 复制 VC++ Redist CRT DLL 到测试可执行文件输出目录
    # 测试 exe 用 /MD 动态链接运行时，需要 msvcp140.dll / vcruntime140.dll /
    # vcruntime140_1.dll。主程序通过 POST_BUILD 已复制，但测试目标默认不会，
    # 缺失时 Windows 会从 PATH 加载旧版本 msvcp140.dll，导致 std::mutex 等
    # STL 同步原语在运行时崩溃（STATUS_ACCESS_VIOLATION）。
    if(MSVC AND EXISTS "${_CRT_SRC_DIR}")
        foreach(_TEST_TARGET test_lyrics_parser test_krc_parser)
            foreach(_CRT_DLL msvcp140.dll vcruntime140.dll vcruntime140_1.dll)
                if(EXISTS "${_CRT_SRC_DIR}/${_CRT_DLL}")
                    add_custom_command(TARGET ${_TEST_TARGET} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${_CRT_SRC_DIR}/${_CRT_DLL}"
                            $<TARGET_FILE_DIR:${_TEST_TARGET}>/${_CRT_DLL}
                        COMMENT "Copying ${_CRT_DLL} to ${_TEST_TARGET} output dir")
                endif()
            endforeach()
        endforeach()
    endif()
endif()