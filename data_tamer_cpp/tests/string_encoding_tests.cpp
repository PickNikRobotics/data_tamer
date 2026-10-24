#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/contrib/SerializeMe.hpp"
#include "data_tamer_v2/custom_types.hpp"
#include "alloc_counter.hpp"
#include "decode_utils.hpp"
#include "guarded_buffer.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// A std::string or std::string_view is the schema type `string`: a uint32 byte count and
// that many bytes (docs/wire_format.md). The payload must hold exactly that.

namespace
{
struct Named
{
  uint8_t tag = 1;
  std::string name;
};

// A custom type whose name is a built-in type's.
struct Impostor
{
  int32_t x = 0;
};
template <typename AddField>
std::string_view TypeDefinition(Impostor& p, AddField& add)
{
  add("x", &p.x);
  return "string";
}
template <typename AddField>
std::string_view TypeDefinition(Named& p, AddField& add)
{
  add("tag", &p.tag);
  add("name", &p.name);
  return "Named";
}

}  // namespace

using DataTamerTest::decodeWithStrings;

TEST(StringEncoding, StringMemberIsRecordedAsText)
{
  DataTamerTest::Recording recording;
  Named named;
  named.name = "abc";
  recording.channel->registerValue("n", &named);

  const auto snapshot = recording.snapshot();
  EXPECT_EQ(snapshot.payload, (std::vector<uint8_t>{ 1, 3, 0, 0, 0, 'a', 'b', 'c' }));
  EXPECT_NE(DataTamer2::ToStr(recording.channel->getSchema()).find("string name\n"),
            std::string::npos);
  // Without a string callback the parser passes the numbers only.
  const std::map<std::string, double> expected = { { "n/tag", 1 } };
  EXPECT_EQ(DataTamerTest::decode(*recording.channel, snapshot), expected);
}

TEST(StringEncoding, RegisteredStringIsRecordedAsText)
{
  DataTamerTest::Recording recording;
  std::string text = "abc";
  recording.channel->registerValue("s", &text);

  EXPECT_EQ(recording.payload(), (std::vector<uint8_t>{ 3, 0, 0, 0, 'a', 'b', 'c' }));
  const auto field = recording.field();
  EXPECT_EQ(field.type, DataTamer2::BasicType::STRING);
  EXPECT_FALSE(field.is_vector);
  EXPECT_EQ(field.type_name, "string");
}

// Every text type is a `string`, also in a container; a std::vector<char> is not text.
TEST(StringEncoding, TextTypesAndContainersOfTextHaveTheirSchemaType)
{
  DataTamerTest::Recording recording;
  std::string text = "s";
  std::string_view view = "view";  // a literal: the characters outlive the channel
  std::pmr::string pmr = "pmr";
  std::vector<std::string> list = { "a", "" };
  std::array<std::string, 2> pair = { "b", "c" };
  std::vector<char> chars = { 'h', 'i' };
  auto logged = recording.channel->createLoggedValue<std::string>("logged", "lv");
  recording.channel->registerValue("text", &text);
  recording.channel->registerValue("view", &view);
  recording.channel->registerValue("pmr", &pmr);
  recording.channel->registerValue("list", &list);
  recording.channel->registerValue("pair", &pair);
  recording.channel->registerValue("chars", &chars);

  const std::string schema = DataTamer2::ToStr(recording.channel->getSchema());
  for(const char* line :
      { "\nstring logged\n", "\nstring text\n", "\nstring view\n", "\nstring pmr\n",
        "\nstring[] list\n", "\nstring[2] pair\n", "\nchar[] chars\n" })
  {
    EXPECT_NE(schema.find(line), std::string::npos) << line << " in\n" << schema;
  }
  const auto decoded = decodeWithStrings(*recording.channel, recording.snapshot());
  const std::map<std::string, std::string> strings = {
    { "logged", "lv" }, { "text", "s" },   { "view", "view" }, { "pmr", "pmr" },
    { "list[0]", "a" }, { "list[1]", "" }, { "pair[0]", "b" }, { "pair[1]", "c" }
  };
  EXPECT_EQ(decoded.strings, strings);
  const std::map<std::string, double> numbers = { { "chars[0]", 'h' },
                                                  { "chars[1]", 'i' } };
  EXPECT_EQ(decoded.numbers, numbers);
}

TEST(StringEncoding, CustomTypeNamedAsABuiltInTypeIsRefused)
{
  auto channel = DataTamer2::LogChannel::create("c");
  Impostor impostor;
  EXPECT_THROW(channel->registerValue("i", &impostor), std::runtime_error);
  EXPECT_TRUE(channel->getSchema().fields.empty());
}

// A text adds no allocation to the real-time snapshot, whatever its length, as long as
// the slot holds it; past that the snapshot is dropped as oversize, not truncated.
TEST(StringEncoding, RealTimeSnapshotOfTextAllocatesNothing)
{
  auto sink = DataTamerTest::manual<DataTamer2::DummySink>();
  auto channel = DataTamer2::LogChannel::create("c");
  std::string text;
  text.reserve(300);
  channel->registerValue("text", &text);
  channel->setPayloadCapacity(4 + 300);
  channel->addDataSink(sink);
  channel->startLogging();
  sink.drain();  // The first delivery pass sizes the worker's pass list.
  // The sink copies what it receives (on its worker): only the snapshots are counted.
  const auto counted = [&](auto&& step) {
    DataTamerTest::AllocCounter::Scope scope;
    step();
    return scope.allocations();
  };
  text.assign(300, 'x');  // within the capacity reserved above
  EXPECT_EQ(counted([&] {
              EXPECT_EQ(channel->tryTakeSnapshot(), DataTamer2::SnapshotResult::ok);
            }),
            0u);
  sink.drain();
  EXPECT_EQ(sink->latestPayloadSize(), size_t{ 4 + 300 });
  text = "short";
  EXPECT_EQ(counted([&] {
              EXPECT_EQ(channel->tryTakeSnapshot(), DataTamer2::SnapshotResult::ok);
            }),
            0u);
  sink.drain();
  EXPECT_EQ(sink->latestPayloadSize(), size_t{ 4 + 5 });
  text.assign(100'000, 'y');
  EXPECT_EQ(channel->tryTakeSnapshot(), DataTamer2::SnapshotResult::oversize);
  EXPECT_EQ(channel->droppedOversize(), 1u);
}

// The whole of a string longer than 64 KiB is serialized, whether it was that long when
// the channel started (and a real-time snapshot takes it) or grew afterwards (and the
// snapshot may grow the slot).
TEST(StringEncoding, StringLongerThan64KiBIsSerialized)
{
  struct Case
  {
    const char* label;
    std::string at_start;  // the string when logging starts
    bool real_time;        // tryTakeSnapshot() or takeSnapshot()
  };
  const std::string big(70 * 1024, 'x');
  for(const auto& c : { Case{ "long at the start", big, true },
                        Case{ "grown after the start", "abc", false } })
  {
    SCOPED_TRACE(c.label);
    DataTamerTest::Recording recording;
    Named named;
    named.name = c.at_start;
    recording.channel->registerValue("n", &named);
    recording.channel->startLogging();

    named.name = big;
    DataTamer2::SnapshotResult result = DataTamer2::SnapshotResult::rejected;
    EXPECT_NO_THROW(result = c.real_time ? recording.channel->tryTakeSnapshot() :
                                           recording.channel->takeSnapshot());
    EXPECT_EQ(result, DataTamer2::SnapshotResult::ok);
    recording.sink.drain();
    EXPECT_EQ(recording.sink->latestPayloadSize(), size_t{ 1 + 4 + 70 * 1024 });
  }
}

// With the template argument written out too, as 1.x code may call them.
TEST(StringEncoding, SerializeMeSizesAndRoundTripsAString)
{
  const std::string text = "hello";
  EXPECT_EQ(SerializeMe::BufferSize(text), size_t{ 4 + 5 });
  EXPECT_EQ(SerializeMe::BufferSize<std::string>(text), size_t{ 4 + 5 });

  std::vector<uint8_t> storage(SerializeMe::BufferSize(text));
  SerializeMe::SpanBytes out(storage);
  SerializeMe::SerializeIntoBuffer<std::string>(out, text);
  EXPECT_EQ(storage, (std::vector<uint8_t>{ 5, 0, 0, 0, 'h', 'e', 'l', 'l', 'o' }));

  SerializeMe::SpanBytesConst in(storage);
  std::string decoded;
  SerializeMe::DeserializeFromBuffer<std::string>(in, decoded);
  EXPECT_EQ(decoded, text);
  EXPECT_EQ(in.size(), 0u);
}

TEST(StringEncoding, SerializeMeRejectsALengthLargerThanTheBytesThatFollow)
{
  DataTamerTest::GuardedBuffer buffer({ 0xff, 0xff, 0xff, 0xff, 'a', 'b' });
  SerializeMe::SpanBytesConst in(buffer.data(), buffer.size());
  std::string decoded;
  EXPECT_THROW(SerializeMe::DeserializeFromBuffer(in, decoded), std::runtime_error);
}

// With a string callback, the parser passes a text as one value, empty or not, at the
// top level, in a custom type and in a vector of them. A std::array<char, N> is a fixed
// char[N]: it stays one number per character.
TEST(StringEncoding, ParserWithAStringCallbackPassesAStringAsOneValue)
{
  DataTamerTest::Recording recording;
  std::string text = "Z\xc3\xbcrich";  // passed as written: UTF-8 here
  std::string empty;
  Named named;
  named.name = "abc";
  std::vector<Named> list(2);
  list[1].name = "x";
  std::array<char, 2> fixed = { 'o', 'k' };
  recording.channel->registerValue("text", &text);
  recording.channel->registerValue("empty", &empty);
  recording.channel->registerValue("n", &named);
  recording.channel->registerValue("list", &list);
  recording.channel->registerValue("fixed", &fixed);

  const auto snapshot = recording.snapshot();
  const auto decoded = decodeWithStrings(*recording.channel, snapshot);
  const std::map<std::string, std::string> strings = { { "text", "Z\xc3\xbcrich" },
                                                       { "empty", "" },
                                                       { "n/name", "abc" },
                                                       { "list[0]/name", "" },
                                                       { "list[1]/name", "x" } };
  EXPECT_EQ(decoded.strings, strings);
  const std::map<std::string, double> numbers = { { "n/tag", 1 },
                                                  { "list[0]/tag", 1 },
                                                  { "list[1]/tag", 1 },
                                                  { "fixed[0]", 'o' },
                                                  { "fixed[1]", 'k' } };
  EXPECT_EQ(decoded.numbers, numbers);
}

// A string is read at each snapshot: its length may change from one to the next.
TEST(StringEncoding, StringChangesLengthBetweenSnapshots)
{
  DataTamerTest::Recording recording;
  std::string text = "short";
  recording.channel->registerValue("text", &text);
  for(const std::string value : { "short", "a longer string", "", "x" })
  {
    text = value;
    const auto snapshot = recording.snapshot();
    EXPECT_EQ(snapshot.payload.size(), 4 + value.size());
    EXPECT_EQ(decodeWithStrings(*recording.channel, snapshot).strings.at("text"), value);
  }
}

// The text points into the payload: the bytes as written, '\0' included.
TEST(StringEncoding, StringCallbackGetsAViewOfThePayload)
{
  DataTamerParser::Schema schema;
  schema.fields.push_back(
      { "s", DataTamerParser::BasicType::STRING, "string", false, 0 });
  const std::vector<uint8_t> mask = { 1 };
  const std::vector<uint8_t> payload = { 3, 0, 0, 0, 'a', 0, 0xff };
  const DataTamerParser::SnapshotView view{
    0, 0, { mask.data(), mask.size() }, { payload.data(), payload.size() }
  };
  std::string_view seen;
  const auto no_number = [](const std::string& name, const DataTamerParser::VarNumber&) {
    ADD_FAILURE() << name;
  };
  ASSERT_TRUE(DataTamerParser::ParseSnapshot(
      schema, view, no_number, [&](const std::string& name, std::string_view text) {
        EXPECT_EQ(name, "s");
        seen = text;
      }));
  EXPECT_EQ(static_cast<const void*>(seen.data()), payload.data() + 4);
  EXPECT_EQ(seen, std::string_view("a\0\xff", 3));
}

TEST(StringEncoding, StringLongerThanThePayloadThrows)
{
  DataTamerParser::Schema schema;
  schema.fields.push_back(
      { "s", DataTamerParser::BasicType::STRING, "string", false, 0 });
  const std::vector<uint8_t> mask = { 1 };
  for(const auto& payload : { std::vector<uint8_t>{ 4, 0, 0, 0, 'a', 'b', 'c' },
                              std::vector<uint8_t>{ 1, 0, 0 } })
  {
    const DataTamerParser::SnapshotView view{
      0, 0, { mask.data(), mask.size() }, { payload.data(), payload.size() }
    };
    bool called = false;
    EXPECT_THROW(std::ignore = DataTamerParser::ParseSnapshot(
                     schema, view,
                     [](const std::string&, const DataTamerParser::VarNumber&) {},
                     [&](const std::string&, std::string_view) { called = true; }),
                 std::runtime_error);
    EXPECT_FALSE(called);
  }
}
