#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include <iostream>


struct Point3D
{
  double x;
  double y;
  double z;
};

namespace DataTamer
{
template <> RegistrationID
RegisterVariable<Point3D>(LogChannel& channel, const std::string& prefix, Point3D* value)
{
  auto id = channel.registerValue(prefix + "/x", &value->x);
  id += channel.registerValue(prefix + "/y", &value->y);
  id += channel.registerValue(prefix + "/z", &value->z);
  return id;
}
} // namespace DataTamer


int main()
{
  using namespace DataTamer;

  auto dummy_sink = std::make_shared<DummySink>();
  ChannelsRegistry::Global().addDefaultSink(dummy_sink);
  auto channel = ChannelsRegistry::Global().getChannel("my_channel");

  // Register as usual
  auto pointA = channel->createLoggedValue<Point3D>("pointA");

  Point3D pointB;
  channel->registerValue("pointB", &pointB);

  channel->takeSnapshot();
}
