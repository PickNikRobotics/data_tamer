#pragma once

#include "data_tamer/types.hpp"

#include <mutex>
#include <typeindex>
#include <typeinfo>

#include "data_tamer/details/mutex.hpp"

namespace DataTamer {

class CustomSerializer
{
public:
  using Ptr = std::shared_ptr<CustomSerializer>;

  virtual ~CustomSerializer() = default;
  // name of the type, to be written in the schema string.
  virtual const char* typeName() const = 0;
  // optional custom schema of the type
  virtual const char* typeSchema() const { return nullptr; }
  // size in bytes of the serialized object.
  // Needed to pre-allocate memory in the buffer
  virtual uint32_t serializedSize(const void* src_instance) const = 0;
  // serialize an object into a buffer. Return the size in bytes of the serialized data
  virtual void serialize(const void* src_instance, SerializeMe::SpanBytes&) const = 0;
};


template <typename T>
class CustomSerializerT : public CustomSerializer
{
public:
  CustomSerializerT(const std::string& type_name): _name(type_name)
  {
    auto func = [this](const char* field_name, const auto& attr)
    {
      _schema += ToStr(GetBasicType<T>()) + " " + std::string(field_name);
    };
    SerializeMe::TypeDefinition<T>().typeDef(func);
  }

  const char* typeName() const override { return _name.c_str(); }

  const char* typeSchema() const override { return _schema.c_str(); }

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
  std::string _schema;
};

class TypesRegistry {
public:

  // global instance (similar to singleton)
  static TypesRegistry& Global()
  {
    static TypesRegistry global_object;
    return global_object;
  }

  template <typename T> CustomSerializer::Ptr addType(const std::string& type_name)
  {
    std::scoped_lock lk(_mutex);
    auto serializer = std::make_shared<CustomSerializerT<T>>(type_name);
    _types[typeid(T)] = serializer;
    return serializer;
  }

  template <typename T> [[nodiscard]] CustomSerializer::Ptr getSerializer()
  {
    std::scoped_lock lk(_mutex);
    auto it = _types.find(typeid(T));
    return it == _types.end() ? CustomSerializer::Ptr{} : it->second;
  }
private:

  std::unordered_map<std::type_index, CustomSerializer::Ptr> _types;
  Mutex _mutex;
};


}  // namespace DataTamer

