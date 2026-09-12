add_executable(splatkit_ios_tests
  MetalVisibilityTest.mm
  MetalLODTest.mm
  MetalRadixSortTest.mm
  MetalWorldTest.mm
  MetalRasterTest.mm
  MetalTileRasterTest.mm
)
target_include_directories(splatkit_ios_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../Sources/SplatKitCore ${CMAKE_CURRENT_BINARY_DIR}/../embedded
)
target_link_libraries(splatkit_ios_tests PRIVATE splatkit_ios GTest::gtest_main)
target_compile_options(splatkit_ios_tests PRIVATE -Wall -Wextra -Werror -fobjc-arc)
add_dependencies(splatkit_ios_tests splatkit_ios_shaders)
include(GoogleTest)
gtest_discover_tests(splatkit_ios_tests)
