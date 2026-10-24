// Golden test for docs/wire_format.md. The fixture files under
// docs/wire_format/vectors/ are the source of truth: a change in the schema text
// or in the snapshot bytes fails here and must be a deliberate format revision.
//
// Regenerate the fixtures with DATA_TAMER_UPDATE_GOLDEN=1, then review the diff
// and update expected.json by hand. vectors/v4/ was recorded by data_tamer 1.0.4 and is
// never regenerated.
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/data_tamer.hpp"
#include "data_tamer_v2/sinks/dummy_sink.hpp"
#include "data_tamer_v2/sinks/mcap_sink.hpp"
#include "data_tamer_v2/data_tamer_parser.hpp"
#include "decode_utils.hpp"
#include "test_sinks.hpp"
#include "geometry_types.hpp"

#include <mcap/reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

using namespace DataTamer2;
using TestTypes::Point3D;

namespace
{
struct StampedPose
{
  Point3D position;
  uint32_t stamp = 0;
};
template <class AddField>
std::string_view TypeDefinition(StampedPose& p, AddField& add)
{
  add("position", &p.position);
  add("stamp", &p.stamp);
  return "Pose";
}

struct Label
{
  uint8_t id = 0;
  std::string text;
};
template <class AddField>
std::string_view TypeDefinition(Label& p, AddField& add)
{
  add("id", &p.id);
  add("text", &p.text);
  return "Label";
}

const std::string kDir = DATA_TAMER_WIRE_FORMAT_DIR;

std::vector<uint8_t> readFile(const std::string& name)
{
  std::ifstream file(kDir + "/vectors/" + name, std::ios::binary);
  return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

// Compare (or, with DATA_TAMER_UPDATE_GOLDEN set, record) `actual` against the fixture.
void checkGolden(const std::string& name, const std::vector<uint8_t>& actual)
{
  if(std::getenv("DATA_TAMER_UPDATE_GOLDEN"))
  {
    std::ofstream file(kDir + "/vectors/" + name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(actual.data()),
               std::streamsize(actual.size()));
    return;
  }
  const auto expected = readFile(name);
  ASSERT_FALSE(expected.empty()) << "missing fixture " << name;
  EXPECT_EQ(actual, expected) << "wire format changed: " << name
                              << " (see docs/wire_format.md)";
}

// Decoded messages of a recording: (topic, log time) -> flattened name -> value text.
using Messages =
    std::map<std::pair<std::string, uint64_t>, std::map<std::string, std::string>>;

// vectors/v4/expected.tsv: "topic \t log time \t name \t value" lines.
Messages readExpected104()
{
  std::ifstream file(kDir + "/vectors/v4/expected.tsv");
  Messages out;
  std::string line;
  while(std::getline(file, line))
  {
    if(line.empty() || line[0] == '#')
    {
      continue;
    }
    std::istringstream fields(line);
    std::string topic, time, name, value;
    std::getline(fields, topic, '\t');
    std::getline(fields, time, '\t');
    std::getline(fields, name, '\t');
    std::getline(fields, value);
    out[{ topic, std::stoull(time) }][name] = value;
  }
  return out;
}

// A decoded value as expected.tsv writes it: floats by their binary64 bits.
std::string asText(const DataTamerParser::VarNumber& number)
{
  return std::visit(
      [](auto value) -> std::string {
        using T = decltype(value);
        if constexpr(std::is_floating_point_v<T>)
        {
          const double wide = value;
          uint64_t bits = 0;
          std::memcpy(&bits, &wide, sizeof(bits));
          char text[24];
          std::snprintf(text, sizeof(text), "f:%016llx",
                        static_cast<unsigned long long>(bits));
          return text;
        }
        else if constexpr(std::is_same_v<T, char>)
        {
          return std::to_string(static_cast<unsigned char>(value));
        }
        else
        {
          return std::to_string(value);
        }
      },
      number);
}

// Decodes vectors/v4/recorded_1.0.4.mcap, each channel with the schema that
// `parse(topic, schema record text)` returns.
template <class Parse>
Messages decodeRecording104(const Parse& parse)
{
  using namespace DataTamerParser;
  mcap::McapReader reader;
  EXPECT_TRUE(reader.open(kDir + "/vectors/v4/recorded_1.0.4.mcap").ok());
  std::map<std::string, DataTamerParser::Schema> schemas;  // by topic
  Messages out;
  for(const auto& view : reader.readMessages())
  {
    const std::string& topic = view.channel->topic;
    if(schemas.count(topic) == 0)
    {
      const auto* text = reinterpret_cast<const char*>(view.schema->data.data());
      schemas[topic] = parse(topic, std::string(text, view.schema->data.size()));
    }
    const auto& schema = schemas[topic];
    auto snapshot = SplitMcapMessage(
        { reinterpret_cast<const uint8_t*>(view.message.data), view.message.dataSize });
    snapshot.schema_hash = schema.hash;
    auto& values = out[{ topic, view.message.logTime }];
    try
    {
      EXPECT_TRUE(ParseSnapshot(
          schema, snapshot, [&values](const std::string& name, const VarNumber& value) {
            values[name] = asText(value);
          }));
    }
    catch(const std::runtime_error& error)
    {
      const std::string what = error.what();
      const bool opaque = what.find("Pos2D has an opaque encoding") != std::string::npos;
      values = { { "!undecodable", opaque ? "Pos2D" : what } };
    }
  }
  reader.close();
  return out;
}

std::string replaceHash(std::string text, uint64_t hash)
{
  const auto begin = text.find("### hash: ") + 10;
  text.replace(begin, text.find('\n', begin) - begin, std::to_string(hash));
  return text;
}

}  // namespace

TEST(WireFormat, SchemaTextAndSnapshotsMatchGoldenVectors)
{
  const auto mcap_path =
      (std::filesystem::temp_directory_path() /
       ("data_tamer_wire_format_" + std::to_string(::getpid()) + ".mcap"))
          .string();
  auto channel = LogChannel::create("wire_test");
  // Manual delivery: snapshots reach the sinks exactly when the test drains them.
  auto sink = DataTamerTest::manual<DummySink>();
  auto mcap = DataTamerTest::manual<MCAPSink>(mcap_path, false);
  channel->addDataSink(sink);
  channel->addDataSink(mcap);

  // One field of every basic type, in the order of the BasicType enum.
  bool flag = true;
  char letter = 'A';
  int8_t i8 = -8;
  uint8_t u8 = 200;
  int16_t i16 = -1600;
  uint16_t u16 = 60000;
  int32_t i32 = -320000;
  uint32_t u32 = 4000000000u;
  int64_t i64 = -6400000000LL;
  uint64_t u64 = 0x0102030405060708ULL;
  float f32 = 1.5f;
  double f64 = -2.25;
  // Containers and custom types.
  std::vector<double> vec = { 10.0, 20.0, 30.0 };
  std::array<int32_t, 4> arr = { 1, -2, 3, -4 };
  StampedPose pose;
  pose.position = { 1.0, 2.0, 3.0 };
  pose.stamp = 42;
  std::vector<Point3D> points = { { 4.0, 5.0, 6.0 }, { 7.0, 8.0, 9.0 } };

  channel->registerValue("flag", &flag);
  channel->registerValue("letter", &letter);
  channel->registerValue("i8", &i8);
  channel->registerValue("u8", &u8);
  const auto id_i16 = channel->registerValue("i16", &i16);
  channel->registerValue("u16", &u16);
  channel->registerValue("i32", &i32);
  channel->registerValue("u32", &u32);
  channel->registerValue("i64", &i64);
  channel->registerValue("u64", &u64);
  channel->registerValue("f32", &f32);
  channel->registerValue("f64", &f64);
  channel->registerValue("vec", &vec);
  channel->registerValue("arr", &arr);
  const auto id_pose = channel->registerValue("pose", &pose);
  channel->registerValue("points", &points);

  const std::chrono::nanoseconds stamp(1234567890);
  ASSERT_EQ(channel->takeSnapshot(stamp), SnapshotResult::ok);
  sink.drain();
  const Snapshot full = sink->latestSnapshot();

  const std::string schema_text = ToStr(channel->getSchema());
  checkGolden("schema.txt", { schema_text.begin(), schema_text.end() });
  EXPECT_EQ(channel->getSchema().hash, SchemaTextHash(schema_text));
  const std::string schema_yaml = ToYaml(channel->getSchema());
  checkGolden("schema.yaml", { schema_yaml.begin(), schema_yaml.end() });
  checkGolden("snapshot_full.mask", full.active_mask);
  checkGolden("snapshot_full.payload", full.payload);

  // Disable one scalar and one custom type: their bits clear and their bytes
  // disappear from the payload; everything else keeps its relative order.
  channel->setEnabled(id_i16, false);
  channel->setEnabled(id_pose, false);
  ASSERT_EQ(channel->takeSnapshot(stamp), SnapshotResult::ok);
  sink.drain();
  const Snapshot masked = sink->latestSnapshot();
  checkGolden("snapshot_masked.mask", masked.active_mask);
  checkGolden("snapshot_masked.payload", masked.payload);

  // The MCAP records MCAPSink actually wrote: schema, channel and message bodies.
  mcap.worker->stop();
  mcap->stopRecording();
  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(mcap_path).ok());
  std::vector<std::vector<uint8_t>> bodies;
  for(const auto& view : reader.readMessages())
  {
    EXPECT_EQ(view.schema->name,
              "wire_test::" + std::to_string(channel->getSchema().hash));
    EXPECT_EQ(view.schema->encoding, "data_tamer");
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(view.schema->data.data()),
                          view.schema->data.size()),
              schema_text);
    EXPECT_EQ(view.channel->topic, "wire_test");
    EXPECT_EQ(view.channel->messageEncoding, "data_tamer");
    EXPECT_EQ(view.message.logTime, mcap::Timestamp(stamp.count()));
    EXPECT_EQ(view.message.sequence, bodies.size() + 1);  // per channel, from 1
    const auto* data = reinterpret_cast<const uint8_t*>(view.message.data);
    bodies.emplace_back(data, data + view.message.dataSize);
  }
  reader.close();
  std::filesystem::remove(mcap_path);
  ASSERT_EQ(bodies.size(), 2u);
  checkGolden("snapshot_full.mcap_message", bodies[0]);
  checkGolden("snapshot_masked.mcap_message", bodies[1]);
}

// Text (section 1): a std::string is a `string`, which decoders present as text; a
// std::vector<char> is a char[] and a std::array<char, N> a char[N], which they keep
// per character.
TEST(WireFormat, StringsMatchGoldenVectors)
{
  auto channel = LogChannel::create("strings test");
  auto sink = DataTamerTest::manual<DummySink>();
  channel->addDataSink(sink);

  std::string ascii = "data_tamer";
  std::string empty;
  std::string utf8 = "Z\xc3\xbcrich \xe2\x9c\x93";  // "Zürich ✓"
  std::string invalid = "a\xff"
                        "b";  // not UTF-8: a decoder replaces 0xff with U+FFFD
  std::array<char, 3> fixed = { 'a', 'b', 'c' };
  std::vector<char> chars = { 'h', 'i' };
  std::vector<std::string> tags = { "t", "" };
  std::vector<Label> labels = { { 1, "x" }, { 2, "" } };
  channel->registerValue("ascii", &ascii);
  channel->registerValue("empty", &empty);
  channel->registerValue("utf8", &utf8);
  channel->registerValue("invalid", &invalid);
  channel->registerValue("fixed", &fixed);
  channel->registerValue("chars", &chars);
  channel->registerValue("tags", &tags);
  channel->registerValue("labels", &labels);

  ASSERT_EQ(channel->takeSnapshot(std::chrono::nanoseconds(1)), SnapshotResult::ok);
  sink.drain();
  const Snapshot snapshot = sink->latestSnapshot();
  const std::string text = ToStr(channel->getSchema());
  checkGolden("schema_strings.txt", { text.begin(), text.end() });
  checkGolden("snapshot_strings.mask", snapshot.active_mask);
  checkGolden("snapshot_strings.payload", snapshot.payload);

  const auto schema = DataTamerParser::BuildSchemaFromText(text, true);
  std::vector<std::pair<std::string, std::string>> values;  // strings as text, in order
  ASSERT_TRUE(DataTamerParser::ParseSnapshot(
      schema, DataTamerTest::viewOf(snapshot),
      [&](const std::string& name, const DataTamerParser::VarNumber& number) {
        std::visit([&](auto x) { values.emplace_back(name, std::to_string(x)); }, number);
      },
      [&](const std::string& name, std::string_view str) {
        values.emplace_back(name, "\"" + std::string(str) + "\"");
      }));
  const std::vector<std::pair<std::string, std::string>> expected = {
    { "ascii", "\"data_tamer\"" },
    { "empty", "\"\"" },
    { "utf8", "\"Z\xc3\xbcrich \xe2\x9c\x93\"" },
    { "invalid", "\"a\xff"
                 "b\"" },
    { "fixed[0]", std::to_string('a') },
    { "fixed[1]", std::to_string('b') },
    { "fixed[2]", std::to_string('c') },
    { "chars[0]", std::to_string('h') },
    { "chars[1]", std::to_string('i') },
    { "tags[0]", "\"t\"" },
    { "tags[1]", "\"\"" },
    { "labels[0]/id", "1" },
    { "labels[0]/text", "\"x\"" },
    { "labels[1]/id", "2" },
    { "labels[1]/text", "\"\"" },
  };
  EXPECT_EQ(values, expected);
}

// The YAML rendering (section 2.1) of names that are "/"-separated paths:
// nesting, runs that cannot be nested, and keys that need quoting.
TEST(WireFormat, YamlSchemaMatchesGoldenVectors)
{
  auto channel = LogChannel::create("nested test");
  auto sink = DataTamerTest::manual<DummySink>();
  channel->addDataSink(sink);

  double value = 0;
  uint8_t flag = 0;
  StampedPose pose;
  channel->registerValue("arm/joint_1/position", &value);
  channel->registerValue("arm/joint_1/velocity", &value);
  channel->registerValue("arm/joint_2/position", &value);
  channel->registerValue("arm/joint_2/velocity", &value);
  channel->registerValue("arm/mode", &flag);
  channel->registerValue("state", &flag);
  channel->registerValue("arm/late", &value);  // "arm" is taken: stays flat
  channel->registerValue("single/x", &value);  // a run of one: stays flat
  channel->registerValue("dup", &flag);        // a field named like the group
  channel->registerValue("dup/a", &value);
  channel->registerValue("dup/b", &value);
  channel->registerValue("on", &flag);  // YAML 1.1 boolean: quoted
  channel->registerValue("7up/x", &value);
  channel->registerValue("7up/y", &value);  // nested under a quoted key
  channel->registerValue("cart/pose", &pose);

  const Schema schema = channel->getSchema();
  const std::string text = ToStr(schema);
  const std::string yaml = ToYaml(schema);
  checkGolden("schema_nested.txt", { text.begin(), text.end() });
  checkGolden("schema_nested.yaml", { yaml.begin(), yaml.end() });

  // Both renderings parse to the same schema, and the YAML one verifies the
  // hash through the line format it renders back to.
  const auto from_text = DataTamerParser::BuildSchemaFromText(text, true);
  const auto from_yaml = DataTamerParser::BuildSchemaFromText(yaml, true);
  EXPECT_EQ(from_yaml.hash, schema.hash);
  EXPECT_EQ(from_yaml.channel_name, "nested test");
  EXPECT_EQ(from_yaml.fields, from_text.fields);
  EXPECT_EQ(from_yaml.custom_types, from_text.custom_types);
  EXPECT_EQ(DataTamerParser::ToText(from_yaml), text);
  EXPECT_LT(yaml.size(), text.size());
}

// A recording by data_tamer 1.0.4 (schema version 4): custom types in the writer's
// unordered_map order (Pose before the Point3D and Quaternion it holds), disabled fields,
// NaN, infinities, -0, denormals, empty vectors and an opaque type.
TEST(WireFormat, ReadsRecording104)
{
  using DataTamerParser::BuildSchemaFromText;
  const Messages expected = readExpected104();
  ASSERT_EQ(expected.size(), 15u);

  // The schema records are the checked-in texts, and the declared hash is the schema's.
  const Messages decoded =
      decodeRecording104([](const std::string& topic, const std::string& text) {
        const auto file = readFile("v4/schema_" + topic + ".txt");
        EXPECT_EQ(text, std::string(file.begin(), file.end()));
        const auto schema = BuildSchemaFromText(text);
        EXPECT_EQ(replaceHash(text, schema.hash), text);
        EXPECT_NE(schema.hash, DataTamerParser::SchemaTextHash(text));
        return schema;
      });
  EXPECT_EQ(decoded, expected);

  // A writer with another std::hash declared another hash: decoding uses it all the
  // same, and check_hash, which recomputes this platform's, throws.
  const Messages foreign =
      decodeRecording104([](const std::string&, const std::string& text) {
        const std::string other = replaceHash(text, 1234567890123456789u);
        EXPECT_THROW((void)BuildSchemaFromText(other, true), std::runtime_error);
        return BuildSchemaFromText(other);
      });
  EXPECT_EQ(foreign, expected);

#if defined(__GLIBCXX__) && SIZE_MAX == UINT64_MAX
  // The recording was written with libstdc++ and a 64-bit size_t, whose std::hash
  // (Murmur-based for strings, the identity for integers) check_hash reproduces. Other
  // standard libraries (libc++, MSVC) hash strings differently.
  const Messages checked =
      decodeRecording104([](const std::string&, const std::string& text) {
        return BuildSchemaFromText(text, true);
      });
  EXPECT_EQ(checked, expected);
#endif
}
