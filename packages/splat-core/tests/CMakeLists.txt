add_executable(splat_core_tests
  formats/SplatDecoderTest.cpp
  formats/SpzDecoderTest.cpp
  formats/GlbDecoderTest.cpp
  formats/GlbEncoderTest.cpp
  navigation/ColliderTest.cpp
  navigation/ColliderBuilderTest.cpp
  math/Mat4Test.cpp
  math/HalfTest.cpp
  math/SymmetricEigenTest.cpp
  math/CovarianceTest.cpp
  sorting/SpatialOrderTest.cpp
  sorting/FrustumSortTest.cpp
  lod/LodTreeTest.cpp
  lod/LodFileTest.cpp
  io/MappedFileTest.cpp
  loading/SplatWorldLoaderTest.cpp
  sorting/VisibilityPlannerTest.cpp
  diagnostics/TimingSummaryTest.cpp
  tiles/TilesetTest.cpp
  tiles/TileBuilderTest.cpp
  tiles/SlabAllocatorTest.cpp
  tiles/TileSchedulerTest.cpp
  tiles/TileStreamerTest.cpp
  math/FrustumTest.cpp
  tools/CloudEditTest.cpp
  loading/LoadCostTest.cpp
)
# spz already reaches the link line through the static splat_core; only its headers are added
# here, so Apple's linker does not warn about the duplicate archive.
target_link_libraries(splat_core_tests PRIVATE splat_core ZLIB::ZLIB GTest::gtest_main)
target_include_directories(splat_core_tests PRIVATE
  $<TARGET_PROPERTY:spz,INTERFACE_INCLUDE_DIRECTORIES>)
target_include_directories(splat_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../tools)
target_compile_definitions(splat_core_tests PRIVATE
  SPLAT_FIXTURES_DIR="${SPLAT_FIXTURES_DIR}"
)
if(SPLAT_CORE_BUILD_TOOLS)
  target_sources(splat_core_tests PRIVATE tools/SplatConvertTest.cpp)
  target_compile_definitions(splat_core_tests PRIVATE
    SPLAT_CONVERT_PATH="$<TARGET_FILE:splat-convert>"
    PLY2SPZ_PATH="$<TARGET_FILE:ply2spz>"
  )
  add_dependencies(splat_core_tests splat-convert ply2spz)
endif()

# Holds the loader's memory model to what the code allocates. A binary of its own: the
# tracker replaces operator new for everything linked with it.
add_executable(splat_core_memory_tests
  memory/LoadPeakTest.cpp
  support/AllocationTracker.cpp
)
target_link_libraries(splat_core_memory_tests PRIVATE splat_core GTest::gtest_main)
target_include_directories(splat_core_memory_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  $<TARGET_PROPERTY:spz,INTERFACE_INCLUDE_DIRECTORIES>)

include(GoogleTest)
gtest_discover_tests(splat_core_tests)
gtest_discover_tests(splat_core_memory_tests)
