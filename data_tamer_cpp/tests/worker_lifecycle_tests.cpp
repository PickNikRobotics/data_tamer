// SinkWorker lifecycle calls (stop(), start(), drain(), destruction) made from sink
// callbacks and from several threads at once, and the control operations that would
// wait for the worker whose callback calls them.
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/data_sink.hpp"
#include "data_tamer_v2/data_tamer.hpp"
#include "gate.hpp"
#include "hang_watchdog.hpp"
#include "observed_thread.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace DataTamer2;
using DataTamerTest::Attached;
using DataTamerTest::expectFinishes;

namespace
{
/// Where a lifecycle call made from a callback ended.
enum class Outcome
{
  NotCalled,
  LogicError,
  OtherException,
  Returned
};

/// Calls `call` on its own worker from the first onSchema() or onSnapshot().
class ReentrantSink : public DataSink
{
public:
  enum class From
  {
    Schema,
    Snapshot
  };

  ReentrantSink(From from, std::function<void(SinkWorker&)> call)
    : from_(from), call_(std::move(call))
  {}

  SinkWorker* self = nullptr;
  std::atomic<Outcome> outcome{ Outcome::NotCalled };
  std::atomic<int> snapshots{ 0 };
  std::atomic<int> stops{ 0 };

protected:
  void onSchema(const Schema&) override
  {
    if(from_ == From::Schema)
    {
      callOnce();
    }
  }
  void onSnapshot(const SnapshotRef&) override
  {
    if(from_ == From::Snapshot)
    {
      callOnce();
    }
    ++snapshots;
  }
  void onStop() override { ++stops; }

private:
  void callOnce()
  {
    if(outcome != Outcome::NotCalled)
    {
      return;
    }
    try
    {
      call_(*self);
      outcome = Outcome::Returned;
    }
    catch(const std::logic_error&)
    {
      outcome = Outcome::LogicError;
    }
    catch(...)
    {
      outcome = Outcome::OtherException;
    }
  }

  From from_;
  std::function<void(SinkWorker&)> call_;
};

const char* fromName(ReentrantSink::From from)
{
  return from == ReentrantSink::From::Schema ? "onSchema()" : "onSnapshot()";
}

struct Call
{
  const char* name;
  void (*function)(SinkWorker&);
};

constexpr Call kCalls[] = { { "drain()", [](SinkWorker& w) { w.drain(); } },
                            { "start()", [](SinkWorker& w) { w.start(); } },
                            { "stop()", [](SinkWorker& w) { w.stop(); } } };

/// Counts its onStop() and onStart() calls.
class CountingSink : public DataSink
{
public:
  std::atomic<int> stops{ 0 };
  std::atomic<int> starts{ 0 };

protected:
  void onSchema(const Schema&) override {}
  void onSnapshot(const SnapshotRef&) override {}
  void onStop() override { ++stops; }
  void onStart() override { ++starts; }
};

/// What a QueryingSink saw; outlives the sink.
struct QueryJournal
{
  DataTamerTest::Gate gate;  // parks the callback while it holds the channel
  std::atomic<int> stops{ 0 };
  std::atomic<bool> destroyed{ false };
};

/// Queries its channel from onSnapshot() through a weak_ptr (a const query, allowed
/// from a callback), and waits there until the test has dropped its own reference.
class QueryingSink : public DataSink
{
public:
  QueryingSink(std::weak_ptr<LogChannel> channel, std::shared_ptr<QueryJournal> journal)
    : channel_(std::move(channel)), journal_(std::move(journal))
  {}
  ~QueryingSink() override { journal_->destroyed = true; }

protected:
  void onSchema(const Schema&) override {}
  void onSnapshot(const SnapshotRef&) override
  {
    if(auto channel = channel_.lock())
    {
      (void)channel->getSchema();
      journal_->gate.pause();
    }  // the last reference to the channel, and so to this worker, can end here
  }
  void onStop() override { ++journal_->stops; }

private:
  std::weak_ptr<LogChannel> channel_;
  std::shared_ptr<QueryJournal> journal_;
};

/// Runs `function` on two threads released together; returns how many calls threw.
int runTwiceAtOnce(const std::function<void()>& function)
{
  std::latch ready(2);
  std::atomic<int> thrown{ 0 };
  const auto run = [&] {
    ready.arrive_and_wait();
    try
    {
      function();
    }
    catch(...)
    {
      ++thrown;
    }
  };
  std::thread first(run);
  std::thread second(run);
  first.join();
  second.join();
  return thrown;
}

bool waitFor(const std::function<bool()>& condition)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while(!condition())
  {
    if(std::chrono::steady_clock::now() > deadline)
    {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}
}  // namespace

// From a callback, drain(), start() and stop() of the sink's own worker would wait
// for that callback: they throw std::logic_error and change nothing.
TEST(WorkerLifecycle, LifecycleCallsFromOwnCallbacksThrowLogicError)
{
  for(const auto from : { ReentrantSink::From::Schema, ReentrantSink::From::Snapshot })
  {
    for(const auto& call : kCalls)
    {
      SCOPED_TRACE(std::string(call.name) + " from " + fromName(from));
      expectFinishes([from, call] {
        Attached<ReentrantSink> sink(from, call.function);
        sink->self = sink.worker.get();
        double value = 1;
        auto channel = DataTamerTest::channelWith(sink, &value, "reentrant");
        channel->startLogging();  // onSchema() on this thread
        ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
        ASSERT_TRUE(waitFor([&] { return sink->snapshots == 1; }));
        EXPECT_EQ(sink->outcome, Outcome::LogicError);
        // The worker still accepts and delivers, and stop() still finishes the sink.
        EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
        EXPECT_TRUE(waitFor([&] { return sink->snapshots == 2; }));
        sink.worker->stop();
        EXPECT_EQ(sink->stops, 1);
      });
    }
  }
}

// Two shutdown paths stopping the same workers at once, through stopAll() or
// stop(), or a stop() racing a start(): every call returns and the sink sees
// matching onStop() and onStart() calls.
TEST(WorkerLifecycle, ConcurrentLifecycleCallsOnOneWorker)
{
  expectFinishes([] {
    for(int round = 0; round < 100; ++round)
    {
      ChannelsRegistry registry;
      std::vector<Attached<CountingSink>> sinks;
      for(int i = 0; i < 4; ++i)
      {
        sinks.emplace_back();
        registry.addDefaultSink(sinks.back());
      }
      auto channel = registry.getChannel("stops");
      double value = 1;
      channel->registerValue("value", &value);
      ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
      ASSERT_EQ(runTwiceAtOnce([&] { registry.stopAll(); }), 0);
      for(const auto& sink : sinks)
      {
        ASSERT_EQ(sink->stops, 1);
      }
    }
    Attached<CountingSink> sink;
    for(int round = 0; round < 100; ++round)
    {
      const int stops = sink->stops;
      ASSERT_EQ(runTwiceAtOnce([&] { sink.worker->stop(); }), 0);
      ASSERT_EQ(sink->stops, stops + 1);  // the second stop() found it stopped
      std::atomic<bool> stopping{ false };
      ASSERT_EQ(runTwiceAtOnce([&] {
                  stopping.exchange(true) ? sink.worker->stop() : sink.worker->start();
                }),
                0);
      // Stopped, so the start() finishes first or the stop() does nothing.
      ASSERT_GE(sink->stops - sink->starts, 0);
      ASSERT_LE(sink->stops - sink->starts, 1);
      sink.worker->start();
      ASSERT_EQ(sink->stops, sink->starts);
    }
  });
}

// The last reference to a worker released by its own callback: the channel, which
// owned the worker, is destroyed on the worker thread. The worker still finishes:
// onStop() runs once and the sink is destroyed.
TEST(WorkerLifecycle, WorkerReleasedByItsOwnCallbackFinishesTheSink)
{
  expectFinishes([] {
    auto journal = std::make_shared<QueryJournal>();
    auto channel = LogChannel::create("released_by_callback");
    double value = 1;
    channel->registerValue("value", &value);
    channel->addDataSink(  // the channel holds the only reference to the worker
        SinkWorker::create<QueryingSink>(channel, journal));
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    ASSERT_TRUE(journal->gate.waitEntered());
    channel.reset();
    journal->gate.release();
    EXPECT_TRUE(waitFor([&] { return journal->destroyed.load(); }));
    EXPECT_EQ(journal->stops, 1);
  });
}

namespace
{
/// The channels and registry a callback of `worker` acts on.
struct Scene
{
  Scene()
  {
    started->registerValue("value", &value);
    started->startLogging();
    attached->registerValue("value", &value);
    auto channel = registry.getChannel("registered");
    channel->registerValue("value", &value);
    channel->startLogging();
  }

  /// Attaches `sink` to `attached`; returns a started channel whose snapshots run its
  /// callbacks.
  std::shared_ptr<LogChannel> arm(Attached<ReentrantSink>& sink)
  {
    worker = sink.worker;
    sink->self = worker.get();
    attached->addDataSink(worker);  // not started: no onSchema() yet
    auto feed = DataTamerTest::channelWith(worker, &value, "feed");
    feed->startLogging();
    return feed;
  }

  double value = 1;
  std::shared_ptr<SinkWorker> worker;  // the worker whose callback makes the call
  std::shared_ptr<LogChannel> started = LogChannel::create("started");    // without it
  std::shared_ptr<LogChannel> attached = LogChannel::create("attached");  // see arm()
  ChannelsRegistry registry;  // one started channel, "registered"
};

/// A control operation that waits for `Scene::worker`, and what it leaves unchanged.
struct ControlCall
{
  const char* name;
  void (*call)(Scene&);
  void (*expectUnchanged)(Scene&);
};

const ControlCall kWaitingCalls[] = {
  { "addDataSink() to a started channel",
    [](Scene& s) { s.started->addDataSink(s.worker); },
    [](Scene& s) { EXPECT_EQ(s.started->getNumberOfSinks(), 0u); } },
  { "startLogging() of a channel it is attached to",
    [](Scene& s) { s.attached->startLogging(); },
    [](Scene& s) { EXPECT_FALSE(s.attached->isLoggingStarted()); } },
  { "first takeSnapshot() of a channel it is attached to",
    [](Scene& s) { (void)s.attached->takeSnapshot(); },
    [](Scene& s) { EXPECT_FALSE(s.attached->isLoggingStarted()); } },
  { "ChannelsRegistry::addDefaultSink() with a started channel",
    [](Scene& s) { s.registry.addDefaultSink(s.worker); },
    [](Scene& s) {
      EXPECT_EQ(s.registry.getChannel("registered")->getNumberOfSinks(), 0u);
    } },
};

/// Refuses the first schema it hears.
class RefusingOnceSink : public DataSink
{
protected:
  void onSchema(const Schema&) override
  {
    if(!refused_)
    {
      refused_ = true;
      throw std::runtime_error("schema refused");
    }
  }
  void onSnapshot(const SnapshotRef&) override {}

private:
  bool refused_ = false;  // the worker serializes the callbacks
};
}  // namespace

// From a callback, a call that would wait for its own worker throws and changes nothing;
// from the test thread it works.
TEST(WorkerLifecycle, ControlCallsThatWaitForTheirOwnWorkerThrowLogicError)
{
  for(const auto from : { ReentrantSink::From::Schema, ReentrantSink::From::Snapshot })
  {
    for(const auto& control : kWaitingCalls)
    {
      SCOPED_TRACE(std::string(control.name) + " from " + fromName(from));
      expectFinishes([from, control] {
        Scene s;
        Attached<ReentrantSink> sink(from,
                                     [&s, control](SinkWorker&) { control.call(s); });
        auto feed = s.arm(sink);
        ASSERT_EQ(feed->takeSnapshot(), SnapshotResult::ok);
        ASSERT_TRUE(waitFor([&] { return sink->snapshots == 1; }));
        EXPECT_EQ(sink->outcome, Outcome::LogicError);
        control.expectUnchanged(s);
        EXPECT_NO_THROW(control.call(s));
      });
    }
  }
}

#if defined(__linux__)
// The same call from a callback, while another thread's call waits for the worker:
// the callback's throws, the other finishes once the callback returns.
TEST(WorkerLifecycle, ControlCallsBehindAWaitForTheirOwnWorkerThrowLogicError)
{
  for(const auto& control : kWaitingCalls)
  {
    SCOPED_TRACE(control.name);
    expectFinishes([control] {
      Scene s;
      DataTamerTest::Gate gate;  // parks the callback, which holds the worker
      Attached<ReentrantSink> sink(ReentrantSink::From::Snapshot,
                                   [&s, &gate, control](SinkWorker&) {
                                     gate.pause();
                                     control.call(s);
                                   });
      auto feed = s.arm(sink);
      ASSERT_EQ(feed->takeSnapshot(), SnapshotResult::ok);
      ASSERT_TRUE(gate.waitEntered());
      {
        DataTamerTest::ObservedThread other([&s, control] { control.call(s); });
        EXPECT_TRUE(other.sleeps());  // it waits for the parked callback
        gate.release();
      }  // joined: its call returned, after the callback's
      EXPECT_EQ(sink->outcome, Outcome::LogicError);
    });
  }
}
#endif

// After a failed start that reached the worker, a retry from its callback throws too
// (it would lock in reverse order); from the test thread it works.
TEST(WorkerLifecycle, CallbacksCannotRetryAStartThatAnnouncedToTheirWorker)
{
  struct Start
  {
    const char* name;
    void (*call)(LogChannel&);
  };
  const Start starts[] = { { "startLogging()", [](LogChannel& c) { c.startLogging(); } },
                           { "first takeSnapshot()",
                             [](LogChannel& c) { (void)c.takeSnapshot(); } } };
  for(const auto& start : starts)
  {
    SCOPED_TRACE(start.name);
    expectFinishes([start] {
      Scene s;
      std::shared_ptr<LogChannel> retried;
      Attached<ReentrantSink> sink(ReentrantSink::From::Snapshot,
                                   [&](SinkWorker&) { start.call(*retried); });
      auto feed = s.arm(sink);
      // Slots are filled from the highest down and announced from the lowest up: the
      // worker hears the schema before `refusing` throws.
      retried = DataTamerTest::channelWith(DataTamerTest::manual<RefusingOnceSink>(),
                                           &s.value, "retried");
      retried->addDataSink(s.worker);
      EXPECT_THROW(retried->startLogging(), std::runtime_error);
      ASSERT_EQ(feed->takeSnapshot(), SnapshotResult::ok);
      ASSERT_TRUE(waitFor([&] { return sink->outcome != Outcome::NotCalled; }));
      EXPECT_EQ(sink->outcome, Outcome::LogicError);
      EXPECT_FALSE(retried->isLoggingStarted());
      EXPECT_NO_THROW(start.call(*retried));
      EXPECT_TRUE(retried->isLoggingStarted());
    });
  }
}

// What waits only for other workers, or for none, still works from a callback.
TEST(WorkerLifecycle, CallbacksStillMakeControlCallsThatDoNotWaitForTheirWorker)
{
  expectFinishes([] {
    Scene s;
    double late = 2;
    auto other = DataTamerTest::manual<DummySink>();
    // the other worker attached, not started
    auto fresh = DataTamerTest::channelWith(other, &s.value, "fresh");
    auto open = LogChannel::create("open");  // nothing attached, not started
    std::shared_ptr<LogChannel> feed;
    std::shared_ptr<LogChannel> extra;  // started, this worker attached
    struct
    {
      bool attached_other = false, attached_open = false, attached_twice = true;
      SnapshotResult fresh_snapshot{}, feed_snapshot{};
    } seen;
    Attached<ReentrantSink> sink(ReentrantSink::From::Snapshot, [&](SinkWorker&) {
      seen.attached_other = s.started->addDataSink(other);  // onSchema() of `other` here
      fresh->startLogging();
      seen.fresh_snapshot = fresh->takeSnapshot();
      seen.attached_open = open->addDataSink(s.worker);  // schema open: no announcement
      (void)open->registerValue("late", &late);
      seen.attached_twice = feed->addDataSink(s.worker);  // attached already: false
      seen.feed_snapshot = feed->takeSnapshot();          // queued on this very worker
      extra->removeDataSink(s.worker);
      other.drain();
    });
    feed = s.arm(sink);
    extra = DataTamerTest::channelWith(s.worker, &s.value, "extra");
    extra->startLogging();
    ASSERT_EQ(feed->takeSnapshot(), SnapshotResult::ok);
    ASSERT_TRUE(waitFor([&] { return sink->snapshots == 2; }));  // and its own snapshot
    EXPECT_EQ(sink->outcome, Outcome::Returned);
    EXPECT_TRUE(seen.attached_other);
    EXPECT_EQ(seen.fresh_snapshot, SnapshotResult::ok);
    EXPECT_TRUE(seen.attached_open);
    EXPECT_FALSE(seen.attached_twice);
    EXPECT_EQ(seen.feed_snapshot, SnapshotResult::ok);
    EXPECT_EQ(other->schemasCount(), 2u);  // "started" and "fresh"
    EXPECT_EQ(other->snapshotsCount(fresh->getSchema().hash), 1);
    EXPECT_TRUE(fresh->isLoggingStarted());
    EXPECT_EQ(open->getNumberOfSinks(), 1u);
    EXPECT_EQ(open->getSchema().fields.size(), 1u);
    EXPECT_EQ(extra->getNumberOfSinks(), 0u);
  });
}
