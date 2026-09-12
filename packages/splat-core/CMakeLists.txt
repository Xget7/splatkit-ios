cmake_minimum_required(VERSION 3.22)
project(splat_core LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(SPLAT_CORE_BUILD_TESTS "Build splat_core unit tests" ${PROJECT_IS_TOP_LEVEL})
option(SPLAT_CORE_BUILD_TOOLS "Build ply2spz, splat-tile and splat_lod_build" ${PROJECT_IS_TOP_LEVEL})
# "thread" or "address" (address implies undefined). Off by default; CI runs both.
set(SPLAT_CORE_SANITIZE "" CACHE STRING "Sanitizer for splat_core and its tests")

if(SPLAT_CORE_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -g -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(SPLAT_CORE_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -g -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(cmake/dependencies.cmake)

find_package(Threads REQUIRED)

add_library(splat_core STATIC
  src/formats/SplatDecoder.cpp
  src/formats/SpzDecoder.cpp
  src/formats/GlbDecoder.cpp
  src/navigation/Collider.cpp
  src/navigation/CharacterController.cpp
  src/sorting/DistanceSorter.cpp
  src/sorting/AsyncSorter.cpp
  src/sorting/SpatialOrder.cpp
  src/sorting/WorkerPool.cpp
  src/math/SymmetricEigen.cpp
  src/lod/LodTree.cpp
  src/lod/LodSelection.cpp
  src/lod/LodFile.cpp
  src/io/MappedFile.cpp
  src/loading/SplatWorldLoader.cpp
  src/sorting/VisibilityPlanner.cpp
  src/diagnostics/TimingSummary.cpp
  src/tiles/Tileset.cpp
  src/tiles/TileBuilder.cpp
  src/tiles/SlabAllocator.cpp
  src/tiles/TileScheduler.cpp
  src/tiles/TileLoader.cpp
  src/tiles/TiledWorld.cpp
  src/tiles/TileStreamer.cpp
  src/sorting/SlabSorter.cpp
)
target_include_directories(splat_core PUBLIC include)
find_package(ZLIB REQUIRED)  # the SPZ gzip container is inflated here, with a ceiling
target_link_libraries(splat_core PRIVATE spz nlohmann_json::nlohmann_json ZLIB::ZLIB PUBLIC Threads::Threads)
target_compile_options(splat_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
)

if(SPLAT_CORE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SPLAT_CORE_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
