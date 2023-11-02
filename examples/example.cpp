#include "data_tamer/data_tamer.hpp"
#include <iostream>

int main()
{
  using namespace DataTamer;

  auto channel = std::make_shared<LogChannel>("chan");

  double var = 3.14;
  int count = 42;

  channel->registerValue("var", &var);
  channel->registerValue("count", &count);

  auto logged_real = channel->createLoggedValue<float>("real");

  const auto dict = channel->getDictionary();
  std::cout << dict << std::endl;
}
