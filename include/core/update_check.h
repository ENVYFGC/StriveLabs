#pragma once
#include <string>

// Checks GitHub for a newer StriveLabs once, in the background, when the menu first opens. It only reports
// it, nothing is downloaded.
namespace update_check {
  constexpr const char* kVersion = "3.5";  // this build's version (the release tag without its "v")
  void start();                            // call once, from any thread
  std::string newer();                     // the newer release's tag ("v3.6"), or "" if none or not known yet
  const char* releasesUrl();
  void openReleases();                     // opens the release page in the browser (off the render thread)
  // True if the game was built after the newest build this StriveLabs was tested on (a patch since). The color
  // editor warns about it.
  bool gamePatchedSince();
}
