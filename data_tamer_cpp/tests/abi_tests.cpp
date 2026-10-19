#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/custom_types.hpp"
#include "data_tamer_v2/data_sink.hpp"
#include "data_tamer_v2/data_tamer.hpp"
#include "data_tamer_v2/data_tamer_parser.hpp"
#include "data_tamer_v2/details/locked_reference.hpp"
#include "data_tamer_v2/logged_value.hpp"
#include "data_tamer_v2/sinks/mcap_ring_sink.hpp"
#include "data_tamer_v2/sinks/mcap_sink.hpp"
#include "data_tamer_v2/types.hpp"
#include "data_tamer_v2/values.hpp"

#ifdef USING_ROS2
#include "data_tamer_v2/sinks/ros2_publisher_sink.hpp"
#endif

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>

using namespace DataTamer2;

TEST(ABI, SinksAreOnlyOnePointerLargerThanTheInterface)
{
  static_assert(sizeof(MCAPSink) == sizeof(DataSink) + sizeof(std::unique_ptr<int>), "MCA"
                                                                                     "PSi"
                                                                                     "nk "
                                                                                     "gre"
                                                                                     "w "
                                                                                     "a "
                                                                                     "mem"
                                                                                     "ber"
                                                                                     " ou"
                                                                                     "tsi"
                                                                                     "de "
                                                                                     "its"
                                                                                     " Pi"
                                                                                     "mp"
                                                                                     "l");
#ifdef USING_ROS2
  static_assert(
      sizeof(ROS2PublisherSink) == sizeof(DataSink) + sizeof(std::unique_ptr<int>), "ROS2"
                                                                                    "Publ"
                                                                                    "ishe"
                                                                                    "rSin"
                                                                                    "k "
                                                                                    "grew"
                                                                                    " a "
                                                                                    "memb"
                                                                                    "er "
                                                                                    "outs"
                                                                                    "ide "
                                                                                    "its "
                                                                                    "Pimp"
                                                                                    "l");
#endif
  // The sink and the per-channel queues live in the Pimpl: one pointer.
  static_assert(sizeof(SinkWorker) == sizeof(std::unique_ptr<int>), "SinkWorker grew a "
                                                                    "member outside its "
                                                                    "Pimpl");
  // The typed handle of the factories adds no data member and no virtual function.
  static_assert(sizeof(SinkWorkerOf<MCAPSink>) == sizeof(SinkWorker));
  static_assert(alignof(SinkWorkerOf<MCAPSink>) == alignof(SinkWorker));
  static_assert(!std::is_polymorphic_v<SinkWorkerOf<MCAPSink>>);
  // The types registry lives in the Pimpl too.
  static_assert(sizeof(LogChannel) == sizeof(std::enable_shared_from_this<LogChannel>) +
                                          sizeof(std::unique_ptr<int>),
                "LogChannel grew a member outside its Pimpl");
  static_assert(sizeof(SnapshotRef) == sizeof(std::shared_ptr<int>) + sizeof(void*), "Sna"
                                                                                     "psh"
                                                                                     "otR"
                                                                                     "ef "
                                                                                     "lay"
                                                                                     "out"
                                                                                     " is"
                                                                                     " pa"
                                                                                     "rt "
                                                                                     "of "
                                                                                     "the"
                                                                                     " AB"
                                                                                     "I");
}

// DataSink's vtable and layout are frozen for 2.x (see data_sink.hpp): no data
// members. ChannelDefaults crosses the boundary by value: a layout change needs
// a SONAME bump.
TEST(ABI, DataSinkHasNoDataAndChannelDefaultsIsPinned)
{
  static_assert(sizeof(DataSink) == sizeof(void*), "DataSink gained a data member");
  static_assert(sizeof(ChannelDefaults) ==
                    2 * sizeof(size_t) + 2 * sizeof(std::chrono::nanoseconds),
                "ChannelDefaults layout is part of the ABI");
}

// The tests above pin the Pimpl relations (LogChannel, SinkWorker, the sinks,
// SnapshotRef), DataSink and the size of ChannelDefaults. This file is the first line
// of defence of the binary interface for the rest. Every value below is part of what
// a consumer compiles into its own binaries (a size, an alignment, a field offset,
// the width of an enum), so a change to one of them makes a newer libdata_tamer.so
// misbehave with consumers built against an older header. When a pin fails, do not
// just update the number: the change is an ABI break, which needs a new SOVERSION
// (the major version, see "Versioning and ABI policy" in CLAUDE.md) and a CHANGELOG
// entry. The libabigail job (tools/abi_check.sh) checks what sizes cannot: vtable
// slots, member types, removed symbols. Pinning Snapshot, MCAPRingDump and Schema
// completely, although appending a field would be safe for readers, is deliberate.

#define DT_ABI_BREAK " changed: ABI break, bump SOVERSION"
// A failing condition is printed in the compiler error.
#define DT_ABI_REQUIRE(...)                                                              \
  static_assert(__VA_ARGS__, "ABI break, bump SOVERSION: " #__VA_ARGS__)

// Exact byte values are pinned for x86_64 only: Linux, glibc, libstdc++ with the dual
// ABI on and no debug containers (the CI platform). Other platforms lay std::string,
// std::vector and friends out differently, so the same pins would fail to compile
// there. ChannelSharedState, its WriteMutex and its flag table are library-private
// (consumers see an incomplete type), so nothing pinned here contains a mutex.
#if defined(__GLIBCXX__) && defined(__linux__) && !defined(_GLIBCXX_DEBUG) &&            \
    _GLIBCXX_USE_CXX11_ABI == 1
#define DT_ABI_GLIBCXX 1
#else
#define DT_ABI_GLIBCXX 0
#endif
#if DT_ABI_GLIBCXX && defined(__x86_64__)
#define DT_ABI_PINS 1
#else
#define DT_ABI_PINS 0
#endif

#if DT_ABI_PINS

#define DT_PIN(Type, bytes, align)                                                       \
  static_assert(sizeof(Type) == (bytes), #Type " size" DT_ABI_BREAK);                    \
  static_assert(alignof(Type) == (align), #Type " alignment" DT_ABI_BREAK)

#define DT_PIN_OFFSET(Type, member, bytes)                                               \
  static_assert(offsetof(Type, member) == (bytes), #Type "::" #member DT_ABI_BREAK)

// offsetof on a class with private members or default member initializers is only
// conditionally supported; every compiler used here accepts it.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

// ---- Classes whose state sits behind a Pimpl (the others are pinned above) ----
DT_PIN(ChannelsRegistry, 8, 8);
DT_PIN(TypesRegistry, 8, 8);
DT_PIN(MCAPRingSink, 24, 8);  // vptr, shared_ptr<Pimpl>

// ---- Polymorphic interfaces: a vptr and nothing else. Their vtables (slot order)
// are frozen for 2.x and checked by abidiff. ----
DT_PIN(CustomSerializer, 8, 8);
DT_PIN(CustomSerializerT<int>, 48, 8);  // _name, _fixed_size; independent of T

// ---- Structs passed or returned by value across the library boundary. The Stats
// structs (LogChannel, SinkWorker, MCAPRingStats) are built inline from exported
// getters and never cross it: they are not pinned and may grow. ----
DT_PIN_OFFSET(ChannelDefaults, pool_capacity, 0);
DT_PIN_OFFSET(ChannelDefaults, pool_stall_tolerance, 8);
DT_PIN_OFFSET(ChannelDefaults, pool_snapshot_period, 16);
DT_PIN_OFFSET(ChannelDefaults, payload_capacity, 24);
DT_PIN(MCAPRingOptions, 56, 8);
DT_PIN_OFFSET(MCAPRingOptions, filepath, 0);
DT_PIN_OFFSET(MCAPRingOptions, window, 32);
DT_PIN_OFFSET(MCAPRingOptions, capacity_bytes, 40);
DT_PIN_OFFSET(MCAPRingOptions, compression, 48);
DT_PIN(MCAPRingDump, 120, 8);
DT_PIN(Snapshot, 64, 8);
DT_PIN(RegistrationID, 8, 4);
DT_PIN(TypeField, 80, 8);
DT_PIN(CustomSchema, 64, 8);
DT_PIN(Schema, 160, 8);
DT_PIN(VarNumber, 16, 8);
DT_PIN(SerializeMe::Span<uint8_t>, 16, 8);

#ifdef USING_ROS2
// The size of rclcpp::QoS differs between ROS distributions, so the options are
// pinned as the offsets of the members the header declares plus "data_qos is last
// and nothing follows it".
DT_PIN_OFFSET(ROS2PublisherOptions, aggregate, 0);
DT_PIN_OFFSET(ROS2PublisherOptions, max_batch_size, 8);
DT_PIN_OFFSET(ROS2PublisherOptions, max_batch_delay, 16);
DT_PIN_OFFSET(ROS2PublisherOptions, embed_schemas, 24);
DT_PIN_OFFSET(ROS2PublisherOptions, schema_format, 28);
DT_PIN_OFFSET(ROS2PublisherOptions, data_qos, 32);
DT_ABI_REQUIRE(alignof(rclcpp::QoS) <= 8 && alignof(ROS2PublisherOptions) == 8 &&
               sizeof(ROS2PublisherOptions) == 32 + sizeof(rclcpp::QoS));
#endif

// ---- Layouts compiled into consumers through inline code ----
DT_PIN(ValuePtr, 56, 8);
DT_PIN(LoggedValue<double>, 48, 8);  // state_, channel_, value_, id_
DT_PIN(LoggedValue<int>, 48, 8);
DT_PIN(WriteTransaction, 24, 8);  // state_, previous_, owns_
DT_PIN(ConstPtr<double>, 32, 8);
DT_PIN(MutablePtr<double>, 32, 8);

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#endif  // DT_ABI_PINS

// ---- Independent of the platform: widths of enums and signatures of the function
// pointers that cross the boundary. ----
DT_ABI_REQUIRE((std::is_same_v<std::underlying_type_t<BasicType>, uint8_t>));
DT_ABI_REQUIRE((std::is_same_v<std::underlying_type_t<SnapshotResult>, uint8_t>));
DT_ABI_REQUIRE((std::is_same_v<std::underlying_type_t<SinkWorker::Delivery>, uint8_t>));
DT_ABI_REQUIRE((std::is_same_v<std::underlying_type_t<SchemaFormat>, int>));
DT_ABI_REQUIRE(
    (std::is_same_v<ValuePtr::SerializeFn, void (*)(const void*, const CustomSerializer*,
                                                    SerializeMe::SpanBytes&)>));
DT_ABI_REQUIRE(
    (std::is_same_v<ValuePtr::SizeFn, size_t (*)(const void*, const CustomSerializer*)>));

TEST(ABI, LayoutPinsApplyToThisPlatform)
{
  // The pins are compile-time checks. This case only reports platforms where the
  // byte values are not enforced, so that a skipped check is visible in the log.
#if !DT_ABI_PINS
  GTEST_SKIP() << "byte-exact layout pins are for x86_64 Linux with libstdc++";
#endif
}

// The library's own namespace is DataTamer2, but SerializeMe and the parser keep their
// 1.x names inside the inline namespace v2, so that their mangled names differ from
// those of data_tamer 1.x, which can be loaded into the same process (CLAUDE.md,
// "Invariants"). One entity per namespace block: a block that loses its inline namespace
// fails here. Itanium mangling: a type in it is "N11SerializeMe2v2...".
TEST(ABI, SerializeMeAndParserAreInTheInlineNamespaceV2)
{
  const std::pair<const char*, const char*> names[] = {
    { typeid(SerializeMe::SpanBytes).name(), "N11SerializeMe2v2" },
    { typeid(SerializeMe::has_TypeDefinitionTrait<int>).name(), "N11SerializeMe2v2" },
    { typeid(DataTamerParser::Schema).name(), "N15DataTamerParser2v2" },
  };
  for(const auto& [name, prefix] : names)
  {
    EXPECT_NE(std::string(name).find(prefix), std::string::npos) << name;
  }
}
