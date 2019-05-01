cmake_minimum_required(VERSION 3.0.2)

project(csid)

find_package(SDL REQUIRED)
include_directories(${SDL_INCLUDE_DIR})

add_executable(csidl csid-light.c)
target_link_libraries(csidl ${SDL_LIBRARY})

