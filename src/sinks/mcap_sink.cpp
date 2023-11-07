#include "data_tamer/sinks/mcap_sink.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"

#define MCAP_IMPLEMENTATION
#include <mcap/writer.hpp>

namespace DataTamer {

static constexpr char const* kDataTamer = "data_tamer";

MCAPSink::MCAPSink(boost::filesystem::path const& path)
    : filepath_(path.string()) {
  openFile(filepath_);
}

void DataTamer::MCAPSink::openFile(std::string const& filepath) {
  writer_ = std::make_unique<mcap::McapWriter>();
  mcap::McapWriterOptions options(kDataTamer);
  options.compression = mcap::Compression::Zstd;
  auto status = writer_->open(filepath, options);
  if (!status.ok()) {
    throw std::runtime_error("Failed to open MCAP file for writing");
  }
  start_time_ = std::chrono::system_clock::now();
  // clean up, in case this was opened a second time
  hash_to_channel_id_.clear();
}

MCAPSink::~MCAPSink() {
  writer_->close();
}

void MCAPSink::addChannel(std::string const& channel_name,
                          Schema const& schema) {
  schemas_[channel_name] = schema;
  auto it = hash_to_channel_id_.find(schema.hash);
  if (it != hash_to_channel_id_.end()) {
    return;
  }

  // write the schema, one entry per line
  std::string schema_str;
  for (auto const& entry : schema.fields) {
    schema_str += entry.name + " " + ToStr(entry.type) + "\n";
  }

  auto const schema_name =
      channel_name + "::" + std::to_string(schema.hash);

  // Register a Schema
  mcap::Schema mcap_schema(schema_name, kDataTamer, schema_str);
  writer_->addSchema(mcap_schema);

  // Register a Channel
  mcap::Channel publisher(channel_name, kDataTamer, mcap_schema.id);
  writer_->addChannel(publisher);

  hash_to_channel_id_[schema.hash] = publisher.id;
}

bool MCAPSink::storeSnapshot(const std::vector<uint8_t>& snapshot) {

  SerializeMe::SpanBytesConst ptr(snapshot.data(), snapshot.size());
  int64_t hash;
  int64_t timestamp;
  SerializeMe::DeserializeFromBuffer(ptr, hash);
  SerializeMe::DeserializeFromBuffer(ptr, timestamp);

  // the payload must contain both the ActiveMask andthe other data

  // Write our message
  mcap::Message msg;
  msg.channelId = hash_to_channel_id_.at(hash);
  msg.sequence = 1;  // Optional
  // Timestamp requires nanosecond
  msg.logTime = mcap::Timestamp(timestamp);
  msg.publishTime = msg.logTime;
  msg.data = reinterpret_cast<std::byte const*>(ptr.data());  // NOLINT
  msg.dataSize = ptr.size();
  auto status = writer_->write(msg);

  // If reset_time_ is exceeded, we want to overwrite the current file.
  // Better than filling the disk, if you forgot to stop the application.
  auto const now = std::chrono::system_clock::now();
  if (now - start_time_ > reset_time_) {
    // close current file
    writer_->close();
    // reopen the same path (overwrite)
    openFile(filepath_);

    // rebuild the channels
    for (auto const& [name, schema] : schemas_) {
      addChannel(name, schema);
    }
  }
  return true;
}

void MCAPSink::flush() {
  writer_->closeLastChunk();
}

void MCAPSink::setMaxTimeBeforeReset(std::chrono::seconds reset_time) {
  reset_time_ = reset_time;
}

}  // namespace DataTamer
