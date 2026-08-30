// =============================================================================
//  Phase 5b -- quest rewards.
// =============================================================================
//  Distribute reaches actors and containers. It cannot reach the one place a
//  quest reward actually appears: the PLAYER'S inventory, which RollRef refuses
//  outright on purpose -- the player's gear is what they chose, and rolling it
//  on a cell load would rewrite the character's equipment.
//
//  So a reward is not skipped by the quest guards; it is never looked at. This
//  file is the missing path:
//
//    * one event, filtered to "created into your inventory from nothing", which
//      is what a script grant IS and what looting and buying are NOT
//    * only items a leveled list can also produce, so artifacts keep their own
//      identity (Apply::IsGeneric, and the note there is the reasoning)
//    * only single items, never stacks
//
//  ★THE ROLL LANDS ABOUT A SECOND AFTER THE ITEM DOES, and both halves of that
//  sentence are load-bearing.
//
//  Not on the event: TESContainerChangedEvent announces a change the engine has
//  not finished making, and acting inside it -- from a task, even -- cost two
//  crashes. The sink records a FormID and a timer does the work once the
//  inventory has stopped moving.
//
//  Not by conjuring per-instance data either: a script-granted item has no
//  ExtraDataList, and Apply.h records in detail what happens to anyone who tries
//  to manufacture one. Apply::ToNewInstance instead drops the item and picks it
//  straight back up, because a dropped reference comes with an ExtraDataList the
//  ENGINE built, and the pickup does the entry accounting the same way it does
//  for every item the player has ever picked up.
//
//  ★NOT PERSISTED, so the queue lives for a second and dies with the session.
//  Nothing outlives a load; Forget clears it.
// =============================================================================
#pragma once

#include <cstdint>

namespace roll { class AffixTable; }

namespace QuestReward
{
    // Registers the container-changed sink. Call on kDataLoaded, after the
    // affix table, the effect resolution AND Apply::IndexLeveledItems -- rolling
    // before the leveled index exists would read every reward as unique.
    void Install();

    // Borrowed; must outlive the session. Call before Install.
    void SetTable(const roll::AffixTable& a_table);

    void               SetEnabled(bool a_enabled);
    [[nodiscard]] bool Enabled();

    // Drops everything queued but not yet rolled. Call on kPreLoadGame.
    //
    // ★A QUEUED FormID BELONGS TO THE WORLD IT WAS QUEUED IN. Carrying one
    // across a load would roll an item in the save being opened because a
    // different save handed one over a second earlier -- and with created
    // enchantments and a co-save tier map on the other end of that, it is not a
    // cosmetic mistake.
    void Forget();

    struct Stats
    {
        // Everything the filter let through to a decision, and where each one
        // went. These close the same way Distribute's do: granted must equal the
        // sum of the rest.
        std::uint64_t granted{ 0 };
        std::uint64_t notGear{ 0 };      // not a weapon or armour
        std::uint64_t stacked{ 0 };      // more than one at a time; see the cpp
        std::uint64_t fromCrafting{ 0 }; // smithing output, which is not a reward
        std::uint64_t notGeneric{ 0 };   // absent from every leveled list
        std::uint64_t ineligible{ 0 };   // quest object, favourited, no slot
        std::uint64_t gone{ 0 };         // no longer held by the time it settled
        std::uint64_t rolledWhite{ 0 };
        std::uint64_t affixed{ 0 };
        std::uint64_t attachFailed{ 0 }; // the engine declined to make a list
    };

    [[nodiscard]] Stats CurrentStats();
    void                LogStats();
}
