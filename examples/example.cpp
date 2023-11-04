#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include <iostream>

int main()
{
  using namespace DataTamer;

  auto dummy_sink = std::make_shared<DummySync>();
  auto channel = ChannelsRegistry::Global().getChannel("chan");

  double var = 3.14;
  int count = 42;

  auto id1 = channel->registerValue("var", &var);
  auto id2 = channel->registerValue("count", &count);

  auto logged_real = channel->createLoggedValue<float>("real");
  std::cout << "-------\n" << channel->getDictionary() << std::endl;

}
