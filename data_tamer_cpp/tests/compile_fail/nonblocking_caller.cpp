// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt), built
// with -Wfunction-effects -Werror by Clang 20 or later. A [[clang::nonblocking]] control
// cycle may call the real-time API, which is marked DATA_TAMER_NONBLOCKING: as written
// the file compiles. Each DT_* macro adds a call that is not real-time safe, which must
// then be an error.
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/sinks/mcap_ring_sink.hpp"

#include <vector>

namespace probe
{
using namespace DataTamer2;

// The default timestamp argument, NsecSinceEpoch(), is checked at the call too.
SnapshotResult
controlCycle(LogChannel& channel, LoggedValue<double>& value, const RegistrationID& id,
             SinkWorker& worker, MCAPRingSink& ring, [[maybe_unused]] double* extra,
             [[maybe_unused]] LoggedValue<std::vector<double>>* vector) noexcept
    [[clang::nonblocking]]
{
  value.set(value.get() + 1.0);
  value.setEnabled(value.isEnabled());
  (void)channel.trySetEnabled(id, channel.isEnabled(id));
  (void)channel.writeLockContended();
  (void)channel.writeLockWaitMaxNs();
  (void)channel.payloadReallocations();
  (void)channel.droppedOversize();
  (void)channel.serializeFailures();
  (void)channel.snapshotAttempts();
  (void)channel.snapshotsAccepted();
  (void)channel.poolExhausted();
  (void)worker.delivered();
  (void)worker.errors();
  (void)worker.queueHighWater();
  (void)ring.requestDump();
#ifdef DT_TAKE_SNAPSHOT
  (void)channel.takeSnapshot();
#endif
#ifdef DT_REGISTER_VALUE
  (void)channel.registerValue("extra", extra);
#endif
#ifdef DT_NON_SCALAR_SET
  vector->set({});
#endif
#ifdef DT_STATS
  (void)channel.stats();
#endif
  const auto result = channel.tryTakeSnapshot();
  (void)accepted(result);
  return result;
}
}  // namespace probe
