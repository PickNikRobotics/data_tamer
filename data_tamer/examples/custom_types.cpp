#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include <iostream>


struct Point3D
{
  double x;
  double y;
  double z;
};

struct Quaternion
{
  double w;
  double x;
  double y;
  double z;
};

struct Pose
{
  Point3D pos;
  Quaternion rot;
};

// You need to add a template specialization of RegisterVariable<> for the new types
namespace DataTamer
{
template <> RegistrationID
RegisterVariable<Point3D>(LogChannel& channel, const std::string& prefix, Point3D* v)
{
  auto id = channel.registerValue(prefix + "/x", &v->x);
  id += channel.registerValue(prefix + "/y", &v->y);
  id += channel.registerValue(prefix + "/z", &v->z);
  return id;
}

template <> RegistrationID
RegisterVariable<Quaternion>(LogChannel& channel, const std::string& prefix, Quaternion* v)
{
  auto id = channel.registerValue(prefix + "/x", &v->x);
  id += channel.registerValue(prefix + "/y", &v->y);
  id += channel.registerValue(prefix + "/z", &v->z);
  id += channel.registerValue(prefix + "/w", &v->w);
  return id;
}

template <> RegistrationID
RegisterVariable<Pose>(LogChannel& channel, const std::string& prefix, Pose* v)
{
  auto id = channel.registerValue(prefix + "/position", &v->pos);
  id += channel.registerValue(prefix + "/rotation", &v->rot);
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
  channel->registerValue<Point3D>("pointB", &pointB);

  // Non-trivial types like Pose are also supported
  Pose pose;
  channel->registerValue("my_pose", &pose);

  // std::vectors can be registered as long as their size will NEVER change
  // It is your responsibility to guarantee that!
  std::vector<Point3D> points_vect(5);
  channel->registerValue("points", &points_vect);

  // std::array is also supported (safer than std::vector due to constant size)
  std::array<int, 3> value_array;
  channel->registerValue("value_array", &value_array);

  // Print the schema to understand how they are serialized
  std::cout << channel->getSchema() << std::endl;
}
