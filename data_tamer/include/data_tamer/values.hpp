#pragma once

#include <functional>
#include <atomic>
#include "data_tamer/types.hpp"
#include <eigen3/Eigen/Core>

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
  ValuePtr(const T* pointer)
    : v_ptr_(pointer),
    type_(GetBasicType<T>()),
    memory_size_(sizeof(T)) {
    static_assert(GetBasicType<T>() != BasicType::OTHER,
                  "ValuePtr can only cast numbers");
  }

  ValuePtr(ValuePtr const& other) = delete;
  ValuePtr& operator=(ValuePtr const& other) = delete;

  ValuePtr(ValuePtr&& other) = default;
  ValuePtr& operator=(ValuePtr&& other) = default;

  size_t serialize(void* dest) const
  {
    // slightly faster than memcpy
    switch(memory_size_) {
      case 8: *static_cast<uint64_t*>(dest) = *static_cast<const uint64_t*>(v_ptr_);
        break;
      case 4: *static_cast<uint32_t*>(dest) = *static_cast<const uint32_t*>(v_ptr_);
        break;
      case 2: *static_cast<uint16_t*>(dest) = *static_cast<const uint16_t*>(v_ptr_);
        break;
      case 1: *static_cast<uint8_t*>(dest) = *static_cast<const uint8_t*>(v_ptr_);
        break;
      default:
        std::memcpy(dest, v_ptr_, memory_size_);
    }
    return memory_size_;
  }

  /// Get the type of the stored variable pointer
  [[nodiscard]] BasicType type() const { return type_; }

private:
  const void* v_ptr_ = nullptr;
  BasicType type_ = BasicType::OTHER;
  std::uint8_t memory_size_ = 0;
};



}  // namespace DataTamer
