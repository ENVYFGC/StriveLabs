#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

class ComboLog {
  std::ofstream file_;
  std::mutex mutex_;

  ComboLog() = default;
  ComboLog(const ComboLog&) = delete;
  ComboLog& operator=(const ComboLog&) = delete;

public:
  static ComboLog& instance() {
    static ComboLog inst;
    return inst;
  }

  void init(const std::filesystem::path& dir) {
    std::lock_guard lock(mutex_);
    auto path = dir / "combo_log.txt";
    {  // The last runs' logs are kept (combo_log.1.txt is the run before this one, up to .5), so a session's log
       // survives the game being started again
      std::error_code ec;
      constexpr int kKeep = 5;
      auto old = [&](int n) { return dir / ("combo_log." + std::to_string(n) + ".txt"); };
      std::filesystem::remove(old(kKeep), ec);
      for (int n = kKeep - 1; n >= 1; n--)
        if (std::filesystem::exists(old(n), ec)) std::filesystem::rename(old(n), old(n + 1), ec);
      if (std::filesystem::exists(path, ec)) std::filesystem::rename(path, old(1), ec);
    }
    file_.open(path, std::ios::out | std::ios::trunc);
    if (file_.is_open()) {
      auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
      std::tm tm_buf{};
      localtime_s(&tm_buf, &now);
      std::ostringstream day;
      day << std::put_time(&tm_buf, "%Y-%m-%d");
      write_impl("ComboLog initialized " + day.str() + " (earlier runs: combo_log.1.txt is the one before this, up to .5)");
    }
  }

  void write(const std::string& msg) {
    std::lock_guard lock(mutex_);
    if (file_.is_open()) {
      write_impl(msg);
    }
  }

private:
  void write_impl(const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    file_ << std::put_time(&tm_buf, "%H:%M:%S") << " | " << msg << "\n";
    file_.flush();
  }
};

#define COMBO_LOG(msg) ComboLog::instance().write(msg)
