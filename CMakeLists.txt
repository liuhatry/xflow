cmake_minimum_required (VERSION 2.8)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -pthread")

# projectname is the same as the main-executable
project(xflow)

add_definitions('-g')
add_definitions('-Wall')
add_definitions('-std=c++11')
add_definitions('-pthread')

include_directories(
  "${PROJECT_SOURCE_DIR}"
  "${PROJECT_SOURCE_DIR}/ps-lite/include"
  "${PROJECT_BINARY_DIR}"
)
message(${PROJECT_SOURCE_DIR})

link_directories(
  "${PROJECT_BINARY_DIR}"
  "${PROJECT_BINARY_DIR}/ps-lite"
)

add_subdirectory(ps-lite)
add_subdirectory(src/io)
add_subdirectory(src/model)
#add_subdirectory(src/c_api)
