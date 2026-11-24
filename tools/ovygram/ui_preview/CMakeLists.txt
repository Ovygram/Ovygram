cmake_minimum_required(VERSION 3.25...3.31)
project(OvygramUiPreview LANGUAGES C CXX)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The standalone Ovygram preview builds on Linux.")
endif()

get_filename_component(repo "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(submodules_loc "${repo}/Telegram")
set(third_party_loc "${submodules_loc}/ThirdParty")
set(cmake_helpers_loc "${repo}/cmake")
set(DESKTOP_APP_USE_PACKAGED ON)
set(DESKTOP_APP_DISABLE_CRASH_REPORTS ON)
set(DESKTOP_APP_USE_PANGO ON)
set(BUILD_EMBED_IGNORE ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(QT_NO_PRIVATE_MODULE_WARNING ON)

include(${cmake_helpers_loc}/nice_target_sources.cmake)
include(${cmake_helpers_loc}/target_compile_options_if_exists.cmake)
include(${cmake_helpers_loc}/target_link_options_if_exists.cmake)
include(${cmake_helpers_loc}/init_target.cmake)
include(${cmake_helpers_loc}/generate_target.cmake)
include(${cmake_helpers_loc}/target_prepare_qrc.cmake)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core CorePrivate Gui GuiPrivate Widgets Network Svg OpenGL OpenGLWidgets DBus Test)
set(QT_VERSION_MAJOR 6)
set(QT_VERSION ${Qt6_VERSION})

add_library(common_options INTERFACE)
add_library(desktop-app::common_options ALIAS common_options)
target_compile_definitions(common_options INTERFACE QT_NO_KEYWORDS QT_NO_CAST_FROM_BYTEARRAY)
include(${cmake_helpers_loc}/options_linux.cmake)

add_library(preview_qt INTERFACE)
add_library(desktop-app::external_qt ALIAS preview_qt)
target_link_libraries(preview_qt INTERFACE
    Qt6::Core Qt6::Gui Qt6::CorePrivate Qt6::GuiPrivate Qt6::Widgets
    Qt6::Network Qt6::Svg Qt6::OpenGL Qt6::OpenGLWidgets Qt6::DBus
)

set(DESKTOP_APP_DISABLE_QT_PLUGINS ON)
add_subdirectory(${cmake_helpers_loc}/external/qt/qt_static_plugins external/qt_static_plugins)

foreach(dependency IN ITEMS crash_reports expected glib gsl jpeg kcoreaddons lz4 openssl pango ranges xxhash zlib)
    add_subdirectory(${cmake_helpers_loc}/external/${dependency} external/${dependency})
endforeach()

foreach(module IN ITEMS lib_rpl lib_crl lib_base lib_ui codegen)
    add_subdirectory(${submodules_loc}/${module} ${module})
endforeach()

set(src_loc "${submodules_loc}/SourceFiles")
add_executable(ovygram-ui-preview
    main.cpp
    plugin_list.cpp
    ${src_loc}/ui/vertical_list.cpp
    ${src_loc}/ovygram/plugin_management_view.cpp
    ${src_loc}/ovygram/plugin_install_view.cpp
    ${src_loc}/ovygram/plugin_ui_tree.cpp
    ${src_loc}/ovygram/plugin_paint.cpp
    ${src_loc}/ovygram/plugin_proxy_view.cpp
)
target_compile_features(ovygram-ui-preview PRIVATE cxx_std_20)
target_compile_definitions(ovygram-ui-preview PRIVATE OVYGRAM_PREVIEW_PYTHON="${CMAKE_CURRENT_SOURCE_DIR}/plugin.py")
target_link_libraries(ovygram-ui-preview PRIVATE desktop-app::lib_ui Qt6::Test)
target_include_directories(ovygram-ui-preview PRIVATE ${src_loc})
target_precompile_headers(ovygram-ui-preview PRIVATE ${src_loc}/ui/ui_pch.h)

include(${submodules_loc}/lib_ui/cmake/generate_styles.cmake)
generate_styles(ovygram-ui-preview ${src_loc} "ovygram/plugin_management.style;ui/menu_icons.style"
    "${submodules_loc}/lib_ui/ui/colors.palette;${submodules_loc}/lib_ui/ui/basic.style;${submodules_loc}/lib_ui/ui/widgets/widgets.style;${submodules_loc}/lib_ui/ui/layers/layers.style")

foreach(index RANGE 1 8)
    target_sources(ovygram-ui-preview PRIVATE ${submodules_loc}/Resources/qrc/emoji_${index}.qrc)
endforeach()
target_prepare_qrc(ovygram-ui-preview)
