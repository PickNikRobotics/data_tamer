#pragma once

#include <map>
#include <mutex>
#include <typeindex>
#include <typeinfo>

#include "data_tamer/types.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"

namespace DataTamer
{

class CustomSerializer
{
public:
  using Ptr = std::shared_ptr<CustomSerializer>;

  virtual ~CustomSerializer() = default;
  // name of the type, to be written in the schema string.
  virtual const char* typeName() const = 0;
  // optional custom schema of the type
  virtual const char* typeSchema() const
  {
    return nullptr;
  }
  // size in bytes of the serialized object.
  // Needed to pre-allocate memory in the buffer
  virtual uint32_t serializedSize(const void* src_instance) const = 0;
  // serialize an object into a buffer. Return the size in bytes of the serialized data
  virtual void serialize(const void* src_instance, SerializeMe::SpanBytes&) const = 0;
};

class TypesRegistry
{
public:
  template <typename T>
  CustomSerializer::Ptr addType(const std::string& type_name,
                                bool skip_if_present = false);

  template <typename T>
  [[nodiscard]] CustomSerializer::Ptr getSerializer();

private:
  std::unordered_map<std::string, CustomSerializer::Ptr> _types;
  std::recursive_mutex _mutex;
};
//------------------------------------------------------------------
template <typename T>
class CustomSerializerT : public CustomSerializer
{
public:
  CustomSerializerT(const std::string& type_name) : _name(type_name)
  {}

  const char* typeName() const override
  {
    return _name.c_str();
  }

  uint32_t serializedSize(const void* src_instance) const override
  {
    const auto* obj = static_cast<const T*>(src_instance);
    return uint32_t(SerializeMe::BufferSize(*obj));
  }

  void serialize(const void* src_instance,
                 SerializeMe::SpanBytes& dst_buffer) const override
  {
    const auto* obj = static_cast<const T*>(src_instance);
    SerializeMe::SerializeIntoBuffer(dst_buffer, *obj);
  }

private:
  std::string _name;
};

//------------------------------------------------------------------

template <typename T>
inline CustomSerializer::Ptr TypesRegistry::getSerializer()
{
  static_assert(!IsNumericType<T>(), "You don't need to create a serializer for a "
                                     "numerical type. "
                                     "There might be an error in your code.");

  std::scoped_lock lk(_mutex);
  const auto type_name = SerializeMe::TypeDefinition<T>().typeName();
  auto it = _types.find(type_name);

  if (it == _types.end())
  {
    CustomSerializer::Ptr serializer = std::make_shared<CustomSerializerT<T>>(type_name);
    _types[type_name] = serializer;
    return serializer;
  }
  return it->second;
}

template <typename T>
inline CustomSerializer::Ptr TypesRegistry::addType(const std::string& type_name,
                                                    bool skip_if_present)
{
  std::scoped_lock lk(_mutex);
  if (skip_if_present && _types.count(type_name) != 0)
  {
    return {};
  }
  CustomSerializer::Ptr serializer = std::make_shared<CustomSerializerT<T>>(type_name);
  _types[type_name] = serializer;
  return serializer;
}

}   // namespace DataTamer
