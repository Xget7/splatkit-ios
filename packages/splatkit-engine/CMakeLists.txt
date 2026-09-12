cmake_minimum_required(VERSION 3.22)
project(splatkit_engine LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(SPLATKIT_ENGINE_BUILD_TESTS "Build splatkit_engine unit tests" ${PROJECT_IS_TOP_LEVEL})

# The engine sits on splat-core; a platform package that already added it skips this.
if(NOT TARGET splat_core)
  set(SPLAT_CORE_BUILD_TESTS ${SPLATKIT_ENGINE_BUILD_TESTS} CACHE BOOL "" FORCE)
  set(SPLAT_CORE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../splat-core splat-core)
endif()

# Everything of the engine that has no graphics API in it: the frame, the camera, the
# diagnostics and the GPU record layout. A platform supplies a SplatRenderer and a log
# sink and gets the whole engine.
add_library(splatkit_engine STATIC
  src/Log.cpp
  src/rendering/GpuLayout.cpp
  src/camera/WalkCamera.cpp
  src/diagnostics/Benchmark.cpp
  src/diagnostics/StatsPublisher.cpp
  src/engine/SplatEngine.cpp
)
target_include_directories(splatkit_engine PUBLIC include)
target_link_libraries(splatkit_engine PUBLIC splat_core)
target_compile_options(splatkit_engine PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
)

if(SPLATKIT_ENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
