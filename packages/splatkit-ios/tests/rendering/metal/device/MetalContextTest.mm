#include <gtest/gtest.h>

#include "rendering/metal/device/MetalContext.h"

namespace splatkit {
namespace {

TEST(MetalContextTest, CreateBuildsDeviceQueueLibraryAndDescription) {
  id<MTLDevice> host = MTLCreateSystemDefaultDevice();
  if (host == nil || ![host supportsFamily:MTLGPUFamilyApple7])
    GTEST_SKIP() << "Apple GPU family 7 unavailable; the context requires A14/M1+";
  auto context = MetalContext::create();
  ASSERT_NE(context, nullptr);
  ASSERT_NE(context->library(), nil);
  EXPECT_NE([context->library() newFunctionWithName:@"splatFragment"], nil);
  EXPECT_EQ(context->description(), std::string(context->device().name.UTF8String) + ", Metal");
}

TEST(MetalContextTest, CapabilitiesNameTheNewestAppleFamilyAndTheLimits) {
  id<MTLDevice> host = MTLCreateSystemDefaultDevice();
  if (host == nil || ![host supportsFamily:MTLGPUFamilyApple7])
    GTEST_SKIP() << "Apple GPU family 7 unavailable; the context requires A14/M1+";
  auto context = MetalContext::create();
  ASSERT_NE(context, nullptr);
  id<MTLDevice> device = context->device();
  int newest = 7;
  while ([device supportsFamily:static_cast<MTLGPUFamily>(MTLGPUFamilyApple1 + newest)]) ++newest;
  const std::string capabilities = context->capabilities();
  const auto contains = [&](const std::string& part) {
    return capabilities.find(part) != std::string::npos;
  };
  EXPECT_TRUE(contains("family Apple" + std::to_string(newest) + ",")) << capabilities;
  EXPECT_TRUE(
      contains("threadgroup memory " + std::to_string(device.maxThreadgroupMemoryLength) + " B"))
      << capabilities;
  EXPECT_TRUE(
      contains("threads per threadgroup " + std::to_string(device.maxThreadsPerThreadgroup.width)))
      << capabilities;
  EXPECT_TRUE(contains("max buffer " + std::to_string(device.maxBufferLength) + " B"))
      << capabilities;
  EXPECT_TRUE(contains("working set " + std::to_string(device.recommendedMaxWorkingSetSize) + " B"))
      << capabilities;
}

}  // namespace
}  // namespace splatkit
