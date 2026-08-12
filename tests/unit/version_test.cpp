#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/version.hpp"

#include <gtest/gtest.h>

namespace arena_core {
namespace {

TEST(LanguageContractTest, BuildUsesExactlyCpp17) {
  EXPECT_GE(kCxxLanguageLevel, 201703L);
  EXPECT_LT(kCxxLanguageLevel, 202002L);
  EXPECT_EQ(kProjectVersion, "1.0.0");
  EXPECT_EQ(kProtocolVersion, "1");
}

}  // namespace
}  // namespace arena_core
