# Joint-controller tutorials

Follow these in order. Each uses a small robot arm with shoulder and elbow joints,
with position, velocity and effort changing as sine and cosine waves. In containers,
indices 0 and 1 denote shoulder and elbow, respectively. The controller
samples at a nominal 1 kHz for about 1.5 seconds; the sleeps simulate scheduling and
do not configure a real-time operating-system thread. Files are written in the current
directory, and the `.mcap` files open in [PlotJuggler](https://github.com/facontidavide/PlotJuggler) 4 or later.

1. [T01_hello_mcap.cpp](T01_hello_mcap.cpp): register borrowed doubles, take snapshots, close an MCAP file.
2. [T02_custom_types.cpp](T02_custom_types.cpp): vectors, arrays, a local TypeDefinition and a vendor TypeDefinitionTrait.
3. [T03_channels_and_registry.cpp](T03_channels_and_registry.cpp): a 1 kHz controller and 10 Hz planner, shared sinks and toggles.
4. [T04_realtime_loop.cpp](T04_realtime_loop.cpp): prepare with startLogging, use tryTakeSnapshot, count failures and read stats outside the loop.
5. [T05_threads.cpp](T05_threads.cpp): a sensor thread, atomic LoggedValue updates and consistent scopedWrite transactions.
6. [T06_flight_recorder.cpp](T06_flight_recorder.cpp): keep recent history in MCAPRingSink and dump around a fault.
7. [T07_custom_sink.cpp](T07_custom_sink.cpp): decode snapshots with the standalone parser into a small CSV file.
8. [T08_mcap_reader.cpp](T08_mcap_reader.cpp): read T01's recording and print counts per time series.

## Build and run

From `data_tamer_cpp/`, every preset builds the tutorials (`debug`, `release`, `asan`,
`tsan`):

```bash
cmake --preset debug
cmake --build --preset debug -j 6
ctest --preset debug -R '^T0[1-8]_'
```

Run one from `build/debug/examples`, e.g. `./T01_hello_mcap`. T08 reads
`T01_joint_states.mcap` unless you pass another file.

Or configure explicitly, from the repository root:

```bash
cmake -S data_tamer_cpp -B build/tutorials -DDATA_TAMER_BUILD_ROS=OFF \
  -DDATA_TAMER_BUILD_EXAMPLES=ON
cmake --build build/tutorials -j 6
ctest --test-dir build/tutorials -R '^T0[1-8]_' --output-on-failure
```

With tests enabled (`DATA_TAMER_BUILD_TESTS`, and `BUILD_TESTING` under ROS), each
tutorial is a CTest case. CTest writes files in the build's `examples/` directory;
its fixture runs T01 before T08, even when selecting only T08. For an offline build,
add `-DFETCHCONTENT_SOURCE_DIR_DATA_TAMER_MCAP=<unpacked-mcap-release>` at configure time.

T01–T05 write `TNN_joint_states.mcap`; T06 writes numbered `fault_*.mcap` dumps;
T07 writes `T07_joint_states.csv`; T08 prints sample counts and creates no file.
Re-running overwrites the regular recordings and CSV; flight-recorder dumps get new numbers.

## Separate demos

- [mcap_1m_per_sec.cpp](mcap_1m_per_sec.cpp): four producers log for ten seconds and report snapshot latency.
- [ros2_publisher.cpp](ros2_publisher.cpp): a ROS 2 node with a 200 Hz wall timer; run
  `ros2 run data_tamer_v2 ros2_publisher [--aggregate] [--yaml]`. See also
  [python/ros2_subscriber.py](../../python/ros2_subscriber.py). It spins until shutdown.

These longer-running demos are built with the examples but are not CTest cases.
