cmake_minimum_required(VERSION 3.10)
project(lvgl)
set(CMAKE_C_STANDARD 11)#C11
set(CMAKE_CXX_STANDARD 17)#C17
set(CMAKE_CXX_STANDARD_REQUIRED ON)

INCLUDE_DIRECTORIES(${PROJECT_SOURCE_DIR})


# set ffmpeg develop environment
set(FFMPEG_DEV_ROOT /opt/ffmpeg)
include_directories(${FFMPEG_DEV_ROOT}/include)
link_directories(${FFMPEG_DEV_ROOT}/lib)
link_directories(${FFMPEG_DEV_ROOT}/lib64)


link_libraries(
    avcodec
    avformat
    avfilter
    avdevice
    swresample
    swscale
    avutil
)

file(GLOB_RECURSE INCLUDES "lv_drivers/*.h" "lv_demos/*.h"  "lvgl/*.h"  "./*.h" )
file(GLOB_RECURSE SOURCES  "lv_drivers/*.c" "lv_demos/*.c"  "lvgl/*.c" )

SET(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin) 

find_package(SDL2 REQUIRED SDL2)

# Add log messages
message ([STATUS] "SDL2_INCLUDE_DIR" ${SDL2_INCLUDE_DIR})
message ([STATUS] "SDL2_LIBRARY" ${SDL2_LIBRARY})

include_directories(main ${SDL2_INCLUDE_DIRS})
add_executable(main main.c mouse_cursor_icon.c ${SOURCES} ${INCLUDES})
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
target_link_libraries(main PRIVATE ${SDL2_LIBRARIES} )
add_custom_target (run COMMAND ${EXECUTABLE_OUTPUT_PATH}/main)

