// Golden test for docs/wire_format.md. The fixture files under
// docs/wire_format/vectors/ are the source of truth: a change in the schema text
// or in the snapshot bytes fails here and must be a deliberate format revision.
//
// Regenerate the fixtures with DATA_TAMER_UPDATE_GOLDEN=1, then review the diff.
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace DataTamer;

namespace
{
struct Point3D
{
  double x = 0;
  double y = 0;
  double z = 0;
};
template <class AddField>
std::string_view TypeDefinition(Point3D& p, AddField& add)
{
  add("x", &p.x);
  add("y", &p.y);
  add("z", &p.z);
  return "Point3D";
}

struct Pose
{
  Point3D position;
  uint32_t stamp = 0;
};
template <class AddField>
std::string_view TypeDefinition(Pose& p, AddField& add)
{
  add("position", &p.position);
  add("stamp", &p.stamp);
  return "Pose";
}

const std::string kDir = DATA_TAMER_WIRE_FORMAT_DIR;

std::vector<uint8_t> readFile(const std::string& name)
{
  std::ifstream file(kDir + "/vectors/" + name, std::ios::binary);
  return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

void writeFile(const std::string& name, const void* data, size_t size)
{
  std::ofstream file(kDir + "/vectors/" + name, std::ios::binary);
  file.write(static_cast<const char*>(data), std::streamsize(size));
}

bool updating() { return std::getenv("DATA_TAMER_UPDATE_GOLDEN") != nullptr; }

// Compare (or, when updating, record) `actual` against the fixture `name`.
void checkGolden(const std::string& name, const std::vector<uint8_t>& actual)
{
  if(updating())
  {
    writeFile(name, actual.data(), actual.size());
    return;
  }
  const auto expected = readFile(name);
  ASSERT_FALSE(expected.empty()) << "missing fixture " << name;
  EXPECT_EQ(actual, expected) << "wire format changed: " << name
                              << " (see docs/wire_format.md)";
}

// Two parts of the schema text are legitimately environment dependent (see the
// spec): the hash line (std::hash is implementation-defined) and the order of
// the MSG sections (unordered_map). Normalize both before comparing.
std::vector<uint8_t> canonicalSchema(std::string text)
{
  const std::string separator = "===========================================================\n";
  std::vector<std::string> sections;
  size_t start = 0;
  for(size_t pos; (pos = text.find(separator, start)) != std::string::npos; start = pos + separator.size())
    sections.push_back(text.substr(start, pos - start));
  sections.push_back(text.substr(start));
  std::sort(sections.begin() + 1, sections.end());
  std::string out;
  for(size_t i = 0; i < sections.size(); ++i)
    out += (i ? separator : "") + sections[i];
  const auto hash_pos = out.find("### hash: ");
  const auto hash_end = out.find('\n', hash_pos);
  out.replace(hash_pos + 10, hash_end - hash_pos - 10, "<implementation-defined>");
  return { out.begin(), out.end() };
}

// The exact bytes MCAPSink stores per message: two SerializeMe vectors.
std::vector<uint8_t> mcapMessage(const Snapshot& snapshot)
{
  std::vector<uint8_t> out(2 * sizeof(uint32_t) + snapshot.active_mask.size() +
                           snapshot.payload.size());
  SerializeMe::SpanBytes buffer(out);
  SerializeMe::SerializeIntoBuffer(buffer, snapshot.active_mask);
  SerializeMe::SerializeIntoBuffer(buffer, snapshot.payload);
  return out;
}

// DummySink delivers on its own thread and has no drain hook: wait until the
// delivered count reaches `count` (bounded, so a regression fails instead of hanging).
Snapshot waitDelivered(DummySink& sink, uint64_t hash, long count)
{
  for(int i = 0; i < 2500 && sink.snapshots_count[hash] < count; ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(sink.snapshots_count[hash], count);
  return sink.latest_snapshot;
}
}  // namespace

TEST(WireFormat, SchemaTextAndSnapshotsMatchGoldenVectors)
{
  auto channel = LogChannel::create("wire_test");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);

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
  Pose pose;
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

  const auto hash = channel->getSchema().hash;
  ASSERT_TRUE(channel->takeSnapshot(std::chrono::nanoseconds(1234567890)));
  const auto full = waitDelivered(*sink, hash, 1);
  ASSERT_EQ(full.active_mask.size(), 2u);  // 16 fields -> 2 mask bytes

  const std::string schema_text = ToStr(channel->getSchema());
  checkGolden("schema.txt", canonicalSchema(schema_text));
  checkGolden("snapshot_full.mask", full.active_mask);
  checkGolden("snapshot_full.payload", full.payload);
  checkGolden("snapshot_full.mcap_message", mcapMessage(full));

  // Disable one scalar and one custom type: their bits clear and their bytes
  // disappear from the payload; everything else keeps its relative order.
  channel->setEnabled(id_i16, false);
  channel->setEnabled(id_pose, false);
  ASSERT_TRUE(channel->takeSnapshot(std::chrono::nanoseconds(1234567890)));
  const auto masked = waitDelivered(*sink, hash, 2);
  ASSERT_EQ(masked.payload.size(), full.payload.size() - sizeof(int16_t) - 3 * sizeof(double) - sizeof(uint32_t));
  checkGolden("snapshot_masked.mask", masked.active_mask);
  checkGolden("snapshot_masked.payload", masked.payload);
  checkGolden("snapshot_masked.mcap_message", mcapMessage(masked));
}
