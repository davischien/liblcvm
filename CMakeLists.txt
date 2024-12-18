cmake_minimum_required(VERSION 3.12)

project(lcvm VERSION 0.7)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE INTERNAL "" FORCE)

# Set compiler flags
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra -Wno-unused-parameter -Wshadow -Werror")

message(STATUS "CMAKE_CURRENT_SOURCE_DIR: ${CMAKE_CURRENT_SOURCE_DIR} ")

# add include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/lib/isobmff/ISOBMFF/include)


# 1. build external libraries

# 1.1. build ISOBMFF library
# apply isobmff fix patch
execute_process(
  COMMAND git apply ${CMAKE_CURRENT_SOURCE_DIR}/lib/isobmff.patch
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib/isobmff
)

# add the ISOBMFF submodule
include(ExternalProject)
ExternalProject_Add(
  ISOBMFF
  SOURCE_DIR ${CMAKE_SOURCE_DIR}/lib/isobmff
  BUILD_IN_SOURCE 1
  CONFIGURE_COMMAND ""
  BUILD_COMMAND make
  INSTALL_COMMAND ""
)

# build isobmff library
execute_process(
  COMMAND make
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib/isobmff
)


# we do not care about fuzzing either h264nal nor h265nal
set(BUILD_CLANG_FUZZER OFF)  # Set the variable here

# 1.2. add h265nal submodule
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/h265nal)

# 1.3. add h264nal submodule
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/h264nal)


# 2. build liblcvm library
add_library(liblcvm src/liblcvm.cc)

target_link_libraries(liblcvm
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/isobmff/Build/Debug/Products/x86_64/libISOBMFF.a
  h265nal
  h264nal
)
target_include_directories(liblcvm
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib/isobmff/ISOBMFF/include
)
target_include_directories(liblcvm
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib/h265nal/include
)
target_include_directories(liblcvm
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib/h264nal/include
)

# set the output name to ensure the library is named liblcvm.a
set_target_properties(liblcvm PROPERTIES OUTPUT_NAME "lcvm")


# 3. build lcvm executable
add_executable(lcvm tools/lcvm.cc)
target_link_libraries(lcvm liblcvm)

# configure a header file to pass some of the CMake settings
# to the source code
configure_file(
  ${CMAKE_SOURCE_DIR}/include/config.h.in
  ${CMAKE_BINARY_DIR}/config.h
)

# Include the directory where config.h is generated
target_include_directories(liblcvm PRIVATE ${CMAKE_BINARY_DIR})


# 4. others

# 4.1. add test target
add_custom_target(test
  COMMAND ${CMAKE_BINARY_DIR}/lcvm ${CMAKE_BINARY_DIR}/*MOV -o /tmp/full.csv
)

# 4.2. add lint target
add_custom_target(lint
  COMMAND clang-format -i -style=google ${CMAKE_CURRENT_SOURCE_DIR}/include/*h
  COMMAND clang-format -i -style=google ${CMAKE_CURRENT_SOURCE_DIR}/src/*cc
  COMMAND clang-format -i -style=google ${CMAKE_CURRENT_SOURCE_DIR}/tools/*cc
)
