cmake_minimum_required(VERSION 3.14)

project(aura
    VERSION 0.6.0
    DESCRIPTION "Self-tuning async I/O library for Linux using io_uring"
    LANGUAGES C
)

# --- Library target -----------------------------------------------------------

add_library(aura
    engine/src/aura.c
    engine/src/adaptive_engine.c
    engine/src/adaptive_ring.c
    engine/src/adaptive_buffer.c
    engine/src/log.c
)

target_include_directories(aura
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/engine/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/engine/src
)

set_target_properties(aura PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# Shared-library visibility settings
if(BUILD_SHARED_LIBS)
    target_compile_definitions(aura PRIVATE AURA_SHARED_BUILD)
    set_target_properties(aura PROPERTIES C_VISIBILITY_PRESET hidden)
else()
    target_compile_definitions(aura PUBLIC AURA_STATIC_BUILD)
endif()

# --- Dependencies -------------------------------------------------------------

# pthreads
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(aura PUBLIC Threads::Threads)

# liburing via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing)
target_link_libraries(aura PUBLIC PkgConfig::URING)

# libm (floor() in adaptive_engine.c)
target_link_libraries(aura PRIVATE m)

# --- Install support ----------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS aura
    EXPORT auraTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES engine/include/aura.h engine/include/aura.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY engine/include/aura/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aura
)

install(EXPORT auraTargets
    FILE auraTargets.cmake
    NAMESPACE aura::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aura
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/auraConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/auraConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/auraConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aura
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/auraConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/auraConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aura
)
