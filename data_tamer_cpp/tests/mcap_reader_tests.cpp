// Decoding helpers: ToSnapshotView() for library snapshots and the MCAP reader
// (ForEachSnapshot(), ForEachValue()) on recordings of MCAPSink.
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/data_tamer_parser.hpp"
#include "data_tamer_v2/decode.hpp"
#include "data_tamer_v2/mcap_reader.hpp"
#include "data_tamer_v2/sinks/dummy_sink.hpp"
#include "data_tamer_v2/sinks/mcap_encoding.hpp"
#include "data_tamer_v2/sinks/mcap_sink.hpp"
#include "mcap_test_utils.hpp"
#include "test_sinks.hpp"

#include <mcap/writer.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace DataTamer2;
using DataTamerTest::manual;
using std::chrono::nanoseconds;

namespace
{
using Value = std::tuple<std::string, uint64_t, std::string, double>;

double toDouble(const DataTamerParser::VarNumber& value)
{
  return std::visit([](auto number) { return static_cast<double>(number); }, value);
}

// Two channels in one MCAPSink file, taken out of time order: "a" at 30 and 20, "b" at
// 10 (with b/y disabled).
std::string recordTwoChannels(const DataTamerTest::ScratchDir& dir)
{
  const std::string path = dir.file("two.mcap");
  auto sink = manual<MCAPSink>(path);
  auto a = LogChannel::create("a");
  auto b = LogChannel::create("b");
  int32_t x = 0;
  double y = 0;
  a->registerValue("x", &x);
  b->registerValue("x", &x);
  const auto b_y = b->registerValue("y", &y);
  a->addDataSink(sink);
  b->addDataSink(sink);
  x = 3;
  EXPECT_EQ(a->takeSnapshot(nanoseconds(30)), SnapshotResult::ok);
  x = 1;
  y = 1.5;
  b->setEnabled(b_y, false);
  EXPECT_EQ(b->takeSnapshot(nanoseconds(10)), SnapshotResult::ok);
  x = 2;
  EXPECT_EQ(a->takeSnapshot(nanoseconds(20)), SnapshotResult::ok);
  sink.worker->stop();
  return path;
}

std::vector<Value> readValues(const std::string& path,
                              const std::vector<std::string>& topics = {})
{
  std::vector<Value> values;
  ForEachValue(
      path,
      [&](const std::string& topic, uint64_t timestamp, const std::string& name,
          const DataTamerParser::VarNumber& value) {
        values.emplace_back(topic, timestamp, name, toDouble(value));
      },
      topics);
  return values;
}

// A data_tamer file with one channel "c" (one int32 "v") and the message `body`,
// written as is, plus a channel of another encoding.
std::string writeFile(const DataTamerTest::ScratchDir& dir, const std::string& name,
                      const std::vector<uint8_t>& body)
{
  auto channel = LogChannel::create("c");
  int32_t v = 0;
  channel->registerValue("v", &v);
  const std::string path = dir.file(name);
  mcap::McapWriter writer;
  EXPECT_TRUE(writer.open(path, mcap::McapWriterOptions(mcap_encoding::kEncoding)).ok());
  const auto id = mcap_encoding::AddChannel(writer, channel->getSchema());
  mcap::Schema json_schema("other", "jsonschema", "{}");
  writer.addSchema(json_schema);
  mcap::Channel json("json_topic", "json", json_schema.id);
  writer.addChannel(json);
  const auto write = [&](mcap::ChannelId channel_id, uint64_t time, const auto& bytes) {
    mcap::Message msg;
    msg.channelId = channel_id;
    msg.logTime = msg.publishTime = time;
    msg.data = reinterpret_cast<const std::byte*>(bytes.data());
    msg.dataSize = bytes.size();
    EXPECT_TRUE(writer.write(msg).ok());
  };
  write(json.id, 1, std::string("{}"));
  write(id, 2, body);
  writer.close();
  return path;
}

// u32 mask length, mask, u32 payload length, payload (docs/wire_format.md, 4.1).
std::vector<uint8_t> messageBody(std::vector<uint8_t> payload, size_t trailing = 0)
{
  std::vector<uint8_t> out;
  mcap_encoding::EncodeMessageBody(std::vector<uint8_t>{ 0x01 }, payload, out);
  out.resize(out.size() + trailing);
  return out;
}

template <typename Function>
std::string errorOf(const Function& function)
{
  try
  {
    function();
  }
  catch(const std::runtime_error& e)
  {
    return e.what();
  }
  ADD_FAILURE() << "no std::runtime_error";
  return {};
}
}  // namespace

TEST(McapReader, ForEachValueDecodesInLogTimeOrder)
{
  DataTamerTest::ScratchDir dir("mcap_reader");
  const auto path = recordTwoChannels(dir);
  const std::vector<Value> expected = {
    { "b", 10, "x", 1 },
    { "a", 20, "x", 2 },
    { "a", 30, "x", 3 },
  };
  EXPECT_EQ(readValues(path), expected);
}

// With a string callback, a std::string comes as one value; without one, it is skipped.
TEST(McapReader, ForEachValueWithAStringCallbackPassesStringsAsText)
{
  DataTamerTest::ScratchDir dir("mcap_reader");
  const std::string path = dir.file("strings.mcap");
  {
    auto sink = manual<MCAPSink>(path);
    auto channel = LogChannel::create("s");
    std::string label = "first";
    int32_t x = 7;
    channel->registerValue("label", &label);
    channel->registerValue("x", &x);
    channel->addDataSink(sink);
    EXPECT_EQ(channel->takeSnapshot(nanoseconds(10)), SnapshotResult::ok);
    label.clear();
    EXPECT_EQ(channel->takeSnapshot(nanoseconds(20)), SnapshotResult::ok);
    sink.worker->stop();
  }
  std::vector<Value> numbers;
  std::vector<std::tuple<std::string, uint64_t, std::string, std::string>> strings;
  const size_t count = ForEachValue(
      path,
      [&](const std::string& topic, uint64_t timestamp, const std::string& name,
          const DataTamerParser::VarNumber& value) {
        numbers.emplace_back(topic, timestamp, name, toDouble(value));
      },
      [&](const std::string& topic, uint64_t timestamp, const std::string& name,
          std::string_view text) { strings.emplace_back(topic, timestamp, name, text); },
      { "s" });
  EXPECT_EQ(count, 2u);
  EXPECT_EQ(numbers, (std::vector<Value>{ { "s", 10, "x", 7 }, { "s", 20, "x", 7 } }));
  EXPECT_EQ(strings, (decltype(strings){ { "s", 10, "label", "first" },
                                         { "s", 20, "label", "" } }));
  EXPECT_EQ(readValues(path).size(), 2u);
}

TEST(McapReader, ForEachSnapshotFillsTimestampAndHash)
{
  DataTamerTest::ScratchDir dir("mcap_reader");
  const auto path = recordTwoChannels(dir);
  std::vector<uint64_t> stamps;
  const size_t count = ForEachSnapshot(
      path, [&](const std::string& topic, const DataTamerParser::Schema& schema,
                const DataTamerParser::SnapshotView& snapshot) {
        EXPECT_EQ(topic, schema.channel_name);
        EXPECT_EQ(snapshot.schema_hash, schema.hash);
        stamps.push_back(snapshot.timestamp);
      });
  EXPECT_EQ(count, 3u);
  EXPECT_EQ(stamps, (std::vector<uint64_t>{ 10, 20, 30 }));
}

TEST(McapReader, TopicFilterRestrictsTheChannels)
{
  DataTamerTest::ScratchDir dir("mcap_reader");
  const auto path = recordTwoChannels(dir);
  const std::vector<Value> expected = { { "a", 20, "x", 2 }, { "a", 30, "x", 3 } };
  EXPECT_EQ(readValues(path, { "a" }), expected);
  EXPECT_TRUE(readValues(path, { "none" }).empty());
}

TEST(McapReader, SkipsOtherEncodings)
{
  DataTamerTest::ScratchDir dir("mcap_reader");
  const auto path = writeFile(dir, "ok.mcap", messageBody({ 7, 0, 0, 0 }));
  const std::vector<Value> expected = { { "c", 2, "v", 7 } };
  EXPECT_EQ(readValues(path), expected);
}

TEST(McapReader, CorruptMessageThrowsNamingFileAndTopic)
{
  DataTamerTest::ScratchDir dir("mcap_reader");
  // Bytes after the payload: SplitMcapMessage() refuses the body.
  const auto trailing = writeFile(dir, "trailing.mcap", messageBody({ 7, 0, 0, 0 }, 1));
  const auto none = [](const auto&...) {};
  std::string what = errorOf([&] { ForEachSnapshot(trailing, none); });
  EXPECT_NE(what.find(trailing + ": c: corrupt message"), std::string::npos) << what;
  EXPECT_NE(what.find("trailing bytes"), std::string::npos) << what;

  // A payload one byte too long: ParseSnapshot() returns false.
  const auto longer = writeFile(dir, "longer.mcap", messageBody({ 7, 0, 0, 0, 0 }));
  what = errorOf([&] { readValues(longer); });
  EXPECT_NE(what.find(longer + ": c: corrupt message"), std::string::npos) << what;
  EXPECT_NE(what.find("does not match the schema"), std::string::npos) << what;

  // A payload too short: ParseSnapshot() throws.
  const auto shorter = writeFile(dir, "shorter.mcap", messageBody({ 7, 0 }));
  what = errorOf([&] { readValues(shorter); });
  EXPECT_NE(what.find(shorter + ": c: corrupt message"), std::string::npos) << what;
  EXPECT_NE(what.find("truncated"), std::string::npos) << what;
}

TEST(McapReader, MalformedSchemaThrowsNamingFileAndTopic)
{
  DataTamerTest::ScratchDir dir("mcap_reader");
  const std::string path = dir.file("schema.mcap");
  {
    mcap::McapWriter writer;
    ASSERT_TRUE(
        writer.open(path, mcap::McapWriterOptions(mcap_encoding::kEncoding)).ok());
    mcap::Schema schema("bad::1", mcap_encoding::kEncoding, "not a schema");
    writer.addSchema(schema);
    mcap::Channel channel("bad", mcap_encoding::kEncoding, schema.id);
    writer.addChannel(channel);
    const auto bytes = messageBody({});
    mcap::Message msg;
    msg.channelId = channel.id;
    msg.data = reinterpret_cast<const std::byte*>(bytes.data());
    msg.dataSize = bytes.size();
    ASSERT_TRUE(writer.write(msg).ok());
    writer.close();
  }
  const std::string what = errorOf([&] { readValues(path); });
  EXPECT_EQ(what.rfind(path + ": bad: malformed schema: ", 0), 0u) << what;
}

TEST(McapReader, CallbackExceptionsPassUnchanged)
{
  DataTamerTest::ScratchDir dir("mcap_reader");
  const auto path = writeFile(dir, "ok.mcap", messageBody({ 7, 0, 0, 0 }));
  const std::string what = errorOf([&] {
    ForEachValue(path, [](const auto&...) { throw std::runtime_error("mine"); });
  });
  EXPECT_EQ(what, "mine");
}

TEST(McapReader, UnreadableFileThrowsNamingIt)
{
  DataTamerTest::ScratchDir dir("mcap_reader");
  const std::string missing = dir.file("missing.mcap");
  const auto none = [](const auto&...) {};
  std::string what = errorOf([&] { ForEachSnapshot(missing, none); });
  EXPECT_EQ(what.rfind(missing + ": can't open", 0), 0u) << what;

  const std::string text = dir.file("text.mcap");
  std::ofstream(text) << "this is not an MCAP file, only text\n";
  what = errorOf([&] { ForEachSnapshot(text, none); });
  EXPECT_EQ(what.rfind(text + ": can't open", 0), 0u) << what;
}

// A process that crashes leaves no summary and maybe a partial chunk: the snapshots of
// the complete chunks are read, in the order the sink wrote them.
TEST(McapReader, CrashedRecordingYieldsTheSnapshotsBeforeTheCut)
{
  DataTamerTest::ScratchDir dir("mcap_reader");
  const std::string path = dir.file("crashed.mcap");
  constexpr int kSnapshots = 300;  // of 8 KB: about three chunks of MCAP's 768 KiB
  {
    auto sink = manual<MCAPSink>(path);
    auto channel = LogChannel::create("big");
    std::vector<double> v(1000);
    channel->registerValue("v", &v);
    channel->addDataSink(sink);
    for(int i = 0; i < kSnapshots; ++i)
    {
      v[0] = i;
      ASSERT_EQ(channel->takeSnapshot(nanoseconds(1000 + i)), SnapshotResult::ok);
      sink.drain();
    }
    sink.worker->stop();
  }
  const auto size = std::filesystem::file_size(path);
  const auto firstValues = [&](uintmax_t cut_size) {
    const std::string cut = dir.file("cut.mcap");
    std::filesystem::copy_file(path, cut,
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::resize_file(cut, cut_size);
    std::vector<std::pair<uint64_t, double>> values;
    const size_t count = ForEachValue(cut, [&](const std::string&, uint64_t timestamp,
                                               const std::string& name,
                                               const DataTamerParser::VarNumber& value) {
      if(name == "v[0]")
      {
        values.emplace_back(timestamp, toDouble(value));
      }
    });
    EXPECT_EQ(count, values.size());
    for(size_t i = 0; i < values.size(); ++i)
    {
      EXPECT_EQ(values[i], std::make_pair(uint64_t(1000 + i), double(i))) << i;
    }
    return values.size();
  };
  // Without the footer: everything.
  EXPECT_EQ(firstValues(size - 20), size_t(kSnapshots));
  // Cut inside the second chunk: the first one.
  const size_t before_cut = firstValues(size * 6 / 10);
  EXPECT_GT(before_cut, 0u);
  EXPECT_LT(before_cut, size_t(kSnapshots));
}

TEST(McapReader, ToSnapshotViewRoundTripsThroughParseSnapshot)
{
  auto sink = manual<DummySink>();
  auto channel = LogChannel::create("view");
  int32_t x = 5;
  double y = 2.5;
  channel->registerValue("x", &x);
  channel->registerValue("y", &y);
  channel->addDataSink(sink);
  ASSERT_EQ(channel->takeSnapshot(nanoseconds(42)), SnapshotResult::ok);
  sink.drain();
  const Snapshot snapshot = sink->latestSnapshot();
  const auto schema = DataTamerParser::BuildSchemaFromText(ToStr(channel->getSchema()));

  const DataTamerParser::SnapshotView view = ToSnapshotView(snapshot);  // by ADL
  EXPECT_EQ(view.timestamp, 42u);
  EXPECT_EQ(view.schema_hash, schema.hash);
  std::vector<std::pair<std::string, double>> values;
  EXPECT_TRUE(DataTamerParser::ParseSnapshot(
      schema, view, [&](const std::string& name, const DataTamerParser::VarNumber& v) {
        values.emplace_back(name, toDouble(v));
      }));
  EXPECT_EQ(values,
            (std::vector<std::pair<std::string, double>>{ { "x", 5 }, { "y", 2.5 } }));
}
