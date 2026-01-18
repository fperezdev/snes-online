cmake_minimum_required(VERSION 3.20)
project(snes_online LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SNESONLINE_ENABLE_GGPO "Enable GGPO rollback netcode integration" OFF)
option(SNESONLINE_FETCH_GGPO "Fetch and build GGPO automatically when GGPO is enabled" ON)
set(SNESONLINE_GGPO_GIT_TAG "master" CACHE STRING "Git tag/commit for pond3r/ggpo when fetching")
set(SNESONLINE_GGPO_URL "https://github.com/pond3r/ggpo/archive/refs/heads/master.zip" CACHE STRING "Fallback URL to fetch GGPO source when Git is unavailable")
option(SNESONLINE_BUILD_ANDROID_JNI "Build Android JNI shared library" OFF)
option(SNESONLINE_BUILD_WINDOWS_APP "Build Windows SDL2 runner app" OFF)

add_library(snesonline_core STATIC
    src/AlignedBuffer.cpp
    src/AppConfig.cpp
    src/EmulatorEngine.cpp
    src/LibretroCore.cpp
    src/StunClient.cpp
)

target_include_directories(snesonline_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(snesonline_core PRIVATE
    SNESONLINE_CORE_BUILD=1
)

# Netplay wrapper. Builds without bundling GGPO. If SNESONLINE_ENABLE_GGPO=ON, you must provide GGPO headers/libs.
add_library(snesonline_netplay STATIC
    src/NetplaySession.cpp
    src/LockstepSession.cpp
    src/GGPOCallbacks.cpp
)
target_include_directories(snesonline_netplay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(snesonline_netplay PUBLIC snesonline_core)

if(SNESONLINE_ENABLE_GGPO)
    target_compile_definitions(snesonline_netplay PUBLIC SNESONLINE_ENABLE_GGPO=1)

    if(SNESONLINE_FETCH_GGPO)
        if(POLICY CMP0169)
            cmake_policy(SET CMP0169 OLD)
        endif()

        include(FetchContent)

        find_package(Git QUIET)

        if(GIT_FOUND)
            FetchContent_Declare(
                snesonline_ggpo_src
                GIT_REPOSITORY https://github.com/pond3r/ggpo.git
                GIT_TAG ${SNESONLINE_GGPO_GIT_TAG}
            )
        else()
            message(STATUS "Git not found; fetching GGPO from ${SNESONLINE_GGPO_URL}")
            FetchContent_Declare(
                snesonline_ggpo_src
                URL ${SNESONLINE_GGPO_URL}
            )
        endif()

        FetchContent_GetProperties(snesonline_ggpo_src)
        if(NOT snesonline_ggpo_src_POPULATED)
            FetchContent_Populate(snesonline_ggpo_src)
        endif()

        # Build GGPO as a static library without invoking GGPO's top-level CMakeLists.
        # This avoids GGPO forcing CMAKE_CONFIGURATION_TYPES globally.
        set(_GGPO_SRC_DIR "${snesonline_ggpo_src_SOURCE_DIR}/src")
        include("${_GGPO_SRC_DIR}/CMakeSources.cmake")
        set(_GGPO_SOURCES ${GGPO_LIB_SRC})
        list(TRANSFORM _GGPO_SOURCES PREPEND "${_GGPO_SRC_DIR}/")

        add_library(GGPO STATIC ${_GGPO_SOURCES})
        add_library(GGPO::GGPO ALIAS GGPO)

        target_include_directories(GGPO PUBLIC
            "${_GGPO_SRC_DIR}/include"
            "${_GGPO_SRC_DIR}/lib/ggpo"
        )

        if(WIN32)
            target_link_libraries(GGPO PUBLIC winmm ws2_32)
        endif()

        target_link_libraries(snesonline_netplay PUBLIC GGPO::GGPO)
    else()
        # Advanced mode: user supplies GGPO headers + libs via toolchain.
        # We keep this path for custom GGPO forks/builds.
        find_path(SNESONLINE_GGPO_INCLUDE_DIR ggponet.h)
        find_library(SNESONLINE_GGPO_LIBRARY NAMES GGPO ggpo)
        if(NOT SNESONLINE_GGPO_INCLUDE_DIR OR NOT SNESONLINE_GGPO_LIBRARY)
            message(FATAL_ERROR "SNESONLINE_ENABLE_GGPO is ON but GGPO was not found. Either enable SNESONLINE_FETCH_GGPO, or provide ggponet.h + GGPO library via your toolchain.")
        endif()
        target_include_directories(snesonline_netplay PUBLIC "${SNESONLINE_GGPO_INCLUDE_DIR}")
        target_link_libraries(snesonline_netplay PUBLIC "${SNESONLINE_GGPO_LIBRARY}")
    endif()
endif()

if(SNESONLINE_BUILD_ANDROID_JNI)
    add_subdirectory(platform/android)
endif()

if(SNESONLINE_BUILD_WINDOWS_APP AND WIN32)
    add_subdirectory(platform/windows)
endif()

# ---- Install / Packaging ----
include(GNUInstallDirs)

if(SNESONLINE_BUILD_WINDOWS_APP AND TARGET snesonline_win)
    # Install MSVC runtime DLLs alongside the exe (app-local) when needed.
    if(MSVC)
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS ON)
        # Put the runtime DLLs next to the executable for a portable folder/zip.
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
        include(InstallRequiredSystemLibraries)
    endif()

    install(TARGETS snesonline_win
        RUNTIME DESTINATION .
    )

    if(TARGET snesonline_config)
        install(TARGETS snesonline_config
            RUNTIME DESTINATION .
        )
    endif()

    # Create a ROM directory inside the install folder.
    install(DIRECTORY DESTINATION roms)

    # Include any Libretro cores that exist in the workspace (e.g. cores/snes9x_libretro.dll).
    # This keeps the installed build runnable without having to re-download cores.
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/cores/
        DESTINATION cores
        FILES_MATCHING
            PATTERN "*.dll"
            PATTERN ".gitkeep" EXCLUDE
    )

    # End-user portable package instructions.
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/README_PORTABLE.txt DESTINATION . RENAME README.txt)

    set(CPACK_PACKAGE_NAME "snes-online")
    set(CPACK_PACKAGE_VENDOR "snes-online")
    set(CPACK_PACKAGE_VERSION "0.1.0")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "snes-online")

    # Portable package (zip) with a top-level folder.
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    set(CPACK_PACKAGE_FILE_NAME "snes-online-${CPACK_PACKAGE_VERSION}-win64-portable")

    if(WIN32)
        set(CPACK_GENERATOR "ZIP")
    endif()

    include(CPack)
endif()
