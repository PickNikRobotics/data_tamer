#pragma once

#include "data_tamer_v2/data_sink.hpp"
#include "data_tamer_v2/data_tamer_parser.hpp"

#include <cstdint>

namespace DataTamer2
{

/// View on a library Snapshot for DataTamerParser::ParseSnapshot(), for instance in
/// DataSink::onSnapshot(): `ToSnapshotView(*snapshot_ref)`, found by argument-dependent
/// lookup. The snapshot must outlive the view. The timestamp is the nanosecond count,
/// as uint64_t.
inline DataTamerParser::SnapshotView ToSnapshotView(const Snapshot& snapshot)
{
  DataTamerParser::SnapshotView view;
  view.schema_hash = snapshot.schema_hash;
  view.timestamp = static_cast<uint64_t>(snapshot.timestamp.count());
  view.active_mask = { snapshot.active_mask.data(), snapshot.active_mask.size() };
  view.payload = { snapshot.payload.data(), snapshot.payload.size() };
  return view;
}

}  // namespace DataTamer2
