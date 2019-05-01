cmake_minimum_required(VERSION 3.0.2)

project(csid)

find_package(SDL REQUIRED)
include_directories(${SDL_INCLUDE_DIR})

add_executable(csidl csid-light.c)
target_link_libraries(csidl ${SDL_LIBRARY})

add_executable(csid csid.c)
target_link_libraries(csid ${SDL_LIBRARY})

