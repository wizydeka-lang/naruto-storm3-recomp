# naruto - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(naruto LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The game is not part of this repository; codegen translates the user's copy.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xex")
    message(FATAL_ERROR
        "assets/default.xex not found. Copy your own extracted game files into "
        "assets/ (see README.md), then run tools/check-game.py.")
endif()

include(generated/rexglue.cmake)

# An Android SDK's rexglue tool is an Android binary; codegen runs on the build
# machine, so it uses the desktop SDK's copy instead.
if(ANDROID)
    set(NARUTO_HOST_REXGLUE "$ENV{HOME}/rexglue-sdk/out/install/linux-amd64/bin/rexglue"
        CACHE FILEPATH "Desktop rexglue tool that runs codegen for Android builds")
    if(NOT EXISTS "${NARUTO_HOST_REXGLUE}")
        message(FATAL_ERROR "Desktop rexglue tool not found at ${NARUTO_HOST_REXGLUE}. "
            "Set up the desktop SDK first, or set NARUTO_HOST_REXGLUE.")
    endif()
    set_target_properties(rex::rexglue PROPERTIES
        IMPORTED_LOCATION "${NARUTO_HOST_REXGLUE}"
        IMPORTED_LOCATION_RELEASE "${NARUTO_HOST_REXGLUE}")
endif()

# Sources
set(NARUTO_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(naruto WIN32 ${NARUTO_SOURCES})
elseif(ANDROID)
    # SDL's Java activity loads the game as libmain.so and calls its main().
    add_library(naruto SHARED ${NARUTO_SOURCES})
    set_target_properties(naruto PROPERTIES OUTPUT_NAME main)
else()
    add_executable(naruto ${NARUTO_SOURCES})
endif()

rexglue_setup_target(naruto GPU_PLUGINS xenos)

# Development aid for mapping the game's Direct3D functions: counts calls to
# each guest function in the D3D address range (tools/guest-call-trace). Use a
# separate build directory; it slows the game down.
option(NARUTO_TRACE_GUEST_CALLS "Count guest function calls (development only)" OFF)
if(NARUTO_TRACE_GUEST_CALLS)
    set(NARUTO_TRACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/guest-call-trace")
    # Only the generated code gets the profiling prologue; its tracy/Tracy.hpp
    # is the stand-in in NARUTO_TRACE_DIR.
    target_compile_definitions(naruto_recomp PRIVATE
        REXGLUE_PROFILE_GUEST_FUNCTIONS REXGLUE_ENABLE_PROFILING)
    target_include_directories(naruto_recomp BEFORE PRIVATE "${NARUTO_TRACE_DIR}")
    target_sources(naruto PRIVATE "${NARUTO_TRACE_DIR}/guest_call_trace.cpp")
    set_source_files_properties("${NARUTO_TRACE_DIR}/guest_call_trace.cpp" PROPERTIES
        INCLUDE_DIRECTORIES "${NARUTO_TRACE_DIR}")
endif()

# Seed the gameplay configuration (it carries rendering fixes) for every
# platform and build type, without overwriting settings saved later.
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/naruto.toml")
    configure_file(config/naruto-performance.toml
        "${CMAKE_CURRENT_BINARY_DIR}/naruto.toml" COPYONLY)
endif()

# Keep project checks outside the SDK-managed generated/rexglue.cmake.
if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Android)$")
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    set(NARUTO_DISPATCH_CHECK
        "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_function_dispatch.py"
        --manifest "${CMAKE_CURRENT_SOURCE_DIR}/naruto_manifest.toml"
        --registration "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/naruto_register.cpp"
    )

    # Runs on each game build, after codegen and before recompilation.
    add_custom_target(naruto_check_dispatch
        COMMAND ${NARUTO_DISPATCH_CHECK}
        DEPENDS naruto_codegen
        COMMENT "Checking configured guest function registration"
        VERBATIM
    )
    add_dependencies(naruto naruto_check_dispatch)
    if(TARGET naruto_recomp)
        add_dependencies(naruto_recomp naruto_check_dispatch)
    endif()

    # A new executable must contain the actual registered function symbols.
    add_custom_command(TARGET naruto POST_BUILD
        COMMAND ${NARUTO_DISPATCH_CHECK}
            --binary "$<TARGET_FILE:naruto>" --nm "${CMAKE_NM}"
        COMMENT "Checking compiled guest function entries"
        VERBATIM
    )
    set_property(TARGET naruto APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_function_dispatch.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/naruto_manifest.toml"
    )

    # Explicitly recheck an existing binary even when no relink is needed.
    add_custom_target(naruto_verify_functions
        COMMAND ${NARUTO_DISPATCH_CHECK}
            --binary "$<TARGET_FILE:naruto>" --nm "${CMAKE_NM}"
        DEPENDS naruto
        VERBATIM
    )
endif()
