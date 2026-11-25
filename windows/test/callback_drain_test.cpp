// Teardown drains callback leases without pumping messages. That is only
// safe because a coroutine never holds its lease while suspended (see
// ScopedLease): a continuation that needs this thread to resume would
// otherwise deadlock the drain. This test models such a coroutine, one that
// hops to another apartment and back to the one it started on.
#include "callback_drain.h"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <iostream>
#include <thread>

#include <windows.h>
#include <winrt/base.h>

using universal_ble::AsyncOperationTracker;
using universal_ble::ScopedLease;
using universal_ble::WaitForCallbacksWithoutMessagePump;

namespace {

constexpr UINT kForeignMessage = WM_APP + 2;
int foreign_messages_dispatched = 0;

LRESULT CALLBACK ForeignWindowProc(HWND window, UINT message, WPARAM wparam,
                                   LPARAM lparam) {
  if (message == kForeignMessage) {
    ++foreign_messages_dispatched;
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

struct ResumeOnInitializedMta {
  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> continuation) const {
    std::thread([continuation] {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      continuation.resume();
      winrt::uninit_apartment();
    }).detach();
  }

  void await_resume() const noexcept {}
};

struct Outcome {
  std::atomic<bool> finished{false};
  std::atomic<bool> used_owner{false};
  std::atomic<bool> abandoned{false};
  std::atomic<int32_t> error{0};
};

// The shape of every plugin coroutine: release the lease before awaiting,
// retake it after, and touch nothing of the owner when that fails.
winrt::fire_and_forget OwnerCoroutine(const AsyncOperationTracker &owner,
                                      Outcome &outcome) {
  ScopedLease lease(owner);
  try {
    if (!lease.Acquire()) {
      outcome.abandoned = true;
      outcome.finished = true;
      co_return;
    }
    winrt::apartment_context original_apartment;
    lease.Release();
    co_await ResumeOnInitializedMta{};
    co_await original_apartment;
    if (!lease.Acquire()) {
      outcome.abandoned = true;
    } else {
      outcome.used_owner = true;
    }
  } catch (const winrt::hresult_error &exception) {
    outcome.error = exception.code().value;
  } catch (...) {
    outcome.error = -1;
  }
  outcome.finished = true;
}

bool PumpUntil(const std::atomic<bool> &done,
               std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!done) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    DWORD index = 0;
    CoWaitForMultipleHandles(
        COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES, 10, 0,
        nullptr, &index);
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return true;
}

bool Check(bool condition, const char *expression, int line) {
  if (condition) {
    return true;
  }
  std::cerr << "CHECK failed at line " << line << ": " << expression
            << std::endl;
  return false;
}

}  // namespace

#define CHECK(expression)                            \
  do {                                               \
    if (!Check((expression), #expression, __LINE__)) \
      return 1;                                      \
  } while (false)

int main() {
  winrt::init_apartment(winrt::apartment_type::single_threaded);

  WNDCLASSW cls{};
  cls.lpfnWndProc = ForeignWindowProc;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"UniversalBleCallbackDrainTest";
  CHECK(RegisterClassW(&cls) != 0);
  HWND window = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
  CHECK(window != nullptr);

  // While the owner is alive, the coroutine comes back and uses it.
  {
    AsyncOperationTracker owner;
    Outcome outcome;
    OwnerCoroutine(owner, outcome);
    CHECK(PumpUntil(outcome.finished, std::chrono::seconds(2)));
    CHECK(outcome.error == 0);
    CHECK(outcome.used_owner);
    CHECK(!outcome.abandoned);
    CHECK(owner.IsIdle());
  }

  // Teardown while the coroutine is suspended: the drain returns at once,
  // without dispatching foreign messages, although the continuation still
  // needs this thread.
  {
    auto owner = std::make_unique<AsyncOperationTracker>();
    Outcome outcome;
    OwnerCoroutine(*owner, outcome);
    CHECK(PostMessageW(window, kForeignMessage, 0, 0));

    const auto started = std::chrono::steady_clock::now();
    owner->Close();
    WaitForCallbacksWithoutMessagePump(*owner);
    const auto drained = std::chrono::steady_clock::now() - started;
    CHECK(drained < std::chrono::milliseconds(500));
    CHECK(foreign_messages_dispatched == 0);
    owner.reset();

    // The message loop runs again after teardown; the continuation resumes
    // and finds the owner gone.
    CHECK(PumpUntil(outcome.finished, std::chrono::seconds(2)));
    CHECK(outcome.error == 0);
    CHECK(outcome.abandoned);
    CHECK(!outcome.used_owner);
    CHECK(foreign_messages_dispatched == 1);
  }

  DestroyWindow(window);
  UnregisterClassW(cls.lpszClassName, cls.hInstance);
  winrt::uninit_apartment();
  return 0;
}
