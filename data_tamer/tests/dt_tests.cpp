#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include <gtest/gtest.h>
#include <variant>
#include <string>


using namespace DataTamer;

TEST(DataTamer, SinkAdd)
{
  auto dummy_sink_A = std::make_shared<DummySink>();
  auto dummy_sink_B = std::make_shared<DummySink>();

  ChannelsRegistry registry;
  registry.addDefaultSink(dummy_sink_A);

  auto channel = registry.getChannel("chan");
  channel->addDataSink(dummy_sink_B);

  double var = 3.14;
  int count = 49;
  [[maybe_unused]] auto id1 = channel->registerValue("var", &var);
  [[maybe_unused]] auto id2 = channel->registerValue("count", &count);

  const int shapshot_count = 10;
  for(int i=0; i<shapshot_count; i++)
  {
    channel->takeSnapshot();
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(1));

  const auto hash = channel->getSchema().hash;
  
  ASSERT_EQ(dummy_sink_A->schemas.size(), 1);
  ASSERT_EQ(dummy_sink_A->schemas.begin()->first, hash);
  ASSERT_EQ(dummy_sink_A->snapshots_count[hash], shapshot_count);
  
  ASSERT_EQ(dummy_sink_B->schemas.size(), 1);
  ASSERT_EQ(dummy_sink_B->schemas.begin()->first, hash);
  ASSERT_EQ(dummy_sink_B->snapshots_count[hash], shapshot_count);
}

TEST(DataTamer, SerializeVariant)
{
  [[maybe_unused]] auto to_str = [](auto&& arg) -> void
  {
    std::cout << std::to_string(arg) << std::endl;
  };


  auto serializeAndBack = [&](auto const& value)
  {
    uint8_t buffer[8];
    ValuePtr ptr(&value);
    ptr.serialize(buffer);
    auto var = DeserializeAsVarType(ptr.type(), buffer);;
    std::visit( to_str, var);
    return var;
  };

  double v1 = 69.0;
  auto n1 = serializeAndBack(v1);
  ASSERT_EQ(v1, std::get<double>(n1));

  int32_t v2 = 42;
  auto n2 = serializeAndBack(v2);
  ASSERT_EQ(v2, std::get<int32_t>(n2));

  uint8_t v3 = 200;
  auto n3 = serializeAndBack(v3);
  ASSERT_EQ(v3, std::get<uint8_t>(n3));
}

TEST(DataTamer, TestRegistration)
{
  ChannelsRegistry registry;
  auto channel = registry.getChannel("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);

  double v1 = 69.0;
  double v2 = 77.0;
  double v2_bis = 42.0;

  int32_t i1 = 55;
  int32_t i2 = 44;
  int32_t i2_bis = 33;
  int32_t i3 = 11;

  auto id_v1 = channel->registerValue("v1", &v1);
  channel->registerValue("v2", &v2);
  auto id_i1 = channel->registerValue("i1", &i1);
  channel->registerValue("i2", &i2);

  // changing the pointer to another one is valid
  channel->registerValue("v2", &v2_bis);

  // changing type is not valid
  ASSERT_ANY_THROW( channel->registerValue("v2", &i1); );

  channel->takeSnapshot();
  // give time to the sink thread to do its work
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // chaning the pointer after takeSnapshot is valid
  channel->registerValue("i2", &i2_bis);

  // adding a new value after takeSnapshot is not valid (would change the schema)
  ASSERT_ANY_THROW( channel->registerValue("i3", &i3); );
  ASSERT_EQ(sink->latest_snapshot.active_mask.size(), 1);

  // payload should contain v1, v2, i1 and i2
  auto expected_size = sizeof(double) * 2 + sizeof(int32_t)*2;
  ASSERT_EQ(sink->latest_snapshot.payload.size(), expected_size);

  //-----------------------------------------------------------------
  // Unregister or disable some values. This should reduce the size of the snapshot
  channel->unregister(id_v1);
  channel->setEnabled(id_i1, false);

  channel->takeSnapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // payload should contain v2, and i2
  auto reduced_size = sizeof(double) + sizeof(int32_t);
  ASSERT_EQ(sink->latest_snapshot.payload.size(), reduced_size);

  //-----------------------------------------------------------------
  // Register and enable again
  channel->registerValue("v1", &v1);
  channel->setEnabled(id_i1, true);

  channel->takeSnapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  ASSERT_EQ(sink->latest_snapshot.payload.size(), expected_size);
}
