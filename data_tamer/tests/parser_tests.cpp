#include "data_tamer_parser/data_tamer_parser.hpp"

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

  auto res = BuilSchemaFromText(text);
  ASSERT_TRUE(res);

  const auto& schema = *res;
  ASSERT_EQ(schema.fields.size(), 7);

  Schema::Field field0 = {"v1", BasicType::INT8, false, 0};
  ASSERT_EQ(schema.fields[0], field0);

  Schema::Field field1 = {"v2", BasicType::FLOAT64, false, 0};
  ASSERT_EQ(schema.fields[1], field1);

  Schema::Field field2 = {"array", BasicType::FLOAT32, true, 5};
  ASSERT_EQ(schema.fields[2], field2);

  Schema::Field field3 = {"vect", BasicType::INT32, true, 0};
  ASSERT_EQ(schema.fields[3], field3);

  Schema::Field field4 = {"is_true", BasicType::BOOL, false, 0};
  ASSERT_EQ(schema.fields[4], field4);

  Schema::Field field5 = {"blob", BasicType::CHAR, true, 256};
  ASSERT_EQ(schema.fields[5], field5);

  Schema::Field field6 = {"my/short", BasicType::UINT16, false, 0};
  ASSERT_EQ(schema.fields[6], field6);
}
