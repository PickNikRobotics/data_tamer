#include "data_tamer/values.hpp"

namespace DataTamer
{

template <typename T> inline
void DummyMemcpy(void* dst, void const* src)
{
  *reinterpret_cast<T*>(dst) = *reinterpret_cast<T const*>(src);
}

size_t ValuePtr::serialize(void *dest) const
{
  // Faster than memcpy in benchmarks
  switch (memory_size_) {
    case 8: { DummyMemcpy<uint64_t>(dest, v_ptr_); } break;
    case 4: { DummyMemcpy<uint32_t>(dest, v_ptr_); } break;
    case 2: { DummyMemcpy<uint16_t>(dest, v_ptr_); } break;
    case 1: { DummyMemcpy<uint8_t>(dest, v_ptr_); } break;
    default: break;
  }
  return memory_size_;
}




}  // namespace DataTamer
