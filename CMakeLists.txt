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

# Sources
set(NARUTO_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(naruto WIN32 ${NARUTO_SOURCES})
else()
    add_executable(naruto ${NARUTO_SOURCES})
endif()

rexglue_setup_target(naruto GPU_PLUGINS xenos)

# Seed the gameplay configuration (it carries rendering fixes) for every
# platform and build type, without overwriting settings saved later.
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/naruto.toml")
    configure_file(config/naruto-performance.toml
        "${CMAKE_CURRENT_BINARY_DIR}/naruto.toml" COPYONLY)
endif()

# Keep project checks outside the SDK-managed generated/rexglue.cmake.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
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
