cmake_minimum_required(VERSION 3.16)
project(LBTra VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The engine has no dependencies so it can be dropped into the JUCE plugin as-is.
add_library(lbtra_engine STATIC
  engine/Wav.cpp
  engine/Analysis.cpp
  engine/Effects.cpp
  engine/Engine.cpp)
target_include_directories(lbtra_engine PUBLIC engine)
if(MSVC)
  target_compile_options(lbtra_engine PRIVATE /W4 /utf-8)
else()
  target_compile_options(lbtra_engine PRIVATE -Wall -Wextra)
endif()

add_executable(lbtra_render tools/lbtra_render.cpp)
target_link_libraries(lbtra_render PRIVATE lbtra_engine)

add_executable(lbtra_analyze tools/lbtra_analyze.cpp)
target_link_libraries(lbtra_analyze PRIVATE lbtra_engine)
if(MSVC)
  target_compile_options(lbtra_render PRIVATE /utf-8)
  target_compile_options(lbtra_analyze PRIVATE /utf-8)
endif()

# ---- JUCE plugin (VST3 / AU / Standalone). Off by default so the engine + tools build with no deps.
option(LBTRA_BUILD_PLUGIN "Build the JUCE plugin" OFF)
if(LBTRA_BUILD_PLUGIN)
  set(LBTRA_JUCE_DIR "" CACHE PATH "Local JUCE checkout (empty = download JUCE 8.0.4)")
  if(LBTRA_JUCE_DIR)
    add_subdirectory(${LBTRA_JUCE_DIR} juce)
  else()
    include(FetchContent)
    FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.4 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(juce)
  endif()

  set_target_properties(lbtra_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
  set(LBTRA_FORMATS VST3 Standalone)
  if(APPLE)
    list(APPEND LBTRA_FORMATS AU)
  endif()
  juce_add_plugin(LBTra
    COMPANY_NAME "LuciousBB"
    BUNDLE_ID "com.luciousbb.lbtra"
    PLUGIN_MANUFACTURER_CODE Lcbb
    PLUGIN_CODE Lbtr
    FORMATS ${LBTRA_FORMATS}
    PRODUCT_NAME "LBTra"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Sampler
    AU_MAIN_TYPE kAudioUnitType_MusicDevice)
  target_sources(LBTra PRIVATE plugin/PluginProcessor.cpp plugin/PluginEditor.cpp)
  target_compile_definitions(LBTra PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
  target_link_libraries(LBTra PRIVATE lbtra_engine juce::juce_audio_utils juce::juce_audio_formats
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
  if(MSVC)
    target_compile_options(LBTra PRIVATE /utf-8)
  endif()
  # headless host-sync check (needs a desktop toolkit only for juce_gui_basics' link deps)
  add_executable(lbtra_plugin_test plugin/test/plugin_test.cpp)
  target_link_libraries(lbtra_plugin_test PRIVATE LBTra)
  target_include_directories(lbtra_plugin_test PRIVATE $<TARGET_PROPERTY:LBTra,INCLUDE_DIRECTORIES>)
  target_compile_definitions(lbtra_plugin_test PRIVATE $<TARGET_PROPERTY:LBTra,COMPILE_DEFINITIONS>)
endif()
