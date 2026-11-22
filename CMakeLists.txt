cmake_minimum_required(VERSION 3.24)

if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "Allow older FetchContent dependencies under CMake 4+" FORCE)
endif()

project(StyleScaler VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(STYLE_SCALER_BUILD_TESTS "Build Catch2 and plugin unit tests" ON)
option(STYLE_SCALER_BUILD_CLI "Build style_cli offline MIDI exporter" ON)
option(STYLE_SCALER_BUILD_PLUGIN "Build JUCE 8 VST3 and Standalone plugin" ON)
option(STYLE_SCALER_ENABLE_MUSCRIPTOR "Integrate DamRsn/muscriptor.cpp neural audio-to-MIDI engine" ON)

include(FetchContent)

# --- nlohmann/json (used by style_core) ---
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# --- Pure C++23 core library (zero JUCE dependency) ---
add_library(style_core STATIC
    core/src/Types.cpp
    core/src/StyleProfile.cpp
    core/src/SmfIO.cpp
    core/src/MidiStyleAnalyzer.cpp
    core/src/StyleBlender.cpp
    core/src/GenerationEngine.cpp
)

target_include_directories(style_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
)
target_link_libraries(style_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(style_core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_definitions(style_core PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(style_core PRIVATE /W4 /utf-8 /wd4100 /wd4244 /wd4267)
else()
    target_compile_options(style_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- CLI .mid exporter & analyzer ---
if(STYLE_SCALER_BUILD_CLI AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cli/main.cpp")
    add_executable(style_cli cli/main.cpp)
    target_link_libraries(style_cli PRIVATE style_core)
    if(MSVC)
        target_compile_options(style_cli PRIVATE /W4 /utf-8 /wd4100)
    else()
        target_compile_options(style_cli PRIVATE -Wall -Wextra)
    endif()
endif()

# --- Catch2 core tests ---
if(STYLE_SCALER_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/core_tests.cpp")
        add_executable(style_tests tests/core_tests.cpp)
        target_link_libraries(style_tests PRIVATE style_core Catch2::Catch2WithMain)
        target_compile_definitions(style_tests PRIVATE
            STYLE_SCALER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        )
        if(MSVC)
            target_compile_options(style_tests PRIVATE /W4 /utf-8 /wd4100)
        else()
            target_compile_options(style_tests PRIVATE -Wall -Wextra)
        endif()

        add_test(NAME style_core_tests COMMAND style_tests)
    endif()
endif()

# --- JUCE 8 Plugin + NeuralNote/muscriptor.cpp (Phase 3-4) ---
if(STYLE_SCALER_BUILD_PLUGIN AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugin/PluginProcessor.cpp")
    # Resolve JUCE: explicit JUCE_DIR -> local third_party/JUCE -> sibling auto-eq-saturator -> FetchContent
    if(DEFINED JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
        message(STATUS "Using JUCE from JUCE_DIR: ${JUCE_DIR}")
        add_subdirectory("${JUCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/juce_build")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
        message(STATUS "Using local third_party/JUCE")
        add_subdirectory(third_party/JUCE)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../auto-eq-saturator/third_party/JUCE/CMakeLists.txt")
        message(STATUS "Reusing sibling JUCE 8.0.15 from ../auto-eq-saturator/third_party/JUCE")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../auto-eq-saturator/third_party/JUCE"
                         "${CMAKE_CURRENT_BINARY_DIR}/juce_build")
    else()
        message(STATUS "Fetching JUCE 8.0.15 via FetchContent")
        FetchContent_Declare(
            JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 8.0.15
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(JUCE)
    endif()

    set(STYLE_SCALER_HAS_MUSCRIPTOR 0)
    if(STYLE_SCALER_ENABLE_MUSCRIPTOR)
        if(DEFINED BUILD_TESTING)
            set(SS_SAVED_BUILD_TESTING ${BUILD_TESTING})
        endif()
        if(DEFINED BUILD_SHARED_LIBS)
            set(SS_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
        endif()

        set(BUILD_TESTING OFF)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(MUSCRIPTOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(MUSCRIPTOR_BUILD_BENCH OFF CACHE BOOL "" FORCE)
        set(MUSCRIPTOR_VULKAN OFF CACHE BOOL "" FORCE)

        FetchContent_Declare(
            muscriptor_cpp
            GIT_REPOSITORY https://github.com/DamRsn/muscriptor.cpp.git
            GIT_TAG main
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR cpp
        )
        FetchContent_MakeAvailable(muscriptor_cpp)

        if(DEFINED SS_SAVED_BUILD_TESTING)
            set(BUILD_TESTING ${SS_SAVED_BUILD_TESTING})
        else()
            unset(BUILD_TESTING)
        endif()
        if(DEFINED SS_SAVED_BUILD_SHARED_LIBS)
            set(BUILD_SHARED_LIBS ${SS_SAVED_BUILD_SHARED_LIBS} CACHE BOOL "" FORCE)
        else()
            unset(BUILD_SHARED_LIBS CACHE)
        endif()

        if(TARGET muscriptor_ggml)
            set(STYLE_SCALER_HAS_MUSCRIPTOR 1)
            foreach(_mtarget muscriptor_ggml ggml ggml-base ggml-cpu pffft)
                if(TARGET ${_mtarget})
                    if(MSVC)
                        target_compile_options(${_mtarget} PRIVATE /O2)
                    endif()
                endif()
            endforeach()
        endif()
    endif()

    file(GLOB STYLE_SCALER_PRESET_FILES "${CMAKE_CURRENT_SOURCE_DIR}/presets/styles/*.json")
    juce_add_binary_data(style_preset_data
        NAMESPACE StylePresetData
        HEADER_NAME StylePresetData.h
        SOURCES ${STYLE_SCALER_PRESET_FILES}
    )

    file(GLOB_RECURSE STYLE_SCALER_PLUGIN_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin/*.h"
    )

    juce_add_plugin(StyleScaler
        COMPANY_NAME "JResearch"
        BUNDLE_ID "com.jresearch.stylescaler"
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        PLUGIN_MANUFACTURER_CODE JkRs
        PLUGIN_CODE StSc
        FORMATS VST3 Standalone
        PRODUCT_NAME "StyleScaler"
    )

    juce_generate_juce_header(StyleScaler)

    target_sources(StyleScaler PRIVATE ${STYLE_SCALER_PLUGIN_SOURCES})
    target_include_directories(StyleScaler PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/plugin")
    target_compile_definitions(StyleScaler PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        STYLE_SCALER_HAS_MUSCRIPTOR=${STYLE_SCALER_HAS_MUSCRIPTOR}
    )
    if(MSVC)
        target_compile_options(StyleScaler PRIVATE /W4 /utf-8 /wd4100 /wd4244 /wd4267 /wd4458)
    else()
        target_compile_options(StyleScaler PRIVATE -Wall -Wextra)
    endif()

    target_link_libraries(StyleScaler PRIVATE
        style_core
        style_preset_data
        juce::juce_audio_utils
        juce::juce_cryptography
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
    )
    if(STYLE_SCALER_HAS_MUSCRIPTOR)
        target_link_libraries(StyleScaler PRIVATE muscriptor_ggml)
    endif()

    if(STYLE_SCALER_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/plugin_tests.cpp")
        juce_add_console_app(style_plugin_tests
            PRODUCT_NAME "style_plugin_tests"
        )
        juce_generate_juce_header(style_plugin_tests)
        target_sources(style_plugin_tests PRIVATE
            tests/plugin_tests.cpp
            ${STYLE_SCALER_PLUGIN_SOURCES}
        )
        target_include_directories(style_plugin_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/plugin")
        target_compile_definitions(style_plugin_tests PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
            STYLE_SCALER_HAS_MUSCRIPTOR=${STYLE_SCALER_HAS_MUSCRIPTOR}
            STYLE_SCALER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        )
        if(MSVC)
            target_compile_options(style_plugin_tests PRIVATE /W4 /utf-8 /wd4100 /wd4244 /wd4267 /wd4458)
        endif()
        target_link_libraries(style_plugin_tests PRIVATE
            style_core
            style_preset_data
            Catch2::Catch2
            juce::juce_audio_utils
            juce::juce_cryptography
            juce::juce_dsp
            juce::juce_recommended_config_flags
        )
        if(STYLE_SCALER_HAS_MUSCRIPTOR)
            target_link_libraries(style_plugin_tests PRIVATE muscriptor_ggml)
        endif()

        add_test(NAME style_plugin_tests COMMAND style_plugin_tests)
    endif()
endif()
