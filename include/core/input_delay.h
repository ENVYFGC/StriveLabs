#pragma once

#include <array>
#include <cstdint>

namespace input_delay {

constexpr int kMaxFrames = 15;
constexpr uint16_t kSystemBits = 0x1000 | 0x2000 | 0x4000;
inline bool forget_wanted = false;

class Queue {
public:
  uint16_t step(uint16_t live, int frames, uint64_t tick) {
    frames = frames < 0 ? 0 : frames > kMaxFrames ? kMaxFrames : frames;
    if (stamped_ && tick == tick_) return out_;
    if (!stamped_ || tick != tick_ + 1 || frames != frames_) {
      filled_ = 0;
      frames_ = frames;
    }
    stamped_ = true;
    tick_ = tick;
    words_[head_] = live;
    head_ = (head_ + 1) % kSize;
    if (filled_ < kSize) filled_++;
    const uint16_t old = filled_ > frames_ ? words_[(head_ + kSize - 1 - frames_) % kSize] : 0;
    out_ = static_cast<uint16_t>((old & ~kSystemBits) | (live & kSystemBits));
    return out_;
  }

  void forget() { stamped_ = false; }

private:
  static constexpr int kSize = kMaxFrames + 1;
  std::array<uint16_t, kSize> words_{};
  int head_ = 0;
  int filled_ = 0;
  int frames_ = 0;
  uint64_t tick_ = 0;
  bool stamped_ = false;
  uint16_t out_ = 0;
};

}
