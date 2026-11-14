cmake_minimum_required(VERSION 3.29)
project(Arena LANGUAGES CXX RC)

set(D2RL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/PluginSDK" CACHE PATH "D2RLoader PluginSDK checkout")
if(NOT TARGET D2RLPlugin::D2RLPlugin)
	add_subdirectory("${D2RL_SDK_DIR}" "${CMAKE_BINARY_DIR}/PluginSDK" EXCLUDE_FROM_ALL)
endif()

add_library(Arena SHARED
	src/arena.cpp
	src/arena.rc
)
set_target_properties(Arena PROPERTIES
	OUTPUT_NAME "d2rl-arena"   # produces d2rl-arena.dll
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
)
target_link_libraries(Arena PRIVATE D2RLPlugin::D2RLPlugin)

if(MSVC)
	target_compile_options(Arena PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /EHsc /Zi>)
	target_link_options(Arena PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

# Embed the default config into the DLL (written to d2rloader/config/arena.toml when missing).
d2rlplugin_embed_config(
	TARGET Arena
	FILE "${CMAKE_CURRENT_SOURCE_DIR}/config/arena.toml"
)

# Build (Visual Studio Community, x64):
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
# Deploy: deploy.cmd (mods\arena only).
