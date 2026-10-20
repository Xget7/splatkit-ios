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

}  // namespace
}  // namespace splatkit
