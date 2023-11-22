#include "data_tamer_parser/data_tamer_parser.hpp"
#include "data_tamer/data_tamer.hpp"

#include <gtest/gtest.h>
#include <variant>
#include <string>


using namespace DataTamerParser;

TEST(DataTamerParser, ReadSchema)
{
  const char* text = R"(
int8 v1
float64 v2
float32[5] array
int32[] vect
bool is_true
char[256] blob
uint16  my/short
  )";

  const auto schema = BuilSchemaFromText(text);

  ASSERT_EQ(schema.fields.size(), 7);

  Schema::Field field0 = {"v1", BasicType::INT8, false, 0, {}};
  ASSERT_EQ(schema.fields[0], field0);

  Schema::Field field1 = {"v2", BasicType::FLOAT64, false, 0, {}};
  ASSERT_EQ(schema.fields[1], field1);

  Schema::Field field2 = {"array", BasicType::FLOAT32, true, 5, {}};
  ASSERT_EQ(schema.fields[2], field2);

  Schema::Field field3 = {"vect", BasicType::INT32, true, 0, {}};
  ASSERT_EQ(schema.fields[3], field3);

  Schema::Field field4 = {"is_true", BasicType::BOOL, false, 0, {}};
  ASSERT_EQ(schema.fields[4], field4);

  Schema::Field field5 = {"blob", BasicType::CHAR, true, 256, {}};
  ASSERT_EQ(schema.fields[5], field5);

  Schema::Field field6 = {"my/short", BasicType::UINT16, false, 0, {}};
  ASSERT_EQ(schema.fields[6], field6);
}

TEST(DataTamerParser, SchemaHash)
{
  // Create (or get) a channel using the global registry (singleton)
  auto channel = DataTamer::ChannelsRegistry::Global().getChannel("channel");

  // logs in channelA
  std::vector<double> v1(10, 0);
  std::array<float, 4> v2 = {1, 2, 3, 4};
  int32_t v3 = 5;
  uint16_t v4 = 6;
  double v5 = 10;
  uint16_t v6 = 11;
  std::vector<uint8_t> v7(4, 12);
  std::array<uint32_t, 3> v8 = {13, 14, 15};

  channel->registerValue("vector_10", &v1);
  channel->registerValue("array_4", &v2);
  channel->registerValue("val_int32", &v3);
  channel->registerValue("val_int16", &v4);
  channel->registerValue("real_value", &v5);
  channel->registerValue("short_int", &v6);
  channel->registerValue("vector_4", &v7);
  channel->registerValue("array_3", &v8);

  const auto schema_in = channel->getSchema();
  std::ostringstream ss;
  ss << schema_in;

  const auto& schema_out = BuilSchemaFromText(ss.str());

  ASSERT_EQ(schema_out.fields[0].name, "vector_10");
  ASSERT_EQ(schema_out.fields[1].name, "array_4");
  ASSERT_EQ(schema_out.fields[2].name, "val_int32");
  ASSERT_EQ(schema_out.fields[3].name, "val_int16");
  ASSERT_EQ(schema_out.fields[4].name, "real_value");
  ASSERT_EQ(schema_out.fields[5].name, "short_int");
  ASSERT_EQ(schema_out.fields[6].name, "vector_4");
  ASSERT_EQ(schema_out.fields[7].name, "array_3");
}
