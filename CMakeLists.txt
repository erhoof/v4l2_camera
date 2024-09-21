cmake_minimum_required(VERSION 3.16.0)
project(v4l2_sdl2 VERSION 1.0.0 LANGUAGES CXX)

set(SOURCES
    src/main.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC
    inc
)

find_package(SDL2 REQUIRED)

target_link_libraries(${PROJECT_NAME} PRIVATE
    SDL2::SDL2
)
