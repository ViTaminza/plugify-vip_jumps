# vip_jumps — Plugify C++ plugin: port of the Pisex/ABKAM VIP module "VIP_Jumps".
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no guard library. The map gate and
# the client checks live in src/plugin.cpp; the engine-free jump state machine is shared with the
# test through src/jump_logic.hpp.
#
#   plugify-vip_jumps  the plugin shared library (entry "bin/vip_jumps")

set(VIP_JUMPS_VERSION "1.2.0")

# Binding globals the plugify module fills from the plugins declared as dependencies:
#   __s2sdk_*   (s2sdk_bindings)
#   __vip_*     (vip consumer bindings, generated into plugins/vip/include/external/plugify/include)
# In the monorepo plugins/vip is added first ("vip" < "vip_jumps"), so vip_bindings already exists;
# a standalone exported repo only carries plugins/vip/include/ headers + vip.cpp and has no
# plugins/vip/CMakeLists.txt - define vip_bindings here when absent.
if(NOT TARGET vip_bindings)
    add_library(vip_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include/vip/vip.cpp")
    target_include_directories(vip_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(vip_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

add_library(plugify-vip_jumps SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
)
target_include_directories(plugify-vip_jumps PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated vip/s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-vip_jumps SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-vip_jumps PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-vip_jumps PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-vip_jumps PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_jumps"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-vip_jumps PROPERTIES OUTPUT_NAME vip_jumps)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-vip_jumps PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/vip_jumps.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/vip_jumps.pplugin"
)

# --- tests --------------------------------------------------------------------
# Engine-free logic tests; run by ./test.sh (build/tests). Nothing here links pisexcompat or an
# engine binding: the test only compiles the plugin's own jump_logic.hpp (the C2 slot range, the
# extra-jump budget and the per-tick state machine with its launch-window fix).
add_executable(vip_jumps_tests tests/vip_jumps_tests.cpp)
target_include_directories(vip_jumps_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(vip_jumps_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_jumps_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
target_link_libraries(vip_jumps_tests PRIVATE -static-libstdc++ -static-libgcc)
set_target_properties(vip_jumps_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# Runtime tests: compile ../src/plugin.cpp with the real generated s2sdk/vip bindings.
add_executable(vip_jumps_runtime_tests
        tests/vip_jumps_runtime_tests.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
)
target_include_directories(vip_jumps_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(vip_jumps_runtime_tests SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(vip_jumps_runtime_tests PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(vip_jumps_runtime_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_jumps_runtime_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_jumps_runtime_tests"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(vip_jumps_runtime_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
