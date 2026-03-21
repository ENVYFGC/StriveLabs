#pragma once

#include <string>
#include <atomic>
#include <filesystem>

struct IDXGISwapChain;

enum class VideoState {
  Idle,
  Downloading,
  Buffering,
  Playing,
  Paused,
  Finished,
  Error
};

class VideoPlayer {
public:
  static VideoPlayer& instance();

  void setCacheDir(const std::filesystem::path& dir);
  void setYtDlpPath(const std::filesystem::path& path);

  // Call once after D3D11 device exists (e.g. on_unreal_init)
  void initPresentHook();

  // Start downloading + playing a YouTube video
  void startVideo(const std::string& youtube_url);

  // Controls
  void stop();
  void togglePause();
  void toggleMute();
  void setFullscreen(bool fs);
  bool isFullscreen() const;

  // Queries
  VideoState state() const;
  bool isActive() const;
  bool isMuted() const;
  float downloadProgress() const;
  std::wstring statusText() const;

  // Called from hooked IDXGISwapChain::Present (render thread)
  void onPresent(IDXGISwapChain* swapchain);

  void shutdown();

private:
  VideoPlayer();
  ~VideoPlayer();
  VideoPlayer(const VideoPlayer&) = delete;
  VideoPlayer& operator=(const VideoPlayer&) = delete;

  struct Impl;
  Impl* impl_;
};
