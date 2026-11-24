# Build the pinned WebView sources with Ovygram's per-view proxy API. Keep the
# submodule checkout clean so stock Telegram builds still see upstream files.
find_package(Git REQUIRED)
set(webview_upstream "${CMAKE_CURRENT_SOURCE_DIR}/lib_webview")
set(webview_patch "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/lib_webview_local_proxy.patch")
set(webview_patched "${CMAKE_CURRENT_BINARY_DIR}/ovygram-webview-src")
file(GLOB_RECURSE webview_inputs CONFIGURE_DEPENDS
    RELATIVE "${webview_upstream}" "${webview_upstream}/webview/*")
list(APPEND webview_inputs CMakeLists.txt ForceStaticLink.targets)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${webview_patch}")
file(SHA256 "${webview_patch}" webview_signature)
foreach(input IN LISTS webview_inputs)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${webview_upstream}/${input}")
    file(SHA256 "${webview_upstream}/${input}" input_hash)
    string(APPEND webview_signature "${input}:${input_hash}\n")
endforeach()
string(SHA256 webview_digest "${webview_signature}")
if(NOT EXISTS "${webview_patched}/.prepared-${webview_digest}")
    file(REMOVE_RECURSE "${webview_patched}")
    foreach(input IN LISTS webview_inputs)
        configure_file("${webview_upstream}/${input}" "${webview_patched}/${input}" COPYONLY)
    endforeach()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}"
            "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${webview_patch}"
        WORKING_DIRECTORY "${webview_patched}"
        COMMAND_ERROR_IS_FATAL ANY)
    file(TOUCH "${webview_patched}/.prepared-${webview_digest}")
endif()
add_subdirectory("${webview_patched}" "${CMAKE_CURRENT_BINARY_DIR}/lib_webview")
target_compile_definitions(lib_webview PUBLIC OVYGRAM_WEBVIEW_ROUTES)
