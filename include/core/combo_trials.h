// Combo trials in training: combos saved or downloaded in Combo Maker, loaded into
// training's own Combo Recipe so the game runs the trial (rows light up as each
// move lands, CLEAR at the end). Everything here runs on the game thread.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace combo_trials {
  struct Entry {
    int list = 0, slot = 0;   // which Combo Maker list in the save data, and where in it
    int steps = 0;
    int version = 0;          // battle version the combo was made on
    bool older = false;       // made on an older patch than the newest combo saved
    unsigned number = 0;      // Combo Maker number (0 for your own unsent ones)
    std::wstring creator, title;
  };

  void init(const std::filesystem::path& dir);  // finds what it needs in the game; logs what's missing
  bool available();
  std::vector<Entry> list();  // combos for the character being played, newest list first
  // Puts the combo into training's Combo Recipe and its start settings (position,
  // side, range, gauges...) into training, then asks for a position reset. Your own
  // settings are kept aside (also in a file) until stop(). Empty on success, else why not.
  std::wstring load(const Entry& e);
  // Ends the trial: clears the recipe and puts your training settings back.
  void stop();
  bool active();
  // Game thread, every battle update: puts settings back when training is left, or
  // after a crash or quit during a trial (from the file).
  void update(bool in_training);
  // Game thread, when no battle has run for a moment: a trial still on ends there.
  void idle();

  // Battle version for training, as the game counts them (39 = 5.03), 0 = the
  // current one. Takes effect the next time training loads; both sides get it.
  void setTrainingVersion(int version);
  int trainingVersion();
  // Game thread: the picked version is only taken up while no battle runs.
  void latchVersion(bool battle_running);
}
