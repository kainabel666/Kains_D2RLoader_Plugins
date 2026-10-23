# CMake generated Testfile for 
# Source directory: M:/Tools/RE/d2rl-plugins/third_party/PluginSDK/tests
# Build directory: M:/Tools/RE/d2rl-plugins/auto-identify/build/PluginSDK/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[D2RLPluginContracts]=] "M:/Tools/RE/d2rl-plugins/auto-identify/build/PluginSDK/tests/D2RLPluginContractTests.exe")
set_tests_properties([=[D2RLPluginContracts]=] PROPERTIES  LABELS "offline;sdk" TIMEOUT "60" _BACKTRACE_TRIPLES "M:/Tools/RE/d2rl-plugins/third_party/PluginSDK/tests/CMakeLists.txt;21;add_test;M:/Tools/RE/d2rl-plugins/third_party/PluginSDK/tests/CMakeLists.txt;0;")
add_test([=[D2RLPluginServices]=] "M:/Tools/RE/d2rl-plugins/auto-identify/build/PluginSDK/tests/D2RLPluginServiceTests.exe")
set_tests_properties([=[D2RLPluginServices]=] PROPERTIES  LABELS "offline;sdk" TIMEOUT "60" _BACKTRACE_TRIPLES "M:/Tools/RE/d2rl-plugins/third_party/PluginSDK/tests/CMakeLists.txt;24;add_test;M:/Tools/RE/d2rl-plugins/third_party/PluginSDK/tests/CMakeLists.txt;0;")
