#include "keybindings.h"

#include "bind_watcher.h"

int Keybinds::getButtonIndex(int input)
{
    if (input == 0) return -1;
    for (int i = 0; i < BUTTON_COUNT; i++)
    {
        if (binds::key[i] == input) return i;
    }
    return -1;
}

void Keybinds::setButtonIndex(int index, bool val)
{
    if (index >= 0 && index < BUTTON_COUNT) buttonStates[index] = val;
}

bool Keybinds::setButtonState(int input, bool val)
{
    int index = getButtonIndex(input);
    if (index == -1) return false;
    buttonStates[index] = val;
    return true;
}

bool Keybinds::getButtonState(int input)
{
    int index = getButtonIndex(input);
    return index != -1 && buttonStates[index];
}

void Keybinds::resetButton(int input)
{
    int index = getButtonIndex(input);
    if (index != -1) buttonStates[index] = false;
}

void Keybinds::resetButtons()
{
    for (int i = 0; i < BUTTON_COUNT; i++) buttonStates[i] = false;
}

void Keybinds::loadButtons()
{
    for (int i = 0; i < BUTTON_COUNT; i++) BindWatcherI::addToFilter(binds::key[i]);
}

void Keybinds::checkBinds(bool await)
{
    auto inputs = BindWatcherI::getInputs(await);
    for (const auto& input : inputs)
    {
        const bool repeat = (input & BindWatcherI::REPEAT) != 0;
        const int key = input & ~BindWatcherI::REPEAT;
        if (binds::capturing >= 0) { if (!repeat) binds::offerKey(key); }
        // Held keys repeat only for frame step and the menu arrows (hold to keep stepping or scrolling).
        else if (!repeat || key == ADVANCE_BUTTON || key == MENU_UP_BUTTON || key == MENU_DOWN_BUTTON
                 || key == MENU_LEFT_BUTTON || key == MENU_RIGHT_BUTTON)
            setButtonState(key, true);
    }
}
