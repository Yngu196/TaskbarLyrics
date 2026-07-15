cmake_minimum_required(VERSION 3.20)

project(MoeKoeTaskbarLyrics
    VERSION 1.0.0
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
    find_path(IXWEBSOCKET_SOURCE_DIR ixwebsocket/IXWebSocket.h
        PATHS "${CMAKE_SOURCE_DIR}/third_party/ixwebsocket"
              ${_IXWS_BUILDTREE_PATHS}
              # 硬编码路径作为最后 fallback
              "D:/vcpkg/buildtrees/ixwebsocket"
              "C:/vcpkg/buildtrees/ixwebsocket"
              "${VCPKG_INSTALLED_DIR}/include"
        PATH_SUFFIXES src)
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
        find_library(IXWEBSOCKET_LIBRARY ixwebsocket PATHS "${VCPKG_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
        set(IXWEBSOCKET_RELEASE_LIB "${IXWEBSOCKET_LIBRARY}")
    endif()

    if(EXISTS "${IXWEBSOCKET_DEBUG_LIB}")
        message(STATUS "Using manually built ixwebsocket Debug: ${IXWEBSOCKET_DEBUG_LIB}")
    else()
        message(STATUS "ixwebsocket Debug library not found - using Release for both configurations")
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

# 源文件
set(SOURCES
    src/main.cpp
    src/logger.cpp
    src/websocket_client.cpp
    src/http_server.cpp
    src/d2d_settings_window.cpp
    src/lyrics_parser.cpp
    src/shell_companion.cpp
    src/taskbar_window.cpp
    src/taskbar_embedder.cpp
    src/taskbar_geometry.cpp
    src/fullscreen_detector.cpp
    src/renderer.cpp
    src/lyric_renderer.cpp
    src/card_renderer.cpp
    src/marquee_engine.cpp
    src/spring_animation.cpp
    src/spectrum_capture.cpp
    src/config.cpp
    src/tray_icon.cpp
    src/config_dialog.cpp
    src/native_messaging.cpp
    src/api_enabler.cpp
    src/color_picker.cpp
    src/app_icon.rc
)

# 头文件目录
set(HEADERS
    src/logger.h
    src/lyrics_data.h
    src/websocket_client.h
    src/http_server.h
    src/d2d_settings_window.h
    src/color_picker.h
    src/color_utils.h
    src/settings_draw_utils.h
    src/lyrics_parser.h
    src/shell_companion.h
    src/taskbar_window.h
    src/taskbar_embedder.h
    src/taskbar_geometry.h
    src/fullscreen_detector.h
    src/renderer.h
    src/renderer_utils.h
    src/config.h
    src/tray_icon.h
    src/config_dialog.h
    src/api_enabler.h
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
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE
    )
    # 最小化 Catch2 编译：只构建需要的组件
    set(CATCH_BUILD_TESTING OFF CACHE INTERNAL "")
    set(CATCH_BUILD_EXAMPLES OFF CACHE INTERNAL "")
    set(CATCH_BUILD_EXTRA_TESTS OFF CACHE INTERNAL "")
    set(CATCH_INSTALL_DOCS OFF CACHE INTERNAL "")
    set(CATCH_INSTALL_EXTRAS OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(Catch2)

    # 测试目标：lyrics_parser 核心逻辑
    add_executable(test_lyrics_parser
        tests/test_lyrics_parser.cpp
        src/lyrics_parser.cpp
    )

    target_include_directories(test_lyrics_parser PRIVATE
        src
        ${CMAKE_SOURCE_DIR}/third_party
    )

    target_compile_definitions(test_lyrics_parser PRIVATE
        MOEKOE_UNIT_TEST
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
endif()