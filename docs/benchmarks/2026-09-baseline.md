# Performance baseline — before the lock-free front end

Recorded on the unmodified library at commit `a851887`.

Machine: `Model name:                              Intel(R) Core(TM) Ultra 7 265H`, `16` cores, kernel `6.17.0-1032-oem`,
GCC `g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`, build preset `release` (`-O3`, no sanitizers).

How to reproduce: see the commands in
`docs/superpowers/plans/2026-09-10-lockfree-frontend-plan-1.md`, Task 5, Step 5.

## Micro-benchmarks (`dt_benchmark`)

```
2026-09-10T15:13:03+02:00
Running ./build/release/benchmarks/dt_benchmark
Run on (16 X 5300 MHz CPU s)
CPU Caches:
  L1 Data 48 KiB (x16)
  L1 Instruction 64 KiB (x16)
  L2 Unified 3072 KiB (x16)
  L3 Unified 24576 KiB (x1)
Load Average: 4.10, 4.03, 3.01
***WARNING*** CPU scaling is enabled, the benchmark real time measurements may be noisy and will incur extra overhead.
***WARNING*** Library was built as DEBUG. Timings may be affected.
---------------------------------------------------------------------------------
Benchmark                       Time             CPU   Iterations UserCounters...
---------------------------------------------------------------------------------
DT_Doubles/125                826 ns          790 ns       928234 allocs/op=2
DT_Doubles/250                994 ns          941 ns       565679 allocs/op=2
DT_Doubles/500               2900 ns         2511 ns       260452 allocs/op=2
DT_Doubles/1000              5557 ns         4836 ns       150693 allocs/op=2
DT_Doubles/2000              8224 ns         7359 ns        95687 allocs/op=2
DT_PoseType/125              4420 ns         3891 ns       188178 allocs/op=2
DT_PoseType/250              7395 ns         6557 ns       116834 allocs/op=2
DT_PoseType/500             10377 ns         9307 ns        69656 allocs/op=2
DT_PoseType/1000            11851 ns        10827 ns        65189 allocs/op=2
DT_MultiSink/1               5000 ns         4298 ns       138732 allocs/op=2
DT_MultiSink/2               2917 ns         2699 ns       255604 allocs/op=4
DT_MultiSink/4               6807 ns         6082 ns       103897 allocs/op=8
DT_LoggedValueSet/10          295 ns          294 ns      2393448 items_per_second=34.0195M/s
DT_LoggedValueSet/100        2904 ns         2895 ns       239719 items_per_second=34.5374M/s
DT_LoggedValueSet/1000      29031 ns        28994 ns        24146 items_per_second=34.4896M/s
DT_SnapshotWithWriter       8084 ns         7160 ns        78670 allocs/op=2
```

Note: the "Library was built as DEBUG" warning above refers to the system
`libbenchmark` package (`libbenchmark-dev` 1.8.3-3), which was built without
`NDEBUG`; `data_tamer` itself and this benchmark binary were built with the
`release` preset (`CMAKE_BUILD_TYPE=Release`, `-O3`).

`allocs/op` is 2 for every single-sink case (`DT_Doubles`, `DT_PoseType`,
`DT_MultiSink/1`, `DT_SnapshotWithWriter`) and scales as `2 × sinks` for
`DT_MultiSink` (2, 4, 8 for 1, 2, 4 sinks), matching the expected `Snapshot`
copy into the queue per sink.

## Latency harness (`rt_latency`)

### 1000 values, 1 sink
```
rt_latency values=1000 sinks=1 writers=0 seconds=10 rate=1000Hz mcap=- fifo=0
samples=10000 p50=41530 ns p99=86083 ns p99.9=128921 ns max=217970 ns
allocations per call after warm-up: 2.0000
takeSnapshot returned false: 0 / 10000
fifo=0
```

### 1000 values, 2 sinks
```
rt_latency values=1000 sinks=2 writers=0 seconds=10 rate=1000Hz mcap=- fifo=0
samples=10000 p50=41535 ns p99=86216 ns p99.9=184814 ns max=331823 ns
allocations per call after warm-up: 4.0000
takeSnapshot returned false: 0 / 10000
fifo=0
```

### 1000 values, 1 sink, 2 writer threads
```
rt_latency values=1000 sinks=1 writers=2 seconds=10 rate=1000Hz mcap=- fifo=0
samples=10000 p50=53122 ns p99=113750 ns p99.9=145631 ns max=324286 ns
allocations per call after warm-up: 2.0000
takeSnapshot returned false: 0 / 10000
fifo=0
```

### 1000 values, 1 compressed MCAP sink
```
rt_latency values=1000 sinks=1 writers=0 seconds=10 rate=1000Hz mcap=/tmp/baseline.mcap fifo=0
samples=10000 p50=27956 ns p99=56871 ns p99.9=99758 ns max=392677 ns
allocations per call after warm-up: 2.0000
takeSnapshot returned false: 0 / 10000
fifo=0
```

### 1000 values, 1 sink, 2 writers, SCHED_FIFO (if available)

not run: no CAP_SYS_NICE. Ran without `sudo` as directed; the process fell
back to `SCHED_OTHER` and printed the message below. Full output:

```
rt_latency values=1000 sinks=1 writers=2 seconds=10 rate=1000Hz mcap=- fifo=1
SCHED_FIFO not available (need CAP_SYS_NICE); running SCHED_OTHER
samples=10000 p50=53878 ns p99=130422 ns p99.9=167372 ns max=375072 ns
allocations per call after warm-up: 2.0000
takeSnapshot returned false: 0 / 10000
fifo=0
```
