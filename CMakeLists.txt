cmake_minimum_required(VERSION 3.10)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
project(dawn-test)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fcolor-diagnostics")
set(CMAKE_LINK_FLAGS "${CMAKE_LINK_FLAGS} -Wall -fcolor-diagnostics")

add_subdirectory("dawn" EXCLUDE_FROM_ALL)

add_executable(server
  "server.cc"
  "protocol.cc"
  "pipe.cc"
  "debug.cc"
)
target_link_libraries(server
  dawn_internal_config
  dawncpp
  dawn_proc
  dawn_common
  dawn_native
  dawn_wire
  dawn_utils
  dawn_glfw
  "ev"
)
add_executable(client
  "client.cc"
  "protocol.cc"
  "pipe.cc"
  "debug.cc"
)
target_link_libraries(client
  dawn_internal_config
  dawncpp
  dawn_proc
  dawn_common
  dawn_wire
  dawn_utils
  "ev"
)

target_link_directories(server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libev/lib )
target_link_directories(client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libev/lib )

target_include_directories(server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libev/include )
target_include_directories(client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libev/include )

target_compile_options(server PRIVATE -fno-rtti)
target_compile_options(client PRIVATE -fno-rtti)

if (${CMAKE_BUILD_TYPE} MATCHES "Debug")
  target_compile_definitions(server PRIVATE DEBUG=1)
  target_compile_definitions(client PRIVATE DEBUG=1)

  target_compile_options(server PRIVATE -g -O0 "-ffile-prefix-map=../../=")
  target_compile_options(client PRIVATE -g -O0 "-ffile-prefix-map=../../=")
endif()

