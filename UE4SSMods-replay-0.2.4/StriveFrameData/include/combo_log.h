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
    file_.open(path, std::ios::out | std::ios::trunc);
    if (file_.is_open()) {
      write_impl("ComboLog initialized");
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
