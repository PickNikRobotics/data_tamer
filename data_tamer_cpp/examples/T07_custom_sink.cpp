// T07 A custom CSV sink
// You will learn: decode snapshots in DataSink callbacks delivered by a SinkWorker.
// Run: ./T07_custom_sink then inspect T07_joint_states.csv (timestamp, name, value)
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/data_sink.hpp"
#include "data_tamer_v2/decode.hpp"
#include "data_tamer_v2/data_tamer_parser.hpp"
#include "data_tamer_v2/types.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <ios>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>

class CsvSink : public DataTamer2::DataSink
{
public:
  explicit CsvSink(const std::string& path) : output_(path)
  {
    output_.exceptions(std::ios::failbit | std::ios::badbit);
    output_.precision(17);
    output_ << "timestamp_ns,channel,series,value\n";
  }

protected:
  void onSchema(const DataTamer2::Schema& schema) override
  {
    // Translate the public schema once; the standalone parser needs no library state.
    schemas_.add(schema.hash, DataTamer2::ToStr(schema));
  }

  void onSnapshot(const DataTamer2::SnapshotRef& snapshot) override
  {
    const DataTamerParser::Schema* schema = schemas_.find(snapshot->schema_hash);
    if(schema == nullptr)
    {
      throw std::runtime_error("CSV snapshot of an unknown schema");
    }
    const auto write_number = [&](const std::string& name,
                                  const DataTamerParser::VarNumber& value) {
      output_ << snapshot->timestamp.count() << ',' << schema->channel_name << ',' << name
              << ',';
      std::visit([&](auto number) { output_ << number; }, value);
      output_ << '\n';
    };
    if(!DataTamerParser::ParseSnapshot(*schema, ToSnapshotView(*snapshot), write_number))
    {
      throw std::runtime_error("CSV snapshot does not match its schema");
    }
    // Return promptly: retaining a SnapshotRef holds a pool slot for ALL sinks.
    // These few numeric fields use buffered output, with no per-row flush. For slow
    // storage, copy *snapshot and hand the copy to a separate I/O thread instead.
  }

  void onStop() override { output_.close(); }
  // This one-shot tutorial never restarts the worker. A reusable file sink would
  // also override onStart() to reopen its file after stop().

private:
  // SinkWorker serializes callbacks: these members need no additional mutex.
  std::ofstream output_;
  DataTamerParser::SchemaRegistry schemas_;
};

int main()
{
  std::array<double, 2> positions{}, velocities{}, efforts{};
  auto csv = DataTamer2::SinkWorker::create<CsvSink>("T07_joint_states.csv");
  auto channel = DataTamer2::LogChannel::create("controller");
  channel->addDataSink(csv);
  channel->registerValue("positions", &positions);
  channel->registerValue("velocities", &velocities);
  channel->registerValue("efforts", &efforts);
  for(int cycle = 0; cycle < 1500; ++cycle)
  {
    const double t = cycle * 0.001;
    for(size_t joint = 0; joint < positions.size(); ++joint)
    {
      const double phase = 4 * t + 0.5 * static_cast<double>(joint);
      positions[joint] = std::sin(phase);
      velocities[joint] = 4 * std::cos(phase);
      efforts[joint] = 0.2 * std::sin(phase);
    }
    channel->takeSnapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  csv->stop();
  std::cout << "CSV sink errors=" << csv->errors() << '\n';
  return csv->errors() == 0 ? 0 : 1;
}
