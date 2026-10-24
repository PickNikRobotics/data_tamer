#pragma once

#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/data_tamer_parser.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>

namespace DataTamerTest
{

/// The standalone parser's view of `snapshot`. It points into the snapshot, which must
/// outlive it.
inline DataTamerParser::SnapshotView viewOf(const DataTamer2::Snapshot& snapshot)
{
  return { snapshot.schema_hash,
           uint64_t(snapshot.timestamp.count()),
           { snapshot.active_mask.data(), snapshot.active_mask.size() },
           { snapshot.payload.data(), snapshot.payload.size() } };
}

/// What the standalone parser reads from `snapshot` with the schema `channel` wrote:
/// every number by the name of its field. The payload must end with the last field.
inline std::map<std::string, double> decode(const DataTamer2::LogChannel& channel,
                                            const DataTamer2::Snapshot& snapshot)
{
  const auto schema =
      DataTamerParser::BuildSchemaFromText(DataTamer2::ToStr(channel.getSchema()));
  std::map<std::string, double> values;
  const bool complete = DataTamerParser::ParseSnapshot(
      schema, viewOf(snapshot),
      [&](const std::string& name, const DataTamerParser::VarNumber& number) {
        values[name] = std::visit([](const auto& x) { return double(x); }, number);
      });
  EXPECT_TRUE(complete) << "bytes are left after the last field";
  return values;
}

/// What the parser reads from `snapshot`, with a string callback: the numbers, and the
/// strings as text, by name.
struct Decoded
{
  std::map<std::string, double> numbers;
  std::map<std::string, std::string> strings;
};

inline Decoded decodeWithStrings(const DataTamer2::LogChannel& channel,
                                 const DataTamer2::Snapshot& snapshot)
{
  const auto schema =
      DataTamerParser::BuildSchemaFromText(DataTamer2::ToStr(channel.getSchema()));
  Decoded out;
  const bool complete = DataTamerParser::ParseSnapshot(
      schema, viewOf(snapshot),
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

}  // namespace DataTamerTest
