// Compiled as strict C++17 (see tests/CMakeLists.txt): every core public header
// must remain usable by C++17 consumers even though the library is built as C++20.
#if __cplusplus > 201703L
#error "this translation unit must be compiled as C++17"
#endif
#include "data_tamer_v2/data_tamer.hpp"
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/custom_types.hpp"
#include "data_tamer_v2/data_sink.hpp"
#include "data_tamer_v2/decode.hpp"
#include "data_tamer_v2/logged_value.hpp"
#include "data_tamer_v2/names.hpp"
#include "data_tamer_v2/types.hpp"
#include "data_tamer_v2/mcap_reader.hpp"
#include "data_tamer_v2/values.hpp"
#include "data_tamer_v2/contrib/SerializeMe.hpp"
#include "data_tamer_v2/details/abi.hpp"
#include "data_tamer_v2/details/locked_reference.hpp"
#include "data_tamer_v2/details/snapshot_pool.hpp"
#include "data_tamer_v2/sinks/dummy_sink.hpp"
#include "data_tamer_v2/sinks/mcap_sink.hpp"
#include "data_tamer_v2/sinks/mcap_encoding.hpp"
#include "data_tamer_v2/sinks/mcap_ring_sink.hpp"
// sinks/ros2_publisher_sink.hpp is not checked: rclcpp itself requires C++20 on
// recent ROS 2 distributions, so that header follows rclcpp's standard.
#include "data_tamer_v2/data_tamer_parser.hpp"

namespace probe_lib
{
template <int N>
struct ProbeVec
{
  double v[N] = {};
};
}  // namespace probe_lib

template <int N>
struct DataTamer2::TypeDefinitionTrait<probe_lib::ProbeVec<N>>
{
  static std::string name() { return "ProbeVec" + std::to_string(N); }
  template <class AddField>
  static void define(probe_lib::ProbeVec<N>& p, AddField& add)
  {
    add("x", &p.v[0]);
  }
};

// Instantiate the templates a consumer would.
namespace
{
struct Probe
{
  double x = 0;
};
template <class AddField>
std::string_view TypeDefinition(Probe& p, AddField& add)
{
  add("x", &p.x);
  return "Probe";
}
[[maybe_unused]] void useEverything()
{
  auto channel = DataTamer2::LogChannel::create("probe");
  double d = 0;
  Probe probe;
  probe_lib::ProbeVec<2> probe_vec;
  channel->registerValue("probe_vec", &probe_vec);
  std::vector<double> v;
  channel->registerValue("d", &d);
  channel->registerValue("probe", &probe);
  channel->registerValue("v", &v);
  double joined = 0;
  const std::string joined_name = DataTamer2::JoinNames("ns/", std::string("joined"));
  if(DataTamer2::IsCanonicalName(joined_name))
  {
    channel->registerValue(joined_name, &joined);
  }
  auto logged = channel->createLoggedValue<double>("logged");
  logged->set(1.0);
  auto tx = channel->scopedWrite();
  channel->addDataSink(DataTamer2::DummySink::create());
  const DataTamer2::Snapshot snapshot{};
  [[maybe_unused]] const auto view = ToSnapshotView(snapshot);
  DataTamer2::ForEachValue("probe.mcap", [](const auto&...) {});
}
}  // namespace
