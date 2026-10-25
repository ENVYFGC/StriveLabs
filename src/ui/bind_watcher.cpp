#include "bind_watcher.h"

#include <bitset>
#include <condition_variable>
#include <mutex>

#include <Windows.h>

LRESULT CALLBACK KeyboadCallback(int nCode, WPARAM wParam, LPARAM lParam);

class BindWorker {
  std::atomic<bool> isRunning;
  HHOOK kbh = nullptr;
  std::thread worker_thread;

  void run() {
    MSG msg;
    if (!kbh) kbh = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboadCallback, GetModuleHandle(NULL), 0);

    while (GetMessage(&msg, nullptr, 0, 0) > 0 && isRunning.load()) {
    }

    UnhookWindowsHookEx(kbh);
  }

public:
  BindWorker() {
    isRunning = true;
    worker_thread = std::thread(&BindWorker::run, this);
  }
  ~BindWorker() {
    isRunning = false;
    auto handle = GetThreadId(worker_thread.native_handle());
    PostThreadMessage(handle, WM_USER, 0, 0);
    worker_thread.join();
  }
  LRESULT callNext(int nCode, WPARAM wParam, LPARAM lParam) { return CallNextHookEx(kbh, nCode, wParam, lParam); }
};

class BindWatcher {
  BindWorker worker;
  std::mutex state_mutex;
  std::condition_variable await_con;
  std::bitset<0xff> filter;
  std::vector<int> inputs;
  std::atomic<bool> stopped{false};
  std::atomic<bool> capture_all{false};
  std::atomic<bool> esc_capture{false};
  std::atomic<bool> esc_pressed{false};
  std::bitset<0x100> held;  // hook thread only

  bool markKey(DWORD code) {
    std::lock_guard state_lock(state_mutex);
    if (!capture_all.load() && !filter.test(code & 0xFF)) return false;
    if (inputs.size() > 100) inputs.erase(inputs.begin()); // only store latest 100 inputs
    inputs.push_back(code);
    return true;
  }

  BindWatcher() = default;

public:
  ~BindWatcher() {
    // Wake any thread blocked in getInputs(await=true) so it can exit cleanly.
    stopped.store(true);
    await_con.notify_all();
  }

  void addToFilter(unsigned long code) { filter.set(code & 0xFF, true); }
  void setCaptureAll(bool on) { capture_all.store(on); }
  void setEscapeCapture(bool on) { esc_capture.store(on); if (!on) esc_pressed.store(false); }
  bool takeEscape() { return esc_pressed.exchange(false); }

  std::vector<int> getInputs(bool await) {
    std::unique_lock state_lock(state_mutex);
    if (await) {
      await_con.wait(state_lock, [this] { return !inputs.empty() || stopped.load(); });
    }
    std::vector<int> result;
    result.swap(inputs);
    return result;
  }

  LRESULT callback(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= HC_ACTION && (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)) {
      const DWORD vk = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam)->vkCode & 0xFF;
      held.reset(vk);
      if (vk == VK_ESCAPE && esc_capture.load()) return 1;  // the prompt had the press too
    }
    if (nCode >= HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
      // Only process keystrokes when the game window has focus.
      // WH_KEYBOARD_LL is system-wide; without this check, typing in any
      // other app (browser, etc.) after Alt+Tab fires our hook and can
      // corrupt the Windows input stack if the callback misbehaves.
      //
      // Cache the game HWND to avoid a full window enumeration on every
      // keystroke — FindWindowA under fast input can exceed the hook timeout.
      // Any window owned by this process counts (the Game Pass build does not
      // use the "UnrealWindow" class name).
      HWND fg = GetForegroundWindow();
      DWORD fg_pid = 0;
      if (fg != nullptr) GetWindowThreadProcessId(fg, &fg_pid);
      if (fg != nullptr && fg_pid == GetCurrentProcessId()) {
        KBDLLHOOKSTRUCT &k = *(KBDLLHOOKSTRUCT *)lParam;
        const DWORD vk = k.vkCode & 0xFF;
        const bool repeat = held.test(vk);
        held.set(vk);
        if (vk == VK_ESCAPE && esc_capture.load()) {
          if (!repeat) esc_pressed.store(true);
          return 1;  // not for the game while the prompt is up
        }
        if (markKey(repeat ? (vk | BindWatcherI::REPEAT) : vk)) await_con.notify_all();
      }
    }
    return worker.callNext(nCode, wParam, lParam);
  }

  static BindWatcher &instance() {
    static BindWatcher singleton;
    return singleton;
  }
};

LRESULT CALLBACK KeyboadCallback(int nCode, WPARAM wParam, LPARAM lParam) {
  return BindWatcher::instance().callback(nCode, wParam, lParam);
}

void BindWatcherI::setEscapeCapture(bool on) { BindWatcher::instance().setEscapeCapture(on); }
bool BindWatcherI::takeEscape() { return BindWatcher::instance().takeEscape(); }

void BindWatcherI::addToFilter(unsigned long code) {
  BindWatcher::instance().addToFilter(code);
}

void BindWatcherI::setCaptureAll(bool on) {
  BindWatcher::instance().setCaptureAll(on);
}

std::vector<int> BindWatcherI::getInputs(bool await) {
  return BindWatcher::instance().getInputs(await);
}