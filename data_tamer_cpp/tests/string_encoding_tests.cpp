#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/contrib/SerializeMe.hpp"
#include "data_tamer_v2/custom_types.hpp"
#include "decode_utils.hpp"
#include "guarded_buffer.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The schema describes a std::string as `char[]`: a uint32 count and that many chars
// (docs/wire_format.md). The payload must hold exactly that.

namespace
{
struct Named
{
  uint8_t tag = 1;
  std::string name;
};
template <typename AddField>
std::string_view TypeDefinition(Named& p, AddField& add)
{
  add("tag", &p.tag);
  add("name", &p.name);
  return "Named";
}

/// What the parser reads from `snapshot`, with a string callback: the numbers, and the
/// strings as text, by name.
struct Decoded
{
  std::map<std::string, double> numbers;
  std::map<std::string, std::string> strings;
};

Decoded decodeWithStrings(const DataTamer2::LogChannel& channel,
                          const DataTamer2::Snapshot& snapshot)
{
  const auto schema =
      DataTamerParser::BuildSchemaFromText(DataTamer2::ToStr(channel.getSchema()));
  Decoded out;
  const bool complete = DataTamerParser::ParseSnapshot(
      schema, DataTamerTest::viewOf(snapshot),
      [&](const std::string& name, const DataTamerParser::VarNumber& number) {
        out.numbers[name] = std::visit([](const auto& x) { return double(x); }, number);
      },
      [&](const std::string& name, std::string_view text) {
        EXPECT_EQ(out.strings.count(name), 0u) << name;
        out.strings[name] = std::string(text);
      });
  EXPECT_TRUE(complete) << "bytes are left after the last field";
  return out;
}
}  // namespace

TEST(StringEncoding, StringMemberIsSerializedAsACharVector)
{
  DataTamerTest::Recording recording;
  Named named;
  named.name = "abc";
  recording.channel->registerValue("n", &named);

  const auto snapshot = recording.snapshot();
  EXPECT_EQ(snapshot.payload, (std::vector<uint8_t>{ 1, 3, 0, 0, 0, 'a', 'b', 'c' }));
  EXPECT_NE(DataTamer2::ToStr(recording.channel->getSchema()).find("char[] name\n"),
            std::string::npos);
  const std::map<std::string, double> expected = {
    { "n/tag", 1 }, { "n/name[0]", 'a' }, { "n/name[1]", 'b' }, { "n/name[2]", 'c' }
  };
  EXPECT_EQ(DataTamerTest::decode(*recording.channel, snapshot), expected);
}

TEST(StringEncoding, RegisteredStringIsSerializedAsACharVector)
{
  DataTamerTest::Recording recording;
  std::string text = "abc";
  recording.channel->registerValue("s", &text);

  EXPECT_EQ(recording.payload(), (std::vector<uint8_t>{ 3, 0, 0, 0, 'a', 'b', 'c' }));
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

TEST(StringEncoding, SerializeMeSizesAndRoundTripsAString)
{
  const std::string text = "hello";
  EXPECT_EQ(SerializeMe::BufferSize(text), size_t{ 4 + 5 });

  std::vector<uint8_t> storage(SerializeMe::BufferSize(text));
  SerializeMe::SpanBytes out(storage);
  SerializeMe::SerializeIntoBuffer(out, text);
  EXPECT_EQ(storage, (std::vector<uint8_t>{ 5, 0, 0, 0, 'h', 'e', 'l', 'l', 'o' }));

  SerializeMe::SpanBytesConst in(storage);
  std::string decoded;
  SerializeMe::DeserializeFromBuffer(in, decoded);
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

// With a string callback, the parser passes a std::string as one value, empty or not,
// at the top level, in a custom type and in a vector of them. A std::array<char, N> is
// a fixed char[N]: it stays one number per character.
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
  schema.fields.push_back({ "s", DataTamerParser::BasicType::CHAR, "char", true, 0 });
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
  schema.fields.push_back({ "s", DataTamerParser::BasicType::CHAR, "char", true, 0 });
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
