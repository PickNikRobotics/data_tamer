#include "data_tamer/data_tamer.hpp"
#include <iostream>

int main()
{
  using namespace DataTamer;

  auto channel = std::make_shared<LogChannel>("chan");

  double var = 3.14;
  int count = 42;

  auto id1 = channel->registerValue("var", &var);
  auto id2 = channel->registerValue("count", &count);

  auto logged_real = channel->createLoggedValue<float>("real");
  std::cout << "-------\n" << channel->getDictionary() << std::endl;

}
