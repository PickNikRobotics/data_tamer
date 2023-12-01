#pragma once

#include <mutex>
#include <optional>

#include "data_tamer/types.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"

namespace DataTamer
{

template <class T>
struct TypeDefinition
{
  /// Provide the name of the type.
  ///  Implement this in your template specialization
  std::string typeName() const;

  /// Apply the function in the argument to each field
  /// The signature of the function addField has:
  ///  - a [const char*] as 1st argument
  ///  - [pointer to member] of T as 2nd argument
  template <class Function>
  void typeDef(const T& obj, Function& addField);
};

class CustomSerializer
{
public:
  using Ptr = std::shared_ptr<CustomSerializer>;

  virtual ~CustomSerializer() = default;
  // name of the type, to be written in the schema string.
  virtual const std::string& typeName() const = 0;

  // optional custom schema of the type
  virtual std::optional<CustomSchema> typeSchema() const { return std::nullopt; }
  // size in bytes of the serialized object.
  // Needed to pre-allocate memory in the buffer
  virtual size_t serializedSize(const void* instance) const = 0;

  // true if the method serializedSize will ALWAYS return the same value
  virtual bool isFixedSize() const = 0;

  // serialize an object into a buffer.
  virtual void serialize(const void* instance, SerializeMe::SpanBytes&) const = 0;
};

//------------------------------------------------------------------

// This derived class is used automatically by all the types
// that have a template specialization of TypeDefinition<T>
template <typename T>
class CustomSerializerT : public CustomSerializer
{
public:
  CustomSerializerT(const std::string& type_name = TypeDefinition<T>().typeName());

  const std::string& typeName() const override;

  size_t serializedSize(const void* src_instance) const override;

  bool isFixedSize() const override;

  void serialize(const void* src_instance,
                 SerializeMe::SpanBytes& dst_buffer) const override;

private:
  std::string _name;
  size_t _fixed_size = 0;
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
//------------------------------------------------------------------
//------------------------------------------------------------------

template <template <class, class> class Container, class T, class... TArgs>
struct TypeDefinition<Container<T, TArgs...>>
{
  std::string typeName() const { return TypeDefinition<T>().typeName(); }
};

template <typename T, size_t N>
struct TypeDefinition<std::array<T, N>>
{
  std::string typeName() const { return TypeDefinition<T>().typeName(); }
};

template <class C, typename T>
T getPointerType(T C::*v);

template <class T, typename = void>
struct has_typedef_with_object : std::false_type
{
};

inline void DummyAddField(const char*, const void*){}
using EmptyFunc = decltype(DummyAddField);

template <class T>
struct has_typedef_with_object<T,
                               decltype(TypeDefinition<T>().typeDef(
                                   std::declval<const T&>(), std::declval<EmptyFunc&>()))>
  : std::true_type
{
};

template <class T, typename = void>
struct has_typedef_with_member : std::false_type
{
};

template <class T>
struct has_typedef_with_member<T, decltype(TypeDefinition<T>().typeDef(
                                      std::declval<EmptyFunc&>()))> : std::true_type
{
};

// Recursive function to compute if a type has fixed size (at compile time).
// Used mainly by the CustomSerializerT constructor.
template <typename T>
inline void GetFixedSize(bool& is_fixed_size, size_t& fixed_size)
{
  if constexpr (IsNumericType<T>())
  {
    fixed_size += sizeof(T);
  }
  else
  {
    constexpr auto info = SerializeMe::container_info<T>();
    if constexpr (info.is_container && info.size == 0)
    {
      // vector
      is_fixed_size = false;
    }
    else if constexpr (info.is_container && info.size >= 0)
    {
      // array
      size_t obj_size;
      using Type = typename SerializeMe::container_info<T>::value_type;
      GetFixedSize<Type>(is_fixed_size, obj_size);
      fixed_size += info.size * obj_size;
    }
    else if (is_fixed_size)
    { 
      if constexpr (has_typedef_with_object<T>())
      {
        auto funcA = [&](const char*, auto const* member) {
          using MemberType = std::remove_cv_t<std::remove_reference_t<decltype(*member)>>;
          GetFixedSize<MemberType>(is_fixed_size, fixed_size);
        };
        TypeDefinition<T>().typeDef({}, funcA);
      }
      else
      {
        auto funcB = [&](const char*, auto const& member) {
          using MemberType = decltype(getPointerType(member));
          GetFixedSize<MemberType>(is_fixed_size, fixed_size);
        };
        TypeDefinition<T>().typeDef(funcB);
      }
    }
  }
}

template <typename T>
inline CustomSerializerT<T>::CustomSerializerT(const std::string& type_name) :
  _name(type_name)
{
  static_assert(!SerializeMe::container_info<T>().is_container, "Don't pass containers a "
                                                                "template type");

  bool is_fixed_size = true;
  GetFixedSize<T>(is_fixed_size, _fixed_size);
  if (!is_fixed_size)
  {
    _fixed_size = 0;
  }
}

template <typename T>
inline const std::string& CustomSerializerT<T>::typeName() const
{
  return _name;
}

template <typename T>
inline size_t CustomSerializerT<T>::serializedSize(const void* src_instance) const
{
  if (_fixed_size != 0)
  {
    return _fixed_size;
  }
  const auto* obj = static_cast<const T*>(src_instance);
  return SerializeMe::BufferSize(*obj);
}

template <typename T>
inline bool CustomSerializerT<T>::isFixedSize() const
{
  return _fixed_size > 0;
}

template <typename T>
inline void CustomSerializerT<T>::serialize(const void* src_instance,
                                            SerializeMe::SpanBytes& dst_buffer) const
{
  const auto* obj = static_cast<const T*>(src_instance);
  SerializeMe::SerializeIntoBuffer(dst_buffer, *obj);
}

template <typename T>
inline CustomSerializer::Ptr TypesRegistry::getSerializer()
{
  static_assert(!IsNumericType<T>(), "You don't need to create a serializer for a "
                                     "numerical type.");
  static_assert(!SerializeMe::container_info<T>().is_container, "Don't pass containers "
                                                                "as template type");

  std::scoped_lock lk(_mutex);
  const auto type_name = TypeDefinition<T>().typeName();
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
  static_assert(!IsNumericType<T>(), "You don't need to create a serializer for a "
                                     "numerical type.");
  static_assert(!SerializeMe::container_info<T>().is_container, "Don't pass containers "
                                                                "as template type");

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

// namespace wrapping
// Forward DataTamer::TypeDefinition to SerializeMe::TypeDefinition
namespace SerializeMe
{

template <class T>
inline std::string TypeDefinition<T>::typeName() const
{
  return DataTamer::TypeDefinition<T>().typeName();
}

template <class T>
template <class Function>
void TypeDefinition<T>::typeDef(const T& obj, Function& addField)
{
  if constexpr (DataTamer::has_typedef_with_object<T>())
  {
    DataTamer::TypeDefinition<T>().typeDef(obj, addField);
  }
  else
  {
    auto func = [&obj, &addField](const char* name, const auto& member) {
      addField(name, &(obj.*member));
    };
    DataTamer::TypeDefinition<T>().typeDef(func);
  }
}

}   // namespace SerializeMe
