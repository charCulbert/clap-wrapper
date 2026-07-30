#include "detail/shared/parameter_flush.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace
{
using ClapWrapper::detail::shared::ParameterFlushLifecycle;

bool expect(bool condition, const char *message)
{
  if (!condition) std::cerr << "FAILED: " << message << '\n';
  return condition;
}

template <typename Predicate>
bool waitUntil(Predicate &&predicate)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (predicate()) return true;
    std::this_thread::yield();
  }
  return false;
}

bool testFailureRollback()
{
  ParameterFlushLifecycle lifecycle;
  bool pluginActivated = false;
  bool pluginDeactivated = false;
  bool startAttempted = false;

  const bool started = lifecycle.activateAndStart(
      [&]
      {
        pluginActivated = true;
        return true;
      },
      [&]
      {
        startAttempted = true;
        return false;
      },
      [&]
      {
        pluginDeactivated = true;
        pluginActivated = false;
      });

  bool flushServiced = false;
  const bool serviced = lifecycle.serviceIfInactive([&] { flushServiced = true; });

  return expect(!started && startAttempted && !pluginActivated && pluginDeactivated,
                "failed start rolls back plugin activation") &&
         expect(!lifecycle.requiresAudioThread(),
                "failed lifecycle transition restores inactive flush state") &&
         expect(serviced && flushServiced, "inactive flush resumes after lifecycle failure");
}

bool testActivationFailureSkipsStart()
{
  ParameterFlushLifecycle lifecycle;
  bool startAttempted = false;
  bool rollbackCalled = false;

  const bool started = lifecycle.activateAndStart(
      [] { return false; },
      [&]
      {
        startAttempted = true;
        return true;
      },
      [&] { rollbackCalled = true; });

  return expect(!started && !startAttempted && !rollbackCalled,
                "activation failure skips start and active-state rollback") &&
         expect(!lifecycle.requiresAudioThread(), "activation failure restores inactive state");
}

bool testActiveGate()
{
  ParameterFlushLifecycle lifecycle;
  const bool activated = lifecycle.activate([] { return true; });
  bool flushCalled = false;
  const bool servicedWhileActive = lifecycle.serviceIfInactive([&] { flushCalled = true; });
  const bool flushSkippedWhileActive = !flushCalled;

  lifecycle.deactivate([] {});
  const bool servicedWhileInactive = lifecycle.serviceIfInactive([&] { flushCalled = true; });

  return expect(activated && !servicedWhileActive && flushSkippedWhileActive,
                "active lifecycle rejects inactive flush service") &&
         expect(!lifecycle.requiresAudioThread() && servicedWhileInactive && flushCalled,
                "deactivation restores inactive flush service");
}

bool testFlushAndLifecycleDoNotOverlap()
{
  ParameterFlushLifecycle lifecycle;
  std::atomic_bool flushEntered{false};
  std::atomic_bool flushInside{false};
  std::atomic_bool releaseFlush{false};
  std::atomic_bool activationStarted{false};
  std::atomic_bool activationEntered{false};
  std::atomic_bool overlap{false};

  std::thread flushThread(
      [&]
      {
        lifecycle.serviceIfInactive(
            [&]
            {
              flushInside.store(true, std::memory_order_release);
              flushEntered.store(true, std::memory_order_release);
              while (!releaseFlush.load(std::memory_order_acquire))
                std::this_thread::yield();
              flushInside.store(false, std::memory_order_release);
            });
      });

  if (!waitUntil([&] { return flushEntered.load(std::memory_order_acquire); }))
  {
    releaseFlush.store(true, std::memory_order_release);
    flushThread.join();
    return expect(false, "inactive flush entered lifecycle exclusion");
  }

  std::thread activationThread(
      [&]
      {
        activationStarted.store(true, std::memory_order_release);
        lifecycle.activate(
            [&]
            {
              activationEntered.store(true, std::memory_order_release);
              overlap.store(flushInside.load(std::memory_order_acquire), std::memory_order_release);
              return true;
            });
      });

  const bool transitionPublished =
      waitUntil([&]
                {
                  return activationStarted.load(std::memory_order_acquire) &&
                         lifecycle.requiresAudioThread();
                });
  const bool activationWaited = !activationEntered.load(std::memory_order_acquire);

  releaseFlush.store(true, std::memory_order_release);
  flushThread.join();
  activationThread.join();

  const bool activationCompleted = activationEntered.load(std::memory_order_acquire);
  const bool noOverlap = !overlap.load(std::memory_order_acquire);
  lifecycle.deactivate([] {});

  return expect(transitionPublished, "activation publishes the flush gate before waiting") &&
         expect(activationWaited, "activation waits for an in-flight inactive flush") &&
         expect(activationCompleted && noOverlap, "flush and lifecycle callbacks never overlap");
}
}  // namespace

int main()
{
  bool ok = true;
  ok &= testFailureRollback();
  ok &= testActivationFailureSkipsStart();
  ok &= testActiveGate();
  ok &= testFlushAndLifecycleDoNotOverlap();
  if (ok) std::cout << "Parameter flush lifecycle tests passed\n";
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
