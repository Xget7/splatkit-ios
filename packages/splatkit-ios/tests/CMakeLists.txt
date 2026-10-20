add_executable(splatkit_ios_tests
  rendering/metal/cull/MetalVisibilityPassTest.mm
  rendering/metal/device/MetalContextTest.mm
  rendering/metal/presentation/MetalFrameLoopTest.mm
  rendering/metal/presentation/MetalRenderTargetTest.mm
  rendering/metal/presentation/MetalSurfaceTest.mm
  rendering/metal/presentation/MetalUpscalerTest.mm
  rendering/metal/selection/MetalLodSelectionTest.mm
  rendering/metal/sort/MetalRadixSortTest.mm
  rendering/metal/world/MetalWorldTest.mm
  rendering/metal/MetalSplatRendererTest.mm
)
target_include_directories(splatkit_ios_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../Sources/SplatKitCore ${CMAKE_CURRENT_BINARY_DIR}/../embedded
)
target_link_libraries(splatkit_ios_tests PRIVATE splatkit_ios GTest::gtest_main)
target_compile_options(splatkit_ios_tests PRIVATE -Wall -Wextra -Werror -fobjc-arc)
add_dependencies(splatkit_ios_tests splatkit_ios_shaders)
include(GoogleTest)
gtest_discover_tests(splatkit_ios_tests)
