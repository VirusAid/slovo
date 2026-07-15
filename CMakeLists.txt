cmake_minimum_required(VERSION 3.14)
project(slovo VERSION 0.1.0 LANGUAGES CXX)

add_library(slovo INTERFACE)
add_library(slovo::slovo ALIAS slovo)

target_include_directories(slovo INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(slovo INTERFACE cxx_std_17)

if(WIN32)
    target_link_libraries(slovo INTERFACE winhttp ws2_32)
endif()

option(SLOVO_USE_CURL "Use libcurl as the HTTP transport (required for https on non-Windows)" OFF)
if(SLOVO_USE_CURL)
    find_package(CURL REQUIRED)
    target_compile_definitions(slovo INTERFACE SLOVO_USE_CURL)
    target_link_libraries(slovo INTERFACE CURL::libcurl)
endif()

# ---------------------------------------------------------------------------

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SLOVO_IS_TOP_LEVEL ON)
else()
    set(SLOVO_IS_TOP_LEVEL OFF)
endif()

option(SLOVO_BUILD_TESTS "Build slovo tests" ${SLOVO_IS_TOP_LEVEL})
option(SLOVO_BUILD_EXAMPLES "Build slovo examples" ${SLOVO_IS_TOP_LEVEL})

if(SLOVO_BUILD_TESTS)
    enable_testing()
    add_executable(slovo_tests tests/test_slovo.cpp)
    target_link_libraries(slovo_tests PRIVATE slovo)
    add_test(NAME slovo_tests COMMAND slovo_tests)
endif()

if(SLOVO_BUILD_EXAMPLES)
    foreach(example 01_chat 02_stream 03_tools 04_local)
        add_executable(${example} examples/${example}.cpp)
        target_link_libraries(${example} PRIVATE slovo)
    endforeach()
endif()

# ---------------------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS slovo EXPORT slovoTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT slovoTargets
    NAMESPACE slovo::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/slovo)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/slovoConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/slovoConfig.cmake"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/slovoTargets.cmake\")\n")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/slovoConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/slovoConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/slovo)
