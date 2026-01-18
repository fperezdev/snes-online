cmake_minimum_required(VERSION 3.20)

add_library(snesonline_jni SHARED
    native-lib.cpp
)

target_include_directories(snesonline_jni PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_link_libraries(snesonline_jni PRIVATE snesonline_core)

# Android log if needed by your app.
find_library(log-lib log)
target_link_libraries(snesonline_jni PRIVATE ${log-lib})
