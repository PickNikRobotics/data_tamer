// Uses an installed data_tamer: a channel logging into an MCAPSink, the MCAP
// encoding helpers and the MCAP reader with the MCAP headers that come with the library,
// whose implementation is in libdata_tamer.
#include "data_tamer_v2/data_tamer.hpp"
#include "data_tamer_v2/mcap_reader.hpp"
#include "data_tamer_v2/sinks/mcap_encoding.hpp"
#include "data_tamer_v2/sinks/mcap_sink.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

int main()
{
  using namespace DataTamer2;
  const auto dir = std::filesystem::temp_directory_path();
  const std::string sink_path = (dir / "data_tamer_test_package_sink.mcap").string();
  const std::string writer_path = (dir / "data_tamer_test_package_writer.mcap").string();

  ChannelsRegistry registry;
  auto channel = registry.getChannel("test_package");
  double value = 42;
  channel->registerValue("value", &value);
  if(!channel->addDataSink(MCAPSink::create(sink_path)))
  {
    return 1;
  }
  channel->takeSnapshot();
  registry.stopAll();

  mcap::McapWriter writer;
  if(!writer.open(writer_path, mcap::McapWriterOptions(mcap_encoding::kEncoding)).ok())
  {
    return 1;
  }
  const auto id = mcap_encoding::AddChannel(writer, channel->getSchema());
  writer.close();

  const size_t read = ForEachValue(sink_path, [](const auto&...) {});
  const bool written = read == 1 && std::filesystem::file_size(sink_path) > 0 &&
                       std::filesystem::file_size(writer_path) > 0;
  std::remove(sink_path.c_str());
  std::remove(writer_path.c_str());
  std::cout << "data_tamer " << DATA_TAMER_VERSION << ", MCAP " << MCAP_LIBRARY_VERSION
            << ", channel id " << id << (written ? ": OK" : ": no file written")
            << std::endl;
  return written ? 0 : 1;
}
