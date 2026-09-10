#include "data_tamer/data_tamer.hpp"

#include <gtest/gtest.h>

#include <type_traits>

using namespace DataTamer;

TEST(LoggedValue, IsNotMovableOrCopyable)
{
  static_assert(!std::is_copy_constructible_v<LoggedValue<double>>);
  static_assert(!std::is_copy_assignable_v<LoggedValue<double>>);
  static_assert(!std::is_move_constructible_v<LoggedValue<double>>,
                "moving a LoggedValue would leave the channel with a dangling pointer");
  static_assert(!std::is_move_assignable_v<LoggedValue<double>>);
}

TEST(LoggedValue, SharedPtrHandleStillWorks)
{
  auto channel = LogChannel::create("chan");
  auto v = channel->createLoggedValue<float>("f", 1.0f);
  std::shared_ptr<LoggedValue<float>> moved = std::move(v);  // moving the handle is fine
  ASSERT_FALSE(v);
  ASSERT_EQ(moved->get(), 1.0f);
}
