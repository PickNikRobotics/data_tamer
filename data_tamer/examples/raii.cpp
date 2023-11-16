#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include <cmath>
#include <iostream>

struct Point3D{
  Point3D(double xi, double yi, double zi) : x(xi), y(yi), z(zi) {}
  double x;
  double y;
  double z;
};

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
}

template<typename T, class... Args>
[[nodiscard]] std::pair<DataTamer::RegistrationID, std::shared_ptr<T>> make_shared_and_log(std::shared_ptr<DataTamer::LogChannel> channel, std::string name, Args&&... args){
  auto pointer = new T(std::forward<Args>(args)...);
  auto const id = channel->registerValue(name, pointer);
  auto destructor = [=](T* p){
    channel->setEnabled(id, false);
    std::default_delete<T> deleter;
    deleter(p);
  };
  std::shared_ptr<T> ptr(pointer, destructor);
  return {id, ptr};
}

int main()
{
  using namespace DataTamer;

  // start defining one or more Sinks that must be added by default.
  // Do addDefaultSink BEFORE creating a channel.
  auto mcap_sink = std::make_shared<MCAPSink>("lorenz_attractor.mcap");
  ChannelsRegistry::Global().addDefaultSink(mcap_sink);

  // Create (or get) a channel using the global registry (singleton)
  auto channel = ChannelsRegistry::Global().getChannel("chan");
  int timestep = 0;
  channel->registerValue("timestep", &timestep);

  auto [id, p_t] = make_shared_and_log<Point3D>(channel, "point", 1, 2, 3);
  
  double const t_max = 10;
  double const dt = 0.01;
  double t_cur = 0;

  // simulate a lorenz attractor https://en.wikipedia.org/wiki/Lorenz_system
  double const sigma = 10.;
  double const rho = 28.;
  double const beta = 2.66;
  while (t_cur < t_max) {
    channel->takeSnapshot(std::chrono::nanoseconds(static_cast<int>(std::round(t_cur * 1e9))));
    p_t->x += dt * sigma * (p_t->y - p_t->x);
    p_t->y += dt * (p_t->x * (rho - p_t->z) - p_t->y);
    p_t->z += dt * (p_t->x * p_t->y - beta * p_t->z);
    ++timestep;
    t_cur += dt;
  }

  // now, get rid of our reference to the object
  p_t.reset();
  ++timestep;

  // won't fail because of make_shared_and_log automatically disabling the channel
  // thus we will have one point where timestep is alone at the end
  channel->takeSnapshot();
}
