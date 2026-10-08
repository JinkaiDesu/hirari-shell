cmake_minimum_required(VERSION 3.24)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(Hirari LANGUAGES C CXX)

include(cmake/RegexLibrary.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(seed STATIC
    seed/seed.cpp
)

target_include_directories(seed
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(hirari_runtime STATIC
    hirari/conpty.cpp
    hirari/process.cpp
    hirari/shell.cpp
    hirari/shell_help.cpp
    hirari/wire.cpp
    hirari/unity_client.cpp
    hirari/unity.cpp
    hirari/world.cpp
    hirari/vocabulary.cpp
    hirari/values.cpp
    hirari/regex.cpp
)

target_include_directories(hirari_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(hirari_runtime
    PUBLIC
        seed
        pcre2-8-static
)

add_executable(hirari
    hirari/main.cpp
    hirari/installation.cpp
)

target_link_libraries(hirari
    PRIVATE
        hirari_runtime
)
add_custom_command(TARGET hirari POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/library" "$<TARGET_FILE_DIR:hirari>/Commands"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/Licenses" "$<TARGET_FILE_DIR:hirari>/Licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/regex-library.lock.json" "$<TARGET_FILE_DIR:hirari>/Licenses/regex-library.lock.json")

if (MSVC)
    target_compile_options(seed PRIVATE
        /W4
        /permissive-
    )

    target_compile_options(hirari_runtime PRIVATE
        /W4
        /permissive-
    )

    target_compile_options(hirari PRIVATE
        /W4
        /permissive-
    )
else()
    target_compile_options(seed PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    target_compile_options(hirari_runtime PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    target_compile_options(hirari PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()
if(WIN32)
    enable_language(RC)
    find_program(HIRARI_POWERSHELL NAMES pwsh powershell REQUIRED)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/hirari.ico"
        COMMAND "${HIRARI_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-icon.ps1" -Source "${CMAKE_CURRENT_SOURCE_DIR}/icon.png" -Output "${CMAKE_CURRENT_BINARY_DIR}/hirari.ico"
        DEPENDS icon.png tools/build-icon.ps1 VERBATIM)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/hirari.rc" CONTENT "1 ICON \"${CMAKE_CURRENT_BINARY_DIR}/hirari.ico\"\n")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/hirari.rc" PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/hirari.ico")
    target_sources(hirari PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/hirari.rc" "${CMAKE_CURRENT_BINARY_DIR}/hirari.ico")
    target_link_libraries(hirari PRIVATE shell32 ole32)
    target_compile_definitions(hirari_runtime PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX)
endif()
if(MSVC)
    target_compile_options(seed PRIVATE /utf-8)
    target_compile_options(hirari_runtime PRIVATE /utf-8)
    target_compile_options(hirari PRIVATE /utf-8)
endif()


include(CTest)
option(SEED_DEFERRED_EXPERIMENT "Build isolated deferred-substitution experiment executables" OFF)
if(BUILD_TESTING)
    add_executable(hirari_capability_tests tests/capability_tests.cpp)
    target_link_libraries(hirari_capability_tests PRIVATE hirari_runtime)
    if(MSVC)
        target_compile_options(hirari_capability_tests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME hirari_capabilities COMMAND hirari_capability_tests)
    add_executable(seed_tests tests/seed_tests.cpp)
    target_link_libraries(seed_tests PRIVATE seed)
    add_executable(seed_performance tests/seed_performance.cpp)
    target_link_libraries(seed_performance PRIVATE seed)
    # Keep diagnostic counters out of the production Seed/Hirari binaries.
    add_library(seed_profile STATIC seed/seed.cpp)
    target_include_directories(seed_profile PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(seed_profile PUBLIC SEED_PROFILE)
    add_executable(seed_performance_profile tests/seed_performance.cpp)
    target_link_libraries(seed_performance_profile PRIVATE seed_profile)
    if(MSVC)
        target_compile_options(seed_profile PRIVATE /W4 /permissive- /utf-8)
        target_compile_options(seed_performance PRIVATE /W4 /permissive- /utf-8)
        target_compile_options(seed_performance_profile PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(seed_profile PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(seed_performance PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(seed_performance_profile PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME seed_semantics COMMAND seed_tests)
    if(WIN32)
        add_executable(hirari_test_program tests/test_program.cpp)
        target_link_libraries(hirari_test_program PRIVATE hirari_runtime)
        add_executable(hirari_integration_tests tests/integration_tests.cpp)
        target_link_libraries(hirari_integration_tests PRIVATE hirari_runtime)
        if(MSVC)
            target_compile_options(seed_tests PRIVATE /W4 /permissive- /utf-8)
            target_compile_options(hirari_test_program PRIVATE /W4 /permissive- /utf-8)
            target_compile_options(hirari_integration_tests PRIVATE /W4 /permissive- /utf-8)
        endif()
        add_test(NAME hirari_integration COMMAND hirari_integration_tests $<TARGET_FILE:hirari_test_program>)
        add_executable(hirari_shell_completion_tests tests/shell_completion_tests.cpp)
        target_link_libraries(hirari_shell_completion_tests PRIVATE hirari_runtime)
        if(MSVC)
            target_compile_options(hirari_shell_completion_tests PRIVATE /W4 /permissive- /utf-8)
        endif()
        add_test(NAME hirari_shell_completion COMMAND hirari_shell_completion_tests $<TARGET_FILE:hirari_test_program>)
        set_tests_properties(hirari_shell_completion PROPERTIES TIMEOUT 30)
        set_tests_properties(hirari_integration PROPERTIES TIMEOUT 30)
    endif()
    add_test(NAME hirari_eval_name COMMAND hirari --eval "-foo")
    set_tests_properties(hirari_eval_name PROPERTIES PASS_REGULAR_EXPRESSION "-foo")
    add_test(NAME hirari_eval_failure COMMAND hirari --eval "missing_neutral_program arg")
    set_tests_properties(hirari_eval_failure PROPERTIES WILL_FAIL TRUE)
    add_test(NAME hirari_cli_usage COMMAND hirari --unknown)
    set_tests_properties(hirari_cli_usage PROPERTIES WILL_FAIL TRUE)

    if(SEED_DEFERRED_EXPERIMENT)
    # Opt in by launching these separate executables. Production Seed/Hirari
    # remain the literal-substitution reference, without experimental payloads.
    add_library(seed_deferred STATIC seed/seed.cpp)
    target_include_directories(seed_deferred PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(seed_deferred PUBLIC SEED_DEFERRED_EXPERIMENT)
    add_executable(seed_tests_deferred tests/seed_tests.cpp)
    target_link_libraries(seed_tests_deferred PRIVATE seed_deferred)
    add_test(NAME seed_semantics_deferred COMMAND seed_tests_deferred)
    add_executable(seed_performance_deferred tests/seed_performance.cpp)
    target_link_libraries(seed_performance_deferred PRIVATE seed_deferred)
    add_library(seed_deferred_profile STATIC seed/seed.cpp)
    target_include_directories(seed_deferred_profile PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(seed_deferred_profile PUBLIC SEED_DEFERRED_EXPERIMENT SEED_PROFILE)
    add_executable(seed_performance_deferred_profile tests/seed_performance.cpp)
    target_link_libraries(seed_performance_deferred_profile PRIVATE seed_deferred_profile)
    if(WIN32)
        get_target_property(hirari_runtime_sources hirari_runtime SOURCES)
        add_library(hirari_deferred_runtime STATIC ${hirari_runtime_sources})
        target_include_directories(hirari_deferred_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(hirari_deferred_runtime PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX)
        target_link_libraries(hirari_deferred_runtime PUBLIC seed_deferred pcre2-8-static)
        add_executable(hirari_deferred hirari/main.cpp hirari/installation.cpp)
        target_link_libraries(hirari_deferred PRIVATE hirari_deferred_runtime)
        add_executable(seed_deferred_tests tests/deferred_substitution_tests.cpp)
        target_link_libraries(seed_deferred_tests PRIVATE hirari_deferred_runtime)
        add_test(NAME seed_deferred_differential COMMAND seed_deferred_tests)
        set_tests_properties(seed_deferred_differential PROPERTIES TIMEOUT 60)
        add_executable(hirari_shell_performance tests/shell_performance.cpp)
        target_link_libraries(hirari_shell_performance PRIVATE hirari_deferred_runtime)
        add_executable(hirari_shell_completion_deferred tests/shell_completion_tests.cpp)
        target_link_libraries(hirari_shell_completion_deferred PRIVATE hirari_deferred_runtime)
        add_test(NAME hirari_shell_completion_deferred COMMAND hirari_shell_completion_deferred $<TARGET_FILE:hirari_test_program>)
        set_tests_properties(hirari_shell_completion_deferred PROPERTIES TIMEOUT 30)
    endif()
    foreach(target seed_deferred seed_tests_deferred seed_performance_deferred seed_deferred_profile seed_performance_deferred_profile hirari_deferred_runtime hirari_deferred seed_deferred_tests hirari_shell_completion_deferred hirari_shell_performance)
        if(TARGET ${target})
            if(MSVC)
                target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
            else()
                target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
            endif()
        endif()
    endforeach()
    endif()
endif()
