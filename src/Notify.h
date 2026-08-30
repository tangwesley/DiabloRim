// =============================================================================
//  Swallowing one HUD notification.
// =============================================================================
//  A quest reward announces itself the instant it lands -- "Skyforge Steel
//  Sword" in the corner -- and the roll does not arrive until a second later,
//  when the inventory has settled. The player therefore reads the item's plain
//  name first and its real one second, which gets the order of the story
//  backwards: the affix is the point, and the base name is the spoiler.
//
//  ★THIS IS A VTABLE HOOK, the only one in the project, and it is worth saying
//  why it is an acceptable risk when so much else here was not. It touches the
//  HUD's message handler and nothing else: no inventory, no extra data, no
//  created objects. The worst outcome is a notification that should have shown
//  and did not, which the player can recover from by looking in their pack. The
//  inventory work that cost this feature two crashes had no such ceiling.
//
//  Deliberately NOT a general "quiet mode". It swallows one specific text, once,
//  within a few seconds of being asked, and then disarms itself -- so a mod that
//  hands over a sword and a purse of gold in the same breath still announces the
//  gold.
// =============================================================================
#pragma once

#include <string_view>

namespace Notify
{
    // Hooks HUDMenu::ProcessMessage. Safe to call once, at kDataLoaded; a second
    // call does nothing.
    void Install();

    // Swallow the next HUD notification whose text CONTAINS a_text.
    //
    // ★CONTAINS, NOT EQUALS, and that is what makes one arming cover both
    // notifications this feature can produce. The hand-over announces "Skyforge
    // Steel Sword"; the same item after a roll announces "Nulling Skyforge Steel
    // Sword of Slaying". Passing the base name matches either, so the caller
    // does not have to predict which one it is racing.
    //
    // Expires by itself after a few seconds. An arming that never matches costs
    // nothing and disappears on its own -- which matters, because the caller
    // cannot know whether the engine was going to send the message at all.
    void MuteNext(std::string_view a_text);
}
