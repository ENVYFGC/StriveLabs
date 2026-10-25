#pragma once

#include "keybinds.h"

class Keybinds
{
public:
    static const int BUTTON_COUNT = binds::COUNT;

    // Aliases into the rebindable table (see keybinds.h); rebinding updates these too.
    int& TOGGLE_FRAMEBAR_BUTTON = binds::key[binds::FRAMEBAR];
    int& TOGGLE_HITBOX_BUTTON   = binds::key[binds::HITBOXES];
    int& PAUSE_BUTTON           = binds::key[binds::PAUSE];
    int& ADVANCE_BUTTON         = binds::key[binds::ADVANCE];
    int& TOGGLE_MENU_BUTTON     = binds::key[binds::MENU];
    int& TOGGLE_SAVE_BUTTON     = binds::key[binds::SAVE];
    int& TAKEOVER_P1_BUTTON     = binds::key[binds::TAKEOVER_P1];
    int& TAKEOVER_P2_BUTTON     = binds::key[binds::TAKEOVER_P2];
    int& TOGGLE_LOAD_BUTTON     = binds::key[binds::LOAD];
    int& MENU_UP_BUTTON         = binds::key[binds::UP];
    int& MENU_DOWN_BUTTON       = binds::key[binds::DOWN];
    int& MENU_LEFT_BUTTON       = binds::key[binds::LEFT];
    int& MENU_RIGHT_BUTTON      = binds::key[binds::RIGHT];

    int getButtonIndex(int input);
    void setButtonIndex(int index, bool val);
    bool setButtonState(int input, bool val);
    bool getButtonState(int input);
    void resetButton(int input);
    void resetButtons();
    void loadButtons();
    void checkBinds(bool await = false);

private:
    bool buttonStates[BUTTON_COUNT] = {};
};
