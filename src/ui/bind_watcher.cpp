#include "bind_watcher.h"

#include <bitset>
#include <condition_variable>
#include <mutex>

#include <Windows.h>

namespace recolor_test { bool setupRunning(); }

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
  std::bitset<0x100> filter;
  std::vector<int> inputs;
  std::atomic<bool> stopped{false};
  std::atomic<bool> capture_all{false};
  // Esc goes to the prompt only while the game thread keeps renewing this: if the
  // fight goes away with the prompt up, Esc comes back to the game by itself.
  std::atomic<ULONGLONG> esc_capture_until{0};
  std::atomic<bool> esc_pressed{false};
  std::bitset<0x100> held;       // hook thread only
  bool esc_down_swallowed = false;  // hook thread only: its key-up is ours too

  bool escCaptured() const { return GetTickCount64() < esc_capture_until.load(); }

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
  void setEscapeCapture(bool on) {
    if (on && !escCaptured()) esc_pressed.store(false);  // a new prompt starts clean
    esc_capture_until.store(on ? GetTickCount64() + 500 : 0);
    if (!on) esc_pressed.store(false);
  }
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
      if (vk == VK_ESCAPE && esc_down_swallowed) {  // only the release of a press we kept
        esc_down_swallowed = false;
        return 1;
      }
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
        // The color editor's first-time setup is up ("please wait"): keys are kept from the game and the mod, but not
        // Windows' own (Alt+Tab, Alt+F4, the Windows key, Ctrl+Shift+Esc) or the modifiers
        const bool system = vk == VK_LWIN || vk == VK_RWIN || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_CONTROL ||
                            vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SNAPSHOT ||
                            (k.flags & LLKHF_ALTDOWN) || (vk == VK_ESCAPE && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) ||
                            ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) ||
                            (vk >= VK_VOLUME_MUTE && vk <= VK_LAUNCH_APP2);  // (volume and media keys)
        if (!system && recolor_test::setupRunning()) return 1;
        const bool repeat = held.test(vk);
        held.set(vk);
        if (vk == VK_ESCAPE && escCaptured()) {
          esc_down_swallowed = true;
          // While a key is being rebound, Esc still cancels the rebind.
          if (capture_all.load()) { if (!repeat && markKey(vk)) await_con.notify_all(); }
          else if (!repeat) esc_pressed.store(true);
          return 1;  // not for the game while the prompt is up
        }
        int code = static_cast<int>(vk);
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) code |= BindWatcherI::KEY_CTRL;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) code |= BindWatcherI::KEY_SHIFT;
        if (GetAsyncKeyState(VK_MENU) & 0x8000) code |= BindWatcherI::KEY_ALT;
        if (repeat) code |= BindWatcherI::REPEAT;
        if (markKey(static_cast<DWORD>(code))) await_con.notify_all();
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