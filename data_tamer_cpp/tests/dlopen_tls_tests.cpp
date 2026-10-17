// libdata_tamer loaded with dlopen(), by a plugin that links it: the first access of
// the transaction chain on a thread must not allocate. glibc allocates the dynamic TLS
// of a dlopen()ed library with malloc() on its first access on each thread; malloc is
// interposed here to count it. This program does not link libdata_tamer. glibc only.
#include <gtest/gtest.h>

#include <dlfcn.h>

#include <atomic>
#include <cstddef>

extern "C" void* __libc_malloc(size_t size);
extern "C" void* __libc_calloc(size_t count, size_t size);
extern "C" void* __libc_realloc(void* pointer, size_t size);

namespace
{
std::atomic<bool> counting{ false };
std::atomic<size_t> allocations{ 0 };

void countAllocation()
{
  if(counting.load())
  {
    allocations.fetch_add(1);
  }
}

void window(bool open)
{
  counting.store(open);
}
}  // namespace

extern "C" void* malloc(size_t size) noexcept
{
  countAllocation();
  return __libc_malloc(size);
}

extern "C" void* calloc(size_t count, size_t size) noexcept
{
  countAllocation();
  return __libc_calloc(count, size);
}

extern "C" void* realloc(void* pointer, size_t size) noexcept
{
  countAllocation();
  return __libc_realloc(pointer, size);
}

TEST(DlopenTls, FirstTransactionAccessOnAThreadDoesNotAllocate)
{
  // libdata_tamer must arrive with the plugin.
  ASSERT_EQ(dlopen(DATA_TAMER_SONAME, RTLD_NOW | RTLD_NOLOAD), nullptr);

  // dlopen() allocates through the same malloc as the TLS allocation: if it is not
  // counted, the interposition does not work and the test proves nothing.
  window(true);
  void* plugin = dlopen(DATA_TAMER_DLOPEN_TLS_PLUGIN, RTLD_NOW | RTLD_LOCAL);
  window(false);
  ASSERT_NE(plugin, nullptr) << dlerror();
  ASSERT_GT(allocations.exchange(0), 0u) << "malloc is not interposed";

  using Run = bool (*)(void (*)(bool));
  const char* symbol = "dataTamerFirstTransactionAccess";
  const auto run = reinterpret_cast<Run>(dlsym(plugin, symbol));
  ASSERT_NE(run, nullptr) << dlerror();
  EXPECT_TRUE(run(window)) << "the snapshot did not find the write mutex held";
  EXPECT_EQ(allocations.load(), 0u);
}
