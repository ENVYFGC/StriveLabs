// Combo Maker's online combos, from training: search them and download one into the
// kept Combo Maker list (then Combo Trials lists it), through the game's own online
// requests, the same ones its Combo Maker search screen sends. Game thread only.
#pragma once
#include <string>
#include <vector>

namespace combo_online {
  struct Result {
    unsigned number = 0;
    int chara = -1, vs_chara = -1;
    int downloads = 0, likes = 0, clear_rate = 0;
    bool cleared = false, official = false;
    int platform = 0;
    int version = 0;              // battle version it was made on
    std::string author, comment;  // UTF-8
    // In the game's language, from its own texts: the title, the tags, and each step
    // with how it must connect ("[Hit]", "[Consecutive Hit]").
    std::wstring title, tags, stage;
    std::vector<std::wstring> steps;
    std::string creator_id;  // the creator's account, only to search their combos: never shown or logged
  };

  void init();  // finds what it needs in the game; logs what's missing
  bool available();
  // What the server filters on, like the game's search screen.
  struct Filter {
    int chara = -1;     // the combo's character, -1 = any
    int vs = -1;        // the opponent, -1 = any
    bool current = false;  // made on the current version only
    std::vector<int> tags;  // all of these (2 corner, 3 outside of corner, 4/5/6 0/50/100% gauge)
    std::string creator_id;  // only this player's combos, empty = anyone's
  };
  // Sort: 0 downloads, 1 likes, 2 newest, 3 clear rate. Page from 0.
  // Empty on start, else why not.
  std::wstring search(const Filter& f, int sort, int page);
  std::wstring download(const Result& r);  // into the kept list
  std::wstring tryIt(const Result& r);     // into training only, not kept
  bool busy();
  // Every system update: finishes requests. Returns true once when something finished,
  // with what to tell the player in `message`.
  bool update(std::wstring& message);
  const std::vector<Result>& results();
}
