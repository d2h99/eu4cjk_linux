cmake_minimum_required(VERSION 3.16)
project(eu4cjk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_library(eu4cjk_core OBJECT
    src/byte_pattern.cpp
    src/version.cpp
    src/font.cpp
    src/stubgen.cpp
    src/render.cpp
    src/render_fetch.cpp
    src/render_trace.cpp
    src/render_measure.cpp
    src/render_mapfix.cpp
    src/render_curve.cpp
    src/escape.cpp
    src/savefix.cpp
    src/wrapfix.cpp
)

add_library(eu4cjk SHARED
    src/dllmain.cpp
    $<TARGET_OBJECTS:eu4cjk_core>
)

set_target_properties(eu4cjk PROPERTIES
    OUTPUT_NAME eu4cjk
)

foreach(tgt eu4cjk_core eu4cjk)
    target_compile_options(${tgt} PRIVATE -Wall -Wextra)
    set_target_properties(${tgt} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
endforeach()

target_link_options(eu4cjk PRIVATE -static-libgcc -static-libstdc++ -Wl,--exclude-libs=ALL)

# Build stamp for the injection cache mystery: distinguishes "new .so ran"
# from "stale build / Steam launch-options lag" in one log line.
find_package(Git QUIET)
set(EU4CJK_GIT_ID "unknown")
if(Git_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty --dirty=-DIRTY
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE EU4CJK_GIT_ID OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
string(TIMESTAMP EU4CJK_BUILD_TIME "%Y-%m-%d %H:%M:%S")

target_compile_definitions(eu4cjk PRIVATE
    EU4CJK_VERSION="1.1.0-M6"
    EU4CJK_BUILD_STAMP="${EU4CJK_GIT_ID} ${EU4CJK_BUILD_TIME}"
)

add_executable(eu4cjk_selftest
    test/selftest.cpp
    test/test_fetch.cpp
    test/test_stub.cpp
    test/test_full_stub.cpp
    test/test_measure.cpp
    test/test_wrap.cpp
    test/test_curve.cpp
    test/test_escape.cpp
    test/stub_log.cpp
    $<TARGET_OBJECTS:eu4cjk_core>
)
target_compile_definitions(eu4cjk_selftest PRIVATE
    EU4CJK_VERSION="selftest"
    EU4CJK_BUILD_STAMP="${EU4CJK_GIT_ID} ${EU4CJK_BUILD_TIME}"
)

target_compile_options(eu4cjk_selftest PRIVATE -Wall -Wextra)
# Non-PIE like the game: keeps rel32 stub targets reachable from the low
# mmap'd relay pages (a PIE test binary overflows rel32 and would trip the
# guard added to stubgen).
target_link_options(eu4cjk_selftest PRIVATE -no-pie)

enable_testing()
add_test(NAME selftest COMMAND eu4cjk_selftest)

# Release artifact: strip debug sections (identical runtime behavior).
# Produces build/libeu4cjk.so.release + build/libeu4cjk.so.debug
# (keep the .debug file for offline backtrace symbolication).
find_program(OBJCOPY_EXECUTABLE objcopy)
if(OBJCOPY_EXECUTABLE)
    add_custom_target(eu4cjk_release
        COMMAND ${OBJCOPY_EXECUTABLE} --only-keep-debug
                $<TARGET_FILE:eu4cjk> ${CMAKE_BINARY_DIR}/libeu4cjk.so.debug
        COMMAND ${OBJCOPY_EXECUTABLE} --strip-debug
                $<TARGET_FILE:eu4cjk> ${CMAKE_BINARY_DIR}/libeu4cjk.so.release
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/release
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_BINARY_DIR}/libeu4cjk.so.release ${CMAKE_SOURCE_DIR}/release/libeu4cjk.so
        COMMENT "Stripping release artifact -> release/libeu4cjk.so"
    )
endif()
