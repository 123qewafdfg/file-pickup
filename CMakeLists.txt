cmake_minimum_required(VERSION 3.21)

project(CangjianPC VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 Network Svg Concurrent)

# 依赖路径：优先 -D 传入，其次环境变量，最后回落作者本机默认路径
if (NOT LIBSODIUM_ROOT)
    if (DEFINED ENV{LIBSODIUM_ROOT})
        set(LIBSODIUM_ROOT "$ENV{LIBSODIUM_ROOT}" CACHE PATH "已编译的 libsodium 目录（含 libsodium.lib / libsodium.dll）")
    else()
        set(LIBSODIUM_ROOT "E:/pz/packages/libsodium/60b751c/win64" CACHE PATH "已编译的 libsodium 目录（含 libsodium.lib / libsodium.dll）")
    endif()
endif()
if (NOT LIBSODIUM_SOURCE)
    if (DEFINED ENV{LIBSODIUM_SOURCE})
        set(LIBSODIUM_SOURCE "$ENV{LIBSODIUM_SOURCE}" CACHE PATH "libsodium 源码目录（提供头文件）")
    else()
        set(LIBSODIUM_SOURCE "E:/pz/packages/藏笺参考项目/libsodium" CACHE PATH "libsodium 源码目录（提供头文件）")
    endif()
endif()
if (NOT EXISTS "${LIBSODIUM_ROOT}/libsodium.lib" OR NOT EXISTS "${LIBSODIUM_ROOT}/libsodium.dll")
    message(FATAL_ERROR "找不到已构建的 libsodium：${LIBSODIUM_ROOT}")
endif()
add_library(libsodium SHARED IMPORTED GLOBAL)
set_target_properties(libsodium PROPERTIES
    IMPORTED_IMPLIB "${LIBSODIUM_ROOT}/libsodium.lib"
    IMPORTED_LOCATION "${LIBSODIUM_ROOT}/libsodium.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBSODIUM_SOURCE}/src/libsodium/include"
)

qt_standard_project_setup(REQUIRES 6.5)

# ---------------------------------------------------------------------------
# FluentUI：Windows 11 Fluent Design 组件库（MIT），源码在 E:\pz 下，静态链接
# ---------------------------------------------------------------------------
if (NOT FLUENTUI_ROOT)
    if (DEFINED ENV{FLUENTUI_ROOT})
        set(FLUENTUI_ROOT "$ENV{FLUENTUI_ROOT}" CACHE PATH "FluentUI 源码目录（含 CMakeLists.txt）")
    else()
        set(FLUENTUI_ROOT "E:/pz/packages/fluentui/1.7.7/src" CACHE PATH "FluentUI 源码目录（含 CMakeLists.txt）")
    endif()
endif()
if (NOT EXISTS "${FLUENTUI_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "找不到 FluentUI 源码：${FLUENTUI_ROOT}")
endif()
set(FLUENTUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FLUENTUI_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
# 注意：FluentUI 把 Qt6::Widgets / PrintSupport 挂在 PUBLIC 链接接口上（其 qmlcustomplot 需要），
# 试过从接口里摘掉，但 FluentUI.cpp 里硬注册了 QCustomPlot 类型，链接会缺 QWidget 符号，只能带着。
add_subdirectory("${FLUENTUI_ROOT}" "${CMAKE_BINARY_DIR}/fluentui" EXCLUDE_FROM_ALL)

# ---- 核心库：与界面无关的加解密 / TMSHARE1 载体 / 打包 / 后端协议 ----
add_library(cangjian_core STATIC
    src/core/crypto.cpp
    src/core/crypto.h
    src/core/secretstream.cpp
    src/core/secretstream.h
    src/core/tmshare.cpp
    src/core/tmshare.h
    src/core/partition.cpp
    src/core/partition.h
    src/core/payload.cpp
    src/core/payload.h
    src/core/backend.cpp
    src/core/backend.h
    src/core/creator.cpp
    src/core/creator.h
    src/core/restorer.cpp
    src/core/restorer.h
    src/core/logger.cpp
    src/core/logger.h
    src/core/securestore.cpp
    src/core/securestore.h
    src/core/compressor.cpp
    src/core/compressor.h
    third_party/miniz/miniz.c
)
target_include_directories(cangjian_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz
)
target_link_libraries(cangjian_core PUBLIC Qt6::Core Qt6::Gui Qt6::Network libsodium)

target_link_libraries(cangjian_core PRIVATE bcrypt crypt32)
if (MSVC)
    target_compile_options(cangjian_core PRIVATE /utf-8 /W3)
    set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "/W0")
endif()

# ---- 应用 ----
qt_add_executable(CangjianPC
    src/main.cpp
    src/app/appcontroller.cpp
    src/app/appcontroller.h
    src/ui/circularreveal.cpp
    src/ui/circularreveal.h
    src/qq/qrlogin.cpp
    src/qq/qrlogin.h
    src/qq/channelapi.cpp
    src/qq/channelapi.h
)

if (WIN32)
    target_sources(CangjianPC PRIVATE resources/windows/file-pickup.rc)
endif()

file(GLOB CJ_QML_RELS CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/qml" "${CMAKE_CURRENT_SOURCE_DIR}/qml/*.qml")
set(CJ_QML_FILES "")
foreach(rel ${CJ_QML_RELS})
    list(APPEND CJ_QML_FILES "qml/${rel}")
endforeach()

# QML 单例（Router）必须在 qt_add_qml_module 之前声明，否则 qmldir 里不会带 singleton 标记
set_source_files_properties(qml/Router.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(CangjianPC
    URI Cangjian
    VERSION 1.0
    QML_FILES ${CJ_QML_FILES}
)

# 图标资源独立挂到 /icons 前缀（不放进 QML 模块，否则会被加 /qt/qml/Cangjian 前缀）
file(GLOB CJ_ICONS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/*.svg"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/*.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/*.jpg"
)
qt_add_resources(CangjianPC "icons"
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons"
    FILES ${CJ_ICONS}
)

# B站承载图：固定预设（来自隐像 nativeAssets/bili_carrier.png），
# 只有这张图追加数据后 B 站 CDN 才会原样保留，100x100 白图会被重编码
qt_add_resources(CangjianPC "carrier"
    PREFIX "/carrier"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/carrier"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/carrier/bili_carrier.png"
)

# Python 网络层脚本编入主程序。运行时由应用释放到用户数据目录，
# 发布目录不再携带可见的 scripts/*.py 文件。
set(CJ_RUNTIME_SCRIPTS
    scripts/qq_channel.py
    scripts/bili_dynamic.py
    scripts/feejii.py
    scripts/join_ui.py
    scripts/cdp_capture.py
)
qt_add_resources(CangjianPC "python_scripts"
    PREFIX "/python"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
    FILES ${CJ_RUNTIME_SCRIPTS}
)

target_link_libraries(CangjianPC PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Svg
    Qt6::Concurrent
    cangjian_core
    fluentuiplugin
)

set_target_properties(CangjianPC PROPERTIES
    OUTPUT_NAME "cangjian"
    # GUI 子系统：否则从资源管理器/单文件启动器拉起时会自带一个 cmd 黑窗
    WIN32_EXECUTABLE TRUE
)

if (MSVC)
    target_compile_options(CangjianPC PRIVATE /utf-8 /W3)
endif()

# 运行期要调 Python 脚本做频道发帖：解释器优先使用发布包内的 runtime/python。
target_compile_definitions(CangjianPC PRIVATE
    CJ_PYTHON_DEFAULT="E:/pz/environments/python/3.11.9/python.exe"
    # 开发机上的 aria2c；正式分发时随包放到 exe 同目录即可，优先于这个默认值
    CJ_ARIA2_DEFAULT="E:/pz/packages/aria2/1.37.0/win64/aria2-1.37.0-win-64bit-build1/aria2c.exe"
)

# aria2c 存在时复制到输出目录，保证部署包和单文件包里都带着
set(CJ_ARIA2_EXE "E:/pz/packages/aria2/1.37.0/win64/aria2-1.37.0-win-64bit-build1/aria2c.exe")
if(EXISTS "${CJ_ARIA2_EXE}")
    add_custom_command(TARGET CangjianPC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CJ_ARIA2_EXE}" "$<TARGET_FILE_DIR:CangjianPC>/aria2c.exe")
endif()

add_custom_command(TARGET CangjianPC POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LIBSODIUM_ROOT}/libsodium.dll"
        "$<TARGET_FILE_DIR:CangjianPC>/libsodium.dll"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:CangjianPC>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LIBSODIUM_SOURCE}/LICENSE"
        "$<TARGET_FILE_DIR:CangjianPC>/licenses/libsodium-ISC.txt"
)

set(CJ_ARIA2_LICENSE "E:/pz/packages/aria2/1.37.0/source/COPYING")
if(EXISTS "${CJ_ARIA2_LICENSE}")
    add_custom_command(TARGET CangjianPC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:CangjianPC>/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CJ_ARIA2_LICENSE}"
            "$<TARGET_FILE_DIR:CangjianPC>/licenses/aria2-GPL-2.0.txt"
    )
endif()

# ---- 自测程序：不依赖界面，验证密码学与载体格式 ----
add_executable(cangjian_tests tests/core_tests.cpp tests/compress_tests.cpp)
target_link_libraries(cangjian_tests PRIVATE cangjian_core)
add_custom_command(TARGET cangjian_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LIBSODIUM_ROOT}/libsodium.dll"
        "$<TARGET_FILE_DIR:cangjian_tests>/libsodium.dll"
)
if (MSVC)
    target_compile_options(cangjian_tests PRIVATE /utf-8 /W3)
endif()

include(GNUInstallDirs)
install(TARGETS CangjianPC
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
