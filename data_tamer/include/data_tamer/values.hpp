#pragma once

#include <functional>
#include <atomic>
#include <cstring>
#include "data_tamer/types.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"

namespace DataTamer
{

/**
 * @brief The ValuePtr is a non-owning pointer to
 * a variable.
 */
class ValuePtr {
public:
  ValuePtr() = default;

  template <typename T>
  ValuePtr(const T* pointer);

  // accept any type of container (std::vector, std::deque, std::list)
  // but T must be a numerical value
  template <template <class, class> class Container, class T, class... TArgs>
  ValuePtr(const Container<T, TArgs...>* vect);

  // T must be a numerical value
  template <typename T, size_t N>
  ValuePtr(const std::array<T, N>* vect);

  ValuePtr(ValuePtr const& other) = delete;
  ValuePtr& operator=(ValuePtr const& other) = delete;

  ValuePtr(ValuePtr&& other) = default;
  ValuePtr& operator=(ValuePtr&& other) = default;

  bool operator==(const ValuePtr& other) const;

  bool operator!=(const ValuePtr& other) const
  {
    return !(*this == other);
  }

  void serialize(SerializeMe::SpanBytes& dest) const;

  /// Get the type of the stored variable pointer
  [[nodiscard]] BasicType type() const { return type_; }

  bool isVector() const {
    return is_vector_;
  }

  uint16_t vectorSize() const {
    return array_size_;
  }

private:
  const void* v_ptr_ = nullptr;
  BasicType type_ = BasicType::OTHER;
  std::uint8_t memory_size_ = 0;
  std::function<void(SerializeMe::SpanBytes&)> serialize_impl_;
  bool is_vector_ = false;
  uint16_t array_size_ = 0;
};

//------------------------------------------------------------
//------------------------------------------------------------
//------------------------------------------------------------

template<typename T> inline
ValuePtr::ValuePtr(const T *pointer)
  : v_ptr_(pointer),
  type_(GetBasicType<T>()),
  memory_size_(sizeof(T))
{
  static_assert(GetBasicType<T>() != BasicType::OTHER,
                "ValuePtr: can only accept numbers");
}

inline bool ValuePtr::operator==(const ValuePtr &other) const
{
  return type_ == other.type_ &&
         is_vector_ == other.is_vector_ &&
         array_size_ == other.array_size_;
}

inline void ValuePtr::serialize(SerializeMe::SpanBytes &dest) const
{
  if(is_vector_)
  {
    serialize_impl_(dest);
  }
  // slightly faster than memcpy
  switch(memory_size_) {
    case 8: *reinterpret_cast<uint64_t*>(dest.data()) = *static_cast<const uint64_t*>(v_ptr_);
      break;
    case 4: *reinterpret_cast<uint32_t*>(dest.data()) = *static_cast<const uint32_t*>(v_ptr_);
      break;
    case 2: *reinterpret_cast<uint16_t*>(dest.data()) = *static_cast<const uint16_t*>(v_ptr_);
      break;
    case 1: *(dest.data()) = *static_cast<const uint8_t*>(v_ptr_);
      break;
    default:
      std::memcpy(dest.data(), v_ptr_, memory_size_);
  }
  dest.trimFront(memory_size_);
}

template <template <class, class> class Container, class T, class... TArgs>
inline ValuePtr::ValuePtr(const Container<T, TArgs...>* vect)
  : v_ptr_(vect),
  type_(GetBasicType<T>()),
  memory_size_(sizeof(T)),
  is_vector_(true)
{
  static_assert(GetBasicType<T>() != BasicType::OTHER,
                "ValuePtr: can only accept vectors of numbers");

  serialize_impl_ = [this](SerializeMe::SpanBytes& buffer) -> void
  {
    auto const* vect = static_cast<const Container<T, TArgs...>*>(v_ptr_);
    SerializeMe::SerializeIntoBuffer(buffer, uint32_t(vect->size()));
    for(const auto& val: *vect)
    {
      SerializeMe::SerializeIntoBuffer(buffer, val);
    }
  };
}

template<typename T, size_t N> inline
ValuePtr::ValuePtr(const std::array<T, N> *vect)
  : v_ptr_(vect),
  type_(GetBasicType<T>()),
  memory_size_(sizeof(T)),
  is_vector_(true),
  array_size_(N)
{
  static_assert(GetBasicType<T>() != BasicType::OTHER,
                "ValuePtr: can only accept arrays of numbers");

  serialize_impl_ = [this](SerializeMe::SpanBytes& buffer) -> void
  {
    auto const* array = static_cast<const std::array<T, N>*>(v_ptr_);
    for(size_t i=0; i<N; i++) {
      SerializeMe::SerializeIntoBuffer(buffer, (*array)[i]);
    }
  };
}

}  // namespace DataTamer
