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
//    * minus what the player picked up off the ground, which the engine reports
//      through that same event and in the same shape -- see the activation
//      ledger in the cpp, and the ★ below for why the event cannot say so
//    * only items a leveled list can also produce, so artifacts keep their own
//      identity (Apply::IsGeneric, and the note there is the reasoning)
//    * only single items, never stacks
//
//  ★A GROUND PICKUP IS A GRANT AS FAR AS THE EVENT IS CONCERNED, and the field
//  that looks like it says otherwise does not.
//
//  TESContainerChangedEvent carries a `reference` handle, and the obvious
//  reading -- "set when the item came from a world reference" -- is wrong: on a
//  pickup it arrives EMPTY. That was read the obvious way here, so every sword
//  taken off the ground was rolled as a reward. The proof is in the mod's own
//  log: Apply::ToNewInstance drops the item and picks it back up, and every
//  single roll produced one extra "grant" that the entry guards then refused --
//  affixed and ineligible moving in lockstep, one for one, across a session.
//  A pickup that reported its reference could not have reached the counter.
//
//  So the pickup has to be recognised elsewhere. TESActivateEvent fires first,
//  naming the world reference the player is about to take, and the ledger below
//  holds that base object for a moment so the grant that follows can be matched
//  to it and dropped.
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
    // Registers the container-changed and activate sinks. Call on kDataLoaded,
    // after the
    // affix table, the effect resolution AND Apply::IndexLeveledItems -- rolling
    // before the leveled index exists would read every reward as unique.
    void Install();

    // Borrowed; must outlive the session. Call before Install.
    void SetTable(const roll::AffixTable& a_table);

    void               SetEnabled(bool a_enabled);
    [[nodiscard]] bool Enabled();

    // Drops everything queued but not yet rolled, and the activation ledger
    // with it. Call on kPreLoadGame.
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
        std::uint64_t fromWorld{ 0 };    // picked up off the ground; see the cpp
        std::uint64_t notGear{ 0 };      // not a weapon or armour
        std::uint64_t stacked{ 0 };      // more than one at a time; see the cpp
        std::uint64_t fromCrafting{ 0 }; // smithing output, which is not a reward
        std::uint64_t notGeneric{ 0 };   // absent from every leveled list

        // ★THE REFUSALS ARE COUNTED APART, and the reason is in the cpp beside
        // InspectEntry: they are unrelated situations that happen to share an
        // outcome, and a reward that quietly did not roll is the one case where
        // the player needs the log to say which of them it was. The first two
        // and `gone` come from the entry guard; `alreadyEnchanted` comes from
        // the surgery itself, which is the only place that can see it.
        std::uint64_t questObject{ 0 };      // a quest owns an instance
        std::uint64_t favourited{ 0 };       // an instance carries a hotkey
        std::uint64_t alreadyEnchanted{ 0 }; // the drop drew an enchanted instance
        std::uint64_t gone{ 0 };             // no longer held once it settled
        std::uint64_t ineligible{ 0 };       // no affixable slot on the record
        std::uint64_t rolledWhite{ 0 };
        std::uint64_t affixed{ 0 };
        std::uint64_t attachFailed{ 0 }; // the engine declined to make a list
        std::uint64_t notInWorld{ 0 };   // granted or settled while the player had no cell; see the cpp
    };

    [[nodiscard]] Stats CurrentStats();
    void                LogStats();
}
