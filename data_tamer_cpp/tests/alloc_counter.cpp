#include "alloc_counter.hpp"

#include <cstdlib>
#include <new>

namespace DataTamerTest
{
thread_local bool AllocCounter::enabled = false;
thread_local std::size_t AllocCounter::allocations = 0;
thread_local std::size_t AllocCounter::deallocations = 0;
}  // namespace DataTamerTest

using DataTamerTest::AllocCounter;

namespace
{
void* countedAlloc(std::size_t size)
{
  if(AllocCounter::enabled)
  {
    ++AllocCounter::allocations;
  }
  if(size == 0)
  {
    size = 1;
  }
  void* p = std::malloc(size);
  if(p == nullptr)
  {
    throw std::bad_alloc();
  }
  return p;
}

void countedFree(void* p) noexcept
{
  if(p != nullptr && AllocCounter::enabled)
  {
    ++AllocCounter::deallocations;
  }
  std::free(p);
}
}  // namespace

void* operator new(std::size_t size) { return countedAlloc(size); }
void* operator new[](std::size_t size) { return countedAlloc(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  if(AllocCounter::enabled)
  {
    ++AllocCounter::allocations;
  }
  return std::malloc(size == 0 ? 1 : size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
  if(AllocCounter::enabled)
  {
    ++AllocCounter::allocations;
  }
  return std::malloc(size == 0 ? 1 : size);
}

void operator delete(void* p) noexcept { countedFree(p); }
void operator delete[](void* p) noexcept { countedFree(p); }
void operator delete(void* p, std::size_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::size_t) noexcept { countedFree(p); }
