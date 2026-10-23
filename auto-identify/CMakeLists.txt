cmake_minimum_required(VERSION 3.29)
project(AutoIdentify LANGUAGES CXX RC)

# SDK checkout at third_party/PluginSDK (see CLAUDE.md).
set(D2RL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/PluginSDK" CACHE PATH "D2RLoader PluginSDK checkout")
if(NOT TARGET D2RLPlugin::D2RLPlugin)
	add_subdirectory("${D2RL_SDK_DIR}" "${CMAKE_BINARY_DIR}/PluginSDK" EXCLUDE_FROM_ALL)
endif()

add_library(AutoIdentify SHARED
	src/auto_identify.cpp
	src/auto_identify.rc
)
set_target_properties(AutoIdentify PROPERTIES
	OUTPUT_NAME "d2rl-auto-identify"   # produces d2rl-auto-identify.dll
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
)
target_link_libraries(AutoIdentify PRIVATE D2RLPlugin::D2RLPlugin)

if(MSVC)
	target_compile_options(AutoIdentify PRIVATE /W4 /permissive- /EHsc)
endif()

# Embed the default config into the DLL (written to d2rloader/config/ when missing).
d2rlplugin_embed_config(
	TARGET AutoIdentify
	FILE "${CMAKE_CURRENT_SOURCE_DIR}/config/auto-identify.toml"
)

# Build (Visual Studio Community, x64): run build.cmd, or
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
# Output: build/d2rl-auto-identify.dll  ->  <game>/d2rloader/plugins/
