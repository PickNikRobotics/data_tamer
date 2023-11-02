#include "data_tamer/channel.hpp"

namespace DataTamer
{

RegistrationID LogChannel::registerValueImpl(
    const std::string& name, ValuePtr&& value_ptr)
{
  if (name.find(' ') != std::string::npos)
  {
    throw std::runtime_error("name can not contain spaces");
  }

  std::lock_guard const lock(mutex_);
  active_flags_dirty_ = true;

  // check if this name exists already
  auto it = registered_values_.find(name);
  if( it == registered_values_.end())
  {
    // appending a new ValueHolder to series
    ValueHolder instance;
    instance.name = name;
    instance.holder = std::move(value_ptr);
    series_.emplace_back(std::move(instance));

    const size_t index = series_.size() -1;
    registered_values_.insert( {name, index} );
    max_buffer_size_ += SizeOf(value_ptr.type());

    return {index, 1};
  }

  // trying to registered again an unregistered holder
  const size_t index = it->second;
  auto& instance = series_[index];

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
  return {index, 1};
}


void LogChannel::setEnabled(const RegistrationID& id, bool enable)
{
  std::lock_guard const lock(mutex_);
  for(size_t i=0; i<id.fields_count; i++)
  {
    series_[id.first_index + i].enabled = enable;
  }
}

void LogChannel::unregister(const RegistrationID& id)
{
  std::lock_guard const lock(mutex_);
  for(size_t i=0; i<id.fields_count; i++)
  {
    auto& instance = series_[id.first_index + i];
    instance.registered = false;
    instance.enabled = false;
  }
}

void LogChannel::addDataSink(std::shared_ptr<DataSinkBase> sink)
{
  sinks_.insert(sink);
}

void LogChannel::updateFlags()
{
  if (active_flags_dirty_)
  {
    active_flags_.clear();
    active_flags_.resize((series_.size() + 7) / 8, 0xFF);  // ceiling
    for (size_t i = 0; i < series_.size(); i++)
    {
      auto const& instance = series_[i];
      if (!instance.enabled)
      {
        active_flags_[i >> 3] &= uint8_t(~(1 << (i % 8)));
      }
    }
    active_flags_dirty_ = false;
  }
}

Dictionary LogChannel::getDictionary() const
{
  std::lock_guard const lock(mutex_);
  Dictionary dict;
  dict.entries.reserve(series_.size());
  for(const auto& instance: series_)
  {
    dict.entries.emplace_back( Dictionary::Entry{instance.name, instance.holder.type()} );
  }
  return dict;
}


} // namespace DataTamer
