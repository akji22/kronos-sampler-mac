cmake_minimum_required(VERSION 3.22)

project(KonosSampler VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KONOS_BUILD_JUCE_PLUGIN "Build JUCE VST3/AU/Standalone plugin targets" ON)
option(KONOS_BUILD_TESTS "Build standalone C++20 DSP & Format automated test suite" ON)
option(KONOS_FETCH_JUCE "Automatically fetch JUCE 8.0.4 via FetchContent if not installed locally" OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /fp:fast)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -fno-math-errno)
endif()

set(KONOS_CORE_SOURCES
    Source/AudioEngine/SamplerEngine.cpp
    Source/Voices/SamplerVoice.cpp
    Source/Streaming/DiskSampleStreamer.cpp
    Source/Effects/EffectsChain.cpp
    Source/Browser/LibraryBrowserModel.cpp
    Source/Formats/WavAiffFormatLoader.cpp
    Source/Formats/SfzFormatLoader.cpp
    Source/Formats/Sf2FormatLoader.cpp
    Source/Formats/KontaktFormatInspector.cpp
)

set(KONOS_CORE_HEADERS
    Source/AudioEngine/SamplerParameters.h
    Source/AudioEngine/SamplerEngine.h
    Source/AudioEngine/LockFreeCommandQueue.h
    Source/SamplePlayback/SampleBuffer.h
    Source/Streaming/DiskSampleStreamer.h
    Source/Voices/AdsrEnvelope.h
    Source/Voices/StateVariableFilter.h
    Source/Voices/SamplerVoice.h
    Source/Modulation/ModulationMatrix.h
    Source/Effects/EffectsChain.h
    Source/Browser/LibraryBrowserModel.h
    Source/Instruments/InstrumentPatch.h
    Source/Formats/IFormatAdapter.h
    Source/Formats/WavAiffFormatLoader.h
    Source/Formats/SfzFormatLoader.h
    Source/Formats/Sf2FormatLoader.h
    Source/Formats/KontaktFormatInspector.h
    Source/State/PresetStateManager.h
)

add_library(KonosCore STATIC ${KONOS_CORE_SOURCES} ${KONOS_CORE_HEADERS})
target_include_directories(KonosCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Source)

if(KONOS_BUILD_TESTS)
    enable_testing()
    add_executable(KonosEngineTests Tests/TestMain.cpp)
    target_link_libraries(KonosEngineTests PRIVATE KonosCore)
    add_test(NAME KonosEngineTests COMMAND KonosEngineTests)
endif()

if(KONOS_BUILD_JUCE_PLUGIN)
    if(KONOS_FETCH_JUCE)
        include(FetchContent)
        FetchContent_Declare(
            JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG        8.0.4
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(JUCE)
    elseif(DEFINED JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
        add_subdirectory("${JUCE_DIR}" JUCE_build EXCLUDE_FROM_ALL)
    else()
        find_package(JUCE CONFIG QUIET)
    endif()

    if(COMMAND juce_add_plugin)
        set(KONOS_PLUGIN_FORMATS VST3 Standalone)
        if(APPLE)
            list(APPEND KONOS_PLUGIN_FORMATS AU)
        endif()

        juce_add_plugin(KonosSampler
            COMPANY_NAME                "Konos Audio Labs"
            COMPANY_WEBSITE             "https://konosaudio.example.com"
            COMPANY_EMAIL               "engineering@konosaudio.example.com"
            IS_SYNTH                    TRUE
            NEEDS_MIDI_INPUT            TRUE
            NEEDS_MIDI_OUTPUT           FALSE
            IS_MIDI_EFFECT              FALSE
            EDITOR_WANTS_KEYBOARD_FOCUS TRUE
            COPY_PLUGIN_AFTER_BUILD     FALSE
            PLUGIN_MANUFACTURER_CODE    Knos
            PLUGIN_CODE                 Ksp1
            FORMATS                     ${KONOS_PLUGIN_FORMATS}
            VST3_CATEGORIES             "Instrument" "Sampler" "Synth"
            AU_MAIN_TYPE                "kAudioUnitType_MusicDevice"
            PRODUCT_NAME                "Konos Sampler"
        )

        target_sources(KonosSampler
            PRIVATE
                Source/Plugin/PluginProcessor.cpp
                Source/Plugin/PluginProcessor.h
                Source/UI/WorkstationLookAndFeel.h
                Source/UI/PluginEditor.cpp
                Source/UI/PluginEditor.h
        )

        target_include_directories(KonosSampler
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/Source
        )

        target_compile_definitions(KonosSampler
            PUBLIC
                JUCE_WEB_BROWSER=0
                JUCE_USE_CURL=0
                JUCE_VST3_CAN_REPLACE_VST2=0
                JUCE_DISPLAY_SPLASH_SCREEN=0
        )

        target_link_libraries(KonosSampler
            PRIVATE
                KonosCore
                juce::juce_audio_utils
                juce::juce_audio_processors
                juce::juce_audio_formats
                juce::juce_audio_devices
                juce::juce_dsp
                juce::juce_gui_basics
                juce::juce_gui_extra
            PUBLIC
                juce::juce_recommended_config_flags
                juce::juce_recommended_lto_flags
                juce::juce_recommended_warning_flags
        )
    else()
        message(STATUS "JUCE not found. Built KonosCore and KonosEngineTests only.")
    endif()
endif()
