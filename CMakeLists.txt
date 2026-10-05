cmake_minimum_required(VERSION 3.20)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(Hirari LANGUAGES CXX)

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
    hirari/wire.cpp
    hirari/unity_client.cpp
    hirari/unity.cpp
    hirari/world.cpp
)

target_include_directories(hirari_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(hirari_runtime
    PUBLIC
        seed
)

add_executable(hirari
    hirari/main.cpp
)

target_link_libraries(hirari
    PRIVATE
        hirari_runtime
)

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
    target_compile_definitions(hirari_runtime PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX)
endif()
if(MSVC)
    target_compile_options(seed PRIVATE /utf-8)
    target_compile_options(hirari_runtime PRIVATE /utf-8)
    target_compile_options(hirari PRIVATE /utf-8)
endif()


include(CTest)
if(BUILD_TESTING)
    add_executable(seed_tests tests/seed_tests.cpp)
    target_link_libraries(seed_tests PRIVATE seed)
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
        set_tests_properties(hirari_integration PROPERTIES TIMEOUT 30)
    endif()
    add_test(NAME hirari_eval_name COMMAND hirari --eval ":foo")
    set_tests_properties(hirari_eval_name PROPERTIES PASS_REGULAR_EXPRESSION ":foo")
    add_test(NAME hirari_eval_failure COMMAND hirari --eval "missing_neutral_program arg")
    set_tests_properties(hirari_eval_failure PROPERTIES WILL_FAIL TRUE)
    add_test(NAME hirari_cli_usage COMMAND hirari --unknown)
    set_tests_properties(hirari_cli_usage PROPERTIES WILL_FAIL TRUE)
endif()
