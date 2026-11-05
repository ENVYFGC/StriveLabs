// Combo trials in training: combos saved or downloaded in Combo Maker, loaded into
// training's own Combo Recipe so the game runs the trial (rows light up as each
// move lands, CLEAR at the end). Everything here runs on the game thread.
#pragma once
#include <cstdint>
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
  // The same for a combo block held in memory (a Combo Maker download not kept).
  std::wstring loadData(const void* block, size_t size);
  // Ends the trial: clears the recipe and puts your training settings back.
  void stop();
  bool active();
  int trialVersion();  // battle version the loaded combo was made on, 0 if none
  std::wstring trialName();  // the loaded combo's title, or its number; empty if none
  // The loaded combo's own recording (its maker's inputs), played on your character from
  // the next position reset; your pad does nothing until it ends. False if no combo is
  // loaded or it holds no recording.
  bool watch();
  void stopWatching();
  bool watching();
  // Game thread: at each round start or position reset.
  void onRoundStart();
  // Game thread: your character's input for this frame while watching, mirrored when you
  // face the other way than its maker did. False when not watching.
  bool watchNext(bool facing_left, uint16_t& flags);
  // Game thread, every battle update: puts settings back when training is left, or
  // after a crash or quit during a trial (from the file).
  void update(bool in_training);
  // Game thread, when no battle has run for a moment: a trial still on ends there.
  void idle();

  // Battle version for training, as the game counts them (39 = 5.03), 0 = the
  // current one. Takes effect the next time training loads; both sides get it.
  void setTrainingVersion(int version);
  int trainingVersion();
  // Game thread, in a battle: the battle version it runs on, as the game counts them
  // (an older one in replays, Combo Maker, or training on a picked one). 0 if unknown.
  int battleVersion();
  // Training: the oldest battle version both characters (yours and the dummy) were in
  // the game for; 0 if a character isn't known.
  int oldestVersion();
  // Game thread: the picked version is only taken up while no battle runs.
  void latchVersion(bool battle_running);
  // Online version, for Park and private room matches with a friend who picked the same
  // one. Set by hand (0 = off); taken up between fights only while in one of those lobbies,
  // and off when you go back to the main menu from a lobby or ranked matchmaking starts.
  void setOnlineVersion(int version);
  // One step through Off, then every version newest first (dir -1 or +1).
  void stepOnlineVersion(int dir);
  int onlineVersion();
  bool onlineVersionInUse();  // a fight runs on it right now
  int onlineVersionUsed();    // the version taken up for this fight (0 = none)
  uintptr_t saveRoot();       // the game's save data, 0 if not there yet
  uintptr_t trainingManager();  // training's manager (recipe, frame info), 0 if not found
  int playerChara();          // the character being played (training), -1 if none
  bool inTraining();          // the game's mode is training (the Park's too)
  int localSide();            // our side in the fight, 0 = P1
  bool onlineFight();         // the game's mode is the online one (Park, rooms; ranked to be confirmed)
  int opponentChara();        // the other side's character, -1 if none
  // Game thread, every system update: any fight running (online ones too).
  void updateOnline(bool any_battle_running);
}
