#include "data_tamer_parser/data_tamer_parser.hpp"
#include "data_tamer/data_tamer.hpp"
#include "../examples/geometry_types.hpp"

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

  TypeField field0 = {"v1", BasicType::INT8, {}, false, 0};
  ASSERT_EQ(schema.fields[0], field0);

  TypeField field1 = {"v2", BasicType::FLOAT64, {}, false, 0};
  ASSERT_EQ(schema.fields[1], field1);

  TypeField field2 = {"array", BasicType::FLOAT32, {}, true, 5};
  ASSERT_EQ(schema.fields[2], field2);

  TypeField field3 = {"vect", BasicType::INT32, {}, true, 0};
  ASSERT_EQ(schema.fields[3], field3);

  TypeField field4 = {"is_true", BasicType::BOOL, {}, false, 0};
  ASSERT_EQ(schema.fields[4], field4);

  TypeField field5 = {"blob", BasicType::CHAR, {}, true, 256};
  ASSERT_EQ(schema.fields[5], field5);

  TypeField field6 = {"my/short", BasicType::UINT16, {}, false, 0};
  ASSERT_EQ(schema.fields[6], field6);
}

TEST(DataTamerParser, SchemaHash)
{
  // Create (or get) a channel using the global registry (singleton)
  DataTamer::ChannelsRegistry registry;
  auto channel = registry.getChannel("channel");

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

  ASSERT_EQ(schema_out.fields[0].field_name, "vector_10");
  ASSERT_EQ(schema_out.fields[1].field_name, "array_4");
  ASSERT_EQ(schema_out.fields[2].field_name, "val_int32");
  ASSERT_EQ(schema_out.fields[3].field_name, "val_int16");
  ASSERT_EQ(schema_out.fields[4].field_name, "real_value");
  ASSERT_EQ(schema_out.fields[5].field_name, "short_int");
  ASSERT_EQ(schema_out.fields[6].field_name, "vector_4");
  ASSERT_EQ(schema_out.fields[7].field_name, "array_3");
}

TEST(DataTamerParser, CustomTypes)
{
  DataTamer::ChannelsRegistry registry;
  auto channel = registry.getChannel("channel");

  Pose pose;
  channel->registerValue("pose", &pose);

  const auto& schema_in = channel->getSchema();
  const std::string schema_txt = ToStr(schema_in);
  const auto& schema_out = DataTamerParser::BuilSchemaFromText(schema_txt);

  std::cout << schema_txt << std::endl;

  ASSERT_EQ(schema_in.hash, schema_out.hash);
  ASSERT_EQ(schema_in.channel_name, schema_out.channel_name);
  ASSERT_EQ(schema_in.fields.size(), schema_out.fields.size());
  ASSERT_EQ(schema_in.custom_types.size(), schema_out.custom_types.size());

  for(const auto& [type_name, custom_in]: schema_in.custom_types)
  {
    const auto& custom_out = schema_out.custom_types.at(type_name);
    ASSERT_EQ(custom_in.fields.size(), custom_out.fields.size());

    for(size_t i=0; i<custom_in.fields.size(); i++)
    {
      const auto& field_in = custom_in.fields[i];
      const auto& field_out = custom_out.fields[i];

      ASSERT_EQ(field_in.field_name, field_out.field_name);
      ASSERT_EQ(static_cast<int>(field_in.type), static_cast<int>(field_out.type));
      ASSERT_EQ(field_in.is_vector, field_out.is_vector);
      ASSERT_EQ(field_in.array_size, field_out.array_size);
    }
  }
}
