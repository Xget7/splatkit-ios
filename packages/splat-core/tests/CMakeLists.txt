add_executable(splat_core_tests
  formats/SplatDecoderTest.cpp
  formats/SpzDecoderTest.cpp
  formats/GlbDecoderTest.cpp
  navigation/ColliderTest.cpp
  math/Mat4Test.cpp
  math/HalfTest.cpp
  math/SymmetricEigenTest.cpp
  sorting/DistanceSorterTest.cpp
  sorting/SpatialOrderTest.cpp
  sorting/FrustumSortTest.cpp
  lod/LodTreeTest.cpp
  lod/LodFileTest.cpp
  lod/LodSorterTest.cpp
  io/MappedFileTest.cpp
  loading/SplatWorldLoaderTest.cpp
  sorting/VisibilityPlannerTest.cpp
  diagnostics/TimingSummaryTest.cpp
  tiles/TilesetTest.cpp
  tiles/TileBuilderTest.cpp
  tiles/SlabAllocatorTest.cpp
  tiles/TileSchedulerTest.cpp
  tiles/TileStreamerTest.cpp
  sorting/SlabSorterTest.cpp
  math/FrustumTest.cpp
  tools/CloudEditTest.cpp
)
target_link_libraries(splat_core_tests PRIVATE splat_core spz ZLIB::ZLIB GTest::gtest_main)
target_include_directories(splat_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tools)
target_compile_definitions(splat_core_tests PRIVATE
  SPLAT_FIXTURES_DIR="${SPLAT_FIXTURES_DIR}"
)

include(GoogleTest)
gtest_discover_tests(splat_core_tests)
