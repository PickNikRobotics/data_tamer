#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include <iostream>
#include <thread>


// check the custom type in the following file,  that defines:
//
// - Point3D
// - Quaternion
// - Pose

#include "geometry_types.hpp"


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
  std::array<int32_t, 3> value_array;
  channel->registerValue("value_array", &value_array);

  // Print the schema to understand how they are serialized
  std::cout << channel->getSchema() << std::endl;

  size_t expected_size = sizeof(double) * 3 + // pointA
                         sizeof(double) * 3 + // pointB
                         sizeof(double) * 7 + // my_pose
                         sizeof(uint32_t) + 5 * (sizeof(double) * 3) + //points_vect
                         sizeof(int32_t) * 3;

  channel->takeSnapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  std::cout << "\nMessage size: " << dummy_sink->latest_snapshot.payload.size()
            << " exepcted: " << expected_size << std::endl;

}
