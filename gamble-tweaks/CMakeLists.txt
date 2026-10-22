cmake_minimum_required(VERSION 3.29)
project(GambleTweaks LANGUAGES CXX RC)

# Point this at your SDK checkout. Per Claude.md it lives at third_party/PluginSDK.
# Either add_subdirectory it, or use find_package(D2RLPlugin CONFIG REQUIRED) if installed.
set(D2RL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/PluginSDK" CACHE PATH "D2RLoader PluginSDK checkout")
if(NOT TARGET D2RLPlugin::D2RLPlugin)
	add_subdirectory("${D2RL_SDK_DIR}" "${CMAKE_BINARY_DIR}/PluginSDK" EXCLUDE_FROM_ALL)
endif()

add_library(GambleTweaks SHARED
	src/gamble_tweaks.cpp
	src/gamble_tweaks.rc
)
set_target_properties(GambleTweaks PROPERTIES
	OUTPUT_NAME "d2rl-gamble-tweaks"   # produces d2rl-gamble-tweaks.dll
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
)
target_link_libraries(GambleTweaks PRIVATE D2RLPlugin::D2RLPlugin)

if(MSVC)
	target_compile_options(GambleTweaks PRIVATE /W4 /permissive- /EHsc)
endif()

# Embed the default config into the DLL (written to d2rloader/config/ when missing).
d2rlplugin_embed_config(
	TARGET GambleTweaks
	FILE "${CMAKE_CURRENT_SOURCE_DIR}/config/gamble-tweaks.toml"
)

# Build (Visual Studio Community, x64), from this folder with the SDK under
# third_party/PluginSDK:
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
# Output: build/d2rl-gamble-tweaks.dll  →  <game>/d2rloader/plugins/
