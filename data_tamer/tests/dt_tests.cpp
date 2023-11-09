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

  ChannelsRegistry::Global().addDefaultSink(dummy_sink_A);

  auto channel = ChannelsRegistry::Global().getChannel("chan");
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
