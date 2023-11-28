#pragma once

#include <functional>
#include <atomic>
#include <cstring>
#include <iostream>
#include "data_tamer/custom_types.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"

namespace DataTamer
{

/**
 * @brief The ValuePtr is a non-owning pointer to
 * a variable.
 */
class ValuePtr
{
public:
  ValuePtr() = default;

  template <typename T>
  ValuePtr(const T* pointer, CustomSerializer::Ptr type_info = {});

  template <template <class, class> class Container, class T, class... TArgs>
  ValuePtr(const Container<T, TArgs...>* vect);

  template <template <class, class> class Container, class T, class... TArgs>
  ValuePtr(const Container<T, TArgs...>* vect, CustomSerializer::Ptr type_info);

  template <typename T, size_t N>
  ValuePtr(const std::array<T, N>* vect);

  template <typename T, size_t N>
  ValuePtr(const std::array<T, N>* vect, CustomSerializer::Ptr type_info);

  ValuePtr(ValuePtr const& other) = delete;
  ValuePtr& operator=(ValuePtr const& other) = delete;

  ValuePtr(ValuePtr&& other) = default;
  ValuePtr& operator=(ValuePtr&& other) = default;

  [[nodiscard]] bool operator==(const ValuePtr& other) const;

  [[nodiscard]] bool operator!=(const ValuePtr& other) const
  {
    return !(*this == other);
  }

  void serialize(SerializeMe::SpanBytes& dest) const;

  [[nodiscard]] size_t getSerializedSize() const;

  /// Get the type of the stored variable pointer
  [[nodiscard]] BasicType type() const
  {
    return type_;
  }

  [[nodiscard]] bool isVector() const
  {
    return is_vector_;
  }

  [[nodiscard]] uint16_t vectorSize() const
  {
    return array_size_;
  }

private:
  const void* v_ptr_ = nullptr;
  BasicType type_ = BasicType::OTHER;
  std::uint8_t memory_size_ = 0;
  std::function<void(SerializeMe::SpanBytes&)> serialize_impl_;
  std::function<size_t()> get_size_impl_;
  bool is_vector_ = false;
  uint16_t array_size_ = 0;
};

//------------------------------------------------------------
//------------------------------------------------------------
//------------------------------------------------------------

template <typename T>
inline ValuePtr::ValuePtr(const T* pointer, CustomSerializer::Ptr type_info) :
  v_ptr_(pointer), type_(GetBasicType<T>()), memory_size_(sizeof(T)), is_vector_(false)
{
  if (type_info)
  {
    serialize_impl_ = [type_info, pointer](SerializeMe::SpanBytes& buffer) -> void
    {
      type_info->serialize(pointer, buffer);
    };
    get_size_impl_ = [type_info, pointer]() -> size_t
    {
      return type_info->serializedSize(pointer);
    };
  }
}

template <template <class, class> class Container, class T, class... TArgs>
inline ValuePtr::ValuePtr(const Container<T, TArgs...>* vect) :
  v_ptr_(vect), type_(GetBasicType<T>()), memory_size_(sizeof(T)), is_vector_(true)
{
  serialize_impl_ = [vect](SerializeMe::SpanBytes& buffer) -> void {
    SerializeMe::SerializeIntoBuffer(buffer, *vect);
  };
  get_size_impl_ = [vect]() -> size_t {
    return SerializeMe::BufferSize(*vect);
  };
}

template <template <class, class> class Container, class T, class... TArgs>
inline ValuePtr::ValuePtr(const Container<T, TArgs...>* vect,
                          CustomSerializer::Ptr type_info) :
  v_ptr_(vect), type_(GetBasicType<T>()), memory_size_(sizeof(T)), is_vector_(true)
{
  serialize_impl_ = [type_info, vect](SerializeMe::SpanBytes& buffer) -> void
  {
    SerializeMe::SerializeIntoBuffer(buffer, uint32_t(vect->size()));
    for(const auto& value: (*vect))
    {
      type_info->serialize(&value, buffer);
    }
  };
  get_size_impl_ = [type_info, vect]() -> size_t {
    if(vect->empty())
    {
      return sizeof(uint32_t);
    }
    if(type_info->isFixedSize())
    {
      return sizeof(uint32_t) + vect->size() * type_info->serializedSize(&vect->front());
    }
    size_t tot_size = sizeof(uint32_t);
    for(const auto& value: (*vect))
    {
      tot_size += type_info->serializedSize(&value);
    }
    return tot_size;
  };
}

template <typename T, size_t N>
inline ValuePtr::ValuePtr(const std::array<T, N>* array) :
  v_ptr_(array),
  type_(GetBasicType<T>()),
  is_vector_(true),
  array_size_(N)
{
  serialize_impl_ = [array](SerializeMe::SpanBytes& buffer) -> void {
    SerializeMe::SerializeIntoBuffer(buffer, *array);
  };
  get_size_impl_ = [array]() { return SerializeMe::BufferSize(*array); };
}

template <typename T, size_t N>
inline ValuePtr::ValuePtr(const std::array<T, N>* array,
                          CustomSerializer::Ptr type_info) :
  v_ptr_(array),
  type_(GetBasicType<T>()),
  is_vector_(true),
  array_size_(N)
{
  serialize_impl_ = [type_info, array](SerializeMe::SpanBytes& buffer) -> void
  {
    for(const auto& value: (*array))
    {
      type_info->serialize(&value, buffer);
    }
  };
  get_size_impl_ = [type_info, array]() {
    size_t tot_size = 0;
    if(type_info->isFixedSize())
    {
      return N * type_info->serializedSize(&array->front());
    }
    for(const auto& value: (*array))
    {
      tot_size += type_info->serializedSize(&value);
    }
    return tot_size;
  };
}

inline bool ValuePtr::operator==(const ValuePtr& other) const
{
  return type_ == other.type_ && is_vector_ == other.is_vector_ &&
         array_size_ == other.array_size_;
}

inline void ValuePtr::serialize(SerializeMe::SpanBytes& dest) const
{
  if (serialize_impl_)
  {
    serialize_impl_(dest);
    return;
  }
  // slightly faster than memcpy
  switch (memory_size_)
  {
    case 8:
      *reinterpret_cast<uint64_t*>(dest.data()) = *static_cast<const uint64_t*>(v_ptr_);
      break;
    case 4:
      *reinterpret_cast<uint32_t*>(dest.data()) = *static_cast<const uint32_t*>(v_ptr_);
      break;
    case 2:
      *reinterpret_cast<uint16_t*>(dest.data()) = *static_cast<const uint16_t*>(v_ptr_);
      break;
    case 1:
      *(dest.data()) = *static_cast<const uint8_t*>(v_ptr_);
      break;
    default:
      std::memcpy(dest.data(), v_ptr_, memory_size_);
  }
  dest.trimFront(memory_size_);
}

inline size_t ValuePtr::getSerializedSize() const
{
  if (!get_size_impl_)
  {
    return memory_size_;
  }
  return get_size_impl_();
}

}   // namespace DataTamer
