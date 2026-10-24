#pragma once

#include "data_tamer_v2/data_tamer_parser.hpp"
#include "data_tamer_v2/sinks/mcap_encoding.hpp"

// Do not define MCAP_IMPLEMENTATION: link data_tamer::data_tamer, as for
// sinks/mcap_encoding.hpp.
#include <mcap/reader.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * Reading the MCAP files of MCAPSink and MCAPRingSink, the C++ counterpart of Python's
 * iter_mcap():
 *
 *   DataTamer2::ForEachValue("run.mcap", [](const std::string& topic, uint64_t timestamp,
 *                                           const std::string& name,
 *                                           const DataTamerParser::VarNumber& value) {
 *     ...
 *   });
 *
 * Messages come in log time order (the snapshot timestamps); channels whose message
 * encoding is not data_tamer's are skipped. A recording whose process crashed, which has
 * no summary and may end in a partial chunk, is read too: the messages of its complete
 * chunks, in file order (as the sink wrote them), since it has no message index. So
 * is a file written without chunks. One call reads one file: read the numbered files of
 * a rollover (run.mcap, run_1.mcap, ...) one after the other. Errors throw
 * std::runtime_error, whose message names the file, the topic where there is one, and
 * the reason: a file that can't be opened or is not MCAP, a malformed schema or a
 * corrupt message.
 */
namespace DataTamer2
{

/// Calls `callback(const std::string& topic, const DataTamerParser::Schema& schema,
/// const DataTamerParser::SnapshotView& snapshot)` for each data_tamer message of the
/// MCAP file `path`, with the timestamp (log time) and schema hash of `snapshot` filled
/// in; decode it with DataTamerParser::ParseSnapshot(). The views are valid during the
/// call only. `topics`, if not empty, restricts the channels read.
/// Returns the number of snapshots.
template <typename SnapshotCallback>
size_t ForEachSnapshot(const std::string& path, const SnapshotCallback& callback,
                       const std::vector<std::string>& topics = {})
{
  const auto error = [&path](const std::string& what) {
    return std::runtime_error(path + ": " + what);
  };
  mcap::McapReader reader;
  if(const auto status = reader.open(path); !status.ok())
  {
    throw error("can't open the MCAP file: " + status.message);
  }
  // Without a summary (a crash), this scans the records up to the first incomplete one.
  if(const auto status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
     !status.ok())
  {
    throw error("can't read the MCAP file: " + status.message);
  }
  // A scan finds no message index, which the log time order needs.
  const auto& chunks = reader.chunkIndexes();
  const bool indexed = std::any_of(chunks.begin(), chunks.end(), [](const auto& chunk) {
    return chunk.messageIndexLength > 0;
  });
  mcap::ReadMessageOptions options;
  options.readOrder = indexed ? mcap::ReadMessageOptions::ReadOrder::LogTimeOrder :
                                mcap::ReadMessageOptions::ReadOrder::FileOrder;
  if(!topics.empty())
  {
    options.topicFilter = [&topics](std::string_view topic) {
      return std::find(topics.begin(), topics.end(), topic) != topics.end();
    };
  }
  std::string problem;  // the first one the reader reports
  const auto on_problem = [&problem](const mcap::Status& status) {
    if(problem.empty())
    {
      problem = status.message;
    }
  };
  std::unordered_map<mcap::SchemaId, DataTamerParser::Schema> schemas;
  size_t count = 0;
  for(const auto& message : reader.readMessages(on_problem, options))
  {
    if(!problem.empty())
    {
      break;
    }
    if(!message.schema || message.schema->encoding != mcap_encoding::kEncoding ||
       message.channel->messageEncoding != mcap_encoding::kEncoding)
    {
      continue;
    }
    const std::string& topic = message.channel->topic;
    auto it = schemas.find(message.schema->id);
    if(it == schemas.end())
    {
      const auto& data = message.schema->data;
      const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
      DataTamerParser::Schema schema;
      try
      {
        schema = DataTamerParser::BuildSchemaFromText(text);
      }
      catch(const std::runtime_error& e)
      {
        throw error(topic + ": malformed schema: " + e.what());
      }
      it = schemas.emplace(message.schema->id, std::move(schema)).first;
    }
    DataTamerParser::SnapshotView snapshot;
    try
    {
      snapshot = DataTamerParser::SplitMcapMessage(
          { reinterpret_cast<const uint8_t*>(message.message.data),
            message.message.dataSize });
    }
    catch(const std::runtime_error& e)
    {
      throw error(topic + ": corrupt message: " + e.what());
    }
    snapshot.timestamp = message.message.logTime;
    snapshot.schema_hash = it->second.hash;
    callback(topic, it->second, snapshot);
    ++count;
  }
  if(!problem.empty())
  {
    throw error("can't read the MCAP file: " + problem);
  }
  return count;
}

/// ForEachSnapshot() that also decodes: calls `callback(const std::string& topic,
/// uint64_t timestamp, const std::string& name, const DataTamerParser::VarNumber&
/// value)` for each numeric value and `callback_string(const std::string& topic, uint64_t
/// timestamp, const std::string& name, std::string_view text)` for each "string" value
/// (a std::string in the writer) of each snapshot, in schema order (names as in
/// DataTamerParser::ParseSnapshot()). `text` is valid during the call only. A snapshot
/// that does not match its schema throws, after the callbacks have seen the values
/// decoded before the error. Returns the number of snapshots.
template <typename ValueCallback, typename StringCallback,
          typename = std::enable_if_t<
              std::is_invocable_v<const StringCallback&, const std::string&, uint64_t,
                                  const std::string&, std::string_view>>>
size_t ForEachValue(const std::string& path, const ValueCallback& callback,
                    const StringCallback& callback_string,
                    const std::vector<std::string>& topics = {})
{
  const auto decode = [&](const std::string& topic, const DataTamerParser::Schema& schema,
                          const DataTamerParser::SnapshotView& snapshot) {
    bool in_callback = false;  // what the callback throws is not a decoding error
    const auto on_value = [&](const std::string& name,
                              const DataTamerParser::VarNumber& value) {
      in_callback = true;
      callback(topic, snapshot.timestamp, name, value);
      in_callback = false;
    };
    bool matches = false;
    try
    {
      const auto on_string = [&](const std::string& name, std::string_view text) {
        in_callback = true;
        callback_string(topic, snapshot.timestamp, name, text);
        in_callback = false;
      };
      matches = DataTamerParser::ParseSnapshot(schema, snapshot, on_value, on_string);
    }
    catch(const std::runtime_error& e)
    {
      if(in_callback)
      {
        throw;
      }
      throw std::runtime_error(path + ": " + topic + ": corrupt message: " + e.what());
    }
    if(!matches)
    {
      throw std::runtime_error(path + ": " + topic +
                               ": corrupt message: the payload does not match the "
                               "schema");
    }
  };
  return ForEachSnapshot(path, decode, topics);
}

/// ForEachValue() without a string callback: "string" values are skipped.
template <typename ValueCallback>
size_t ForEachValue(const std::string& path, const ValueCallback& callback,
                    const std::vector<std::string>& topics = {})
{
  return ForEachValue(
      path, callback,
      [](const std::string&, uint64_t, const std::string&, std::string_view) {}, topics);
}

}  // namespace DataTamer2
