#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"

#include <iostream>

namespace DataTamer
{

struct LogChannel::Pimpl
{
  struct ValueHolder {
    std::string name;
    bool enabled = true;
    bool registered = true;
    ValuePtr holder;
  };

  std::string channel_name;

  mutable std::mutex mutex;

  size_t payload_max_buffer_size = 0;

  std::vector<ValueHolder> series;
  std::unordered_map<std::string, size_t> registered_values;

  std::atomic_bool mask_dirty = true;

  Snapshot snapshot;
  Schema schema;
  bool logging_started = false;

  std::unordered_set<std::shared_ptr<DataSinkBase>> sinks;
};

RegistrationID LogChannel::registerValueImpl(
    const std::string& name, ValuePtr&& value_ptr)
{
  if (name.find(' ') != std::string::npos)
  {
    throw std::runtime_error("name can not contain spaces");
  }

  std::lock_guard const lock(_p->mutex);
  _p->mask_dirty = true;

  // check if this name exists already
  auto it = _p->registered_values.find(name);
  if( it == _p->registered_values.end())
  {
    if(_p->logging_started)
    {
      throw std::runtime_error("Can't add new value to the Schema once recording started, "
                               "i.e. after takeShapshot was called the first time");
    }
    // appending a new ValueHolder to series
    Pimpl::ValueHolder instance;
    instance.name = name;
    instance.holder = std::move(value_ptr);
    _p->series.emplace_back(std::move(instance));
    const auto type = value_ptr.type();

    const size_t index = _p->series.size() -1;
    _p->registered_values.insert( {name, index} );
    _p->payload_max_buffer_size += SizeOf(type);

    // update schema and its hash (append only)
    // https://stackoverflow.com/questions/2590677/how-do-i-combine-hash-values-in-c0x
    std::hash<std::string> str_hasher;
    std::hash<BasicType> type_hasher;

    _p->schema.fields.emplace_back( Schema::Field{name, type} );

    auto& hash = _p->schema.hash;
    hash ^= str_hasher(name) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= type_hasher(type) + 0x9e3779b9 + (hash << 6) + (hash >> 2);

    return {index, 1};
  }

  // trying to registered again an unregistered holder
  const size_t index = it->second;
  auto& instance = _p->series[index];

  if(instance.registered)
  {
    throw std::runtime_error("Value already registered");
  }
  const auto old_type = instance.holder.type();
  const auto new_type = value_ptr.type();
  if(new_type != old_type)
  {
    throw std::runtime_error("Can't change the type of a previously "
                             "unregistered value");
  }
  // mark as registered again
  instance.registered = true;
  instance.enabled = true;
  _p->payload_max_buffer_size += SizeOf(new_type);
  return {index, 1};
}

LogChannel::LogChannel(std::string name) : _p(new Pimpl)
{
  _p->channel_name = std::move(name);
  std::hash<std::string> str_hasher;
  _p->schema.hash = str_hasher(_p->channel_name);
}

const std::string &LogChannel::channelName() const
{
  return _p->channel_name;
}

LogChannel::~LogChannel()
{
}

void LogChannel::setEnabled(const RegistrationID& id, bool enable)
{
  std::lock_guard const lock(_p->mutex);
  for(size_t i=0; i<id.fields_count; i++)
  {
    auto& instance = _p->series[id.first_index + i];
    if(instance.enabled != enable)
    {
      instance.enabled = enable;
      // if changed, adjust the size of the payload
      const auto size = SizeOf(instance.holder.type());
      _p->payload_max_buffer_size += enable ? size : -size;
    }
  }
}

void LogChannel::unregister(const RegistrationID& id)
{
  std::lock_guard const lock(_p->mutex);
  for(size_t i=0; i<id.fields_count; i++)
  {
    auto& instance = _p->series[id.first_index + i];
    instance.registered = false;
    instance.enabled = false;
    _p->payload_max_buffer_size -= SizeOf(instance.holder.type());
  }
}

void LogChannel::addDataSink(std::shared_ptr<DataSinkBase> sink)
{
  _p->sinks.insert(sink);
}

Schema LogChannel::getSchema() const
{
  std::lock_guard const lock(_p->mutex);
  return _p->schema;
}

std::mutex &LogChannel::writeMutex() { return _p->mutex; }

bool LogChannel::takeSnapshot(std::chrono::nanoseconds timestamp)
{
  std::lock_guard const lock(_p->mutex);

  if(_p->sinks.empty())
  {
    return false;
  }
  // update the _p->snapshot.active_mask if necessary
  if(_p->mask_dirty)
  {
    _p->mask_dirty = false;
    auto& mask = _p->snapshot.active_mask;
    mask.clear();
    const auto vect_size = (_p->series.size() + 7) / 8; // ceiling size
    mask.resize(vect_size, 0xFF);
    for (size_t i = 0; i < _p->series.size(); i++)
    {
      auto const& instance = _p->series[i];
      if (!instance.enabled)
      {
        SetBit(mask, i, false);
      }
    }
  }
  // call sink->addChannel (usually done once)
  if( !_p->logging_started )
  {
    _p->logging_started = true;
    _p->snapshot.schema_hash = _p->schema.hash;
    for(auto const& sink: _p->sinks)
    {
      sink->addChannel(_p->channel_name, _p->schema);
    }
  }

  _p->snapshot.timestamp = timestamp;
  // serialize data into _p->snapshot.payload
  _p->snapshot.payload.resize(_p->payload_max_buffer_size);

  auto* payload_ptr = _p->snapshot.payload.data();
  size_t actual_payload_size = 0;

  for(auto const& entry: _p->series)
  {
    if(entry.enabled)
    {
      const auto entry_size = entry.holder.serialize(payload_ptr);
      actual_payload_size += entry_size;
      payload_ptr += entry_size;
    }
  }
  _p->snapshot.payload.resize(actual_payload_size);

  for(auto& sink: _p->sinks)
  {
    sink->pushSnapshot(_p->snapshot);
  }
  return true;
}


} // namespace DataTamer
