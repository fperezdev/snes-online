cmake_minimum_required(VERSION 3.20)

include(FetchContent)

option(SNESONLINE_FETCH_SDL2 "Fetch SDL2 automatically if not found" ON)

# Prefer an installed SDL2 first.
find_package(SDL2 CONFIG QUIET)

if(NOT SDL2_FOUND AND SNESONLINE_FETCH_SDL2)
    # Use a release archive so git is not required.
    FetchContent_Declare(
        SDL2
        URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.10/SDL2-2.30.10.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL2)
endif()

add_executable(snesonline_win WIN32
    ConfigDialog.cpp
    main.cpp
    winmain.cpp
    resources.rc
)

target_link_libraries(snesonline_win PRIVATE snesonline_core)
target_link_libraries(snesonline_win PRIVATE snesonline_netplay)

target_include_directories(snesonline_win PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

if(TARGET SDL2::SDL2)
    target_link_libraries(snesonline_win PRIVATE SDL2::SDL2)
elseif(TARGET SDL2-static)
    target_link_libraries(snesonline_win PRIVATE SDL2-static)
elseif(TARGET SDL2)
    target_link_libraries(snesonline_win PRIVATE SDL2)
else()
    message(FATAL_ERROR "SDL2 not found. Install SDL2 or set SNESONLINE_FETCH_SDL2=ON.")
endif()

target_compile_definitions(snesonline_win PRIVATE
    SNESONLINE_WINDOWS_APP=1
)

# Separate GUI tool to edit config without running the emulator.
add_executable(snesonline_config WIN32
    ConfigDialog.cpp
    config_main.cpp
    resources.rc
)
target_link_libraries(snesonline_config PRIVATE snesonline_core)
target_include_directories(snesonline_config PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
if(WIN32)
    target_link_libraries(snesonline_config PRIVATE user32 gdi32 comdlg32 shell32)
    target_link_libraries(snesonline_config PRIVATE iphlpapi ws2_32 winhttp)
endif()

if(WIN32)
    target_link_libraries(snesonline_win PRIVATE user32 gdi32 comdlg32 shell32)
    target_link_libraries(snesonline_win PRIVATE iphlpapi ws2_32 winhttp)
endif()
