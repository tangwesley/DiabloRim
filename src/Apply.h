// =============================================================================
//  Phase 4 -- turning a roll into a live item.
// =============================================================================
//  This is the boundary. Everything under src/roll/ is pure logic that knows
//  nothing about Skyrim; this file is the only place that turns its output into
//  forms, extra data and enchantments. The dependency runs one way: Apply
//  includes Roll, never the reverse.
//
//  Every step here is something Phase 0 measured rather than assumed:
//
//    * the created ENCH persists across a real restart, and the engine dedupes
//      identical effect sets, refcounting the sharing itself
//    * charge 0 fires from empty and draws no meter
//    * ExtraEnchantment is inert when the record carries an EITM, which is why
//      enchanted bases get swapped for their plain template
//    * detaching with RemoveByType does NOT release the reference, and that is
//      what crashed two saves
// =============================================================================
#pragma once

#include "roll/Roll.h"

#include <string>

namespace Apply
{
    // Resolves every "plugin|0xLOCALID" token in the table to a live
    // EffectSetting, once, and complains loudly about the ones that fail. Call
    // on kDataLoaded, after the table has loaded. Returns how many resolved.
    std::size_t ResolveEffects(const roll::AffixTable& a_table);

    // Indexes every weapon and armour reachable from a leveled item list, once,
    // at kDataLoaded. Returns how many distinct base records were found.
    //
    // ★THIS IS THE DEFINITION OF "NON-UNIQUE", and it is DERIVED FROM THE LOAD
    // ORDER rather than authored here. Every Elven Sword in the game arrives
    // through a leveled list; no artifact ever does. So "appears in at least one
    // LVLI" separates generic gear from named gear with no keyword to invent, no
    // name to pattern-match, and nothing to patch when a mod adds either kind --
    // a mod that puts its swords in the vanilla lists is covered the moment it
    // is installed, and one that adds a unique is excluded for the same reason.
    //
    // Only the quest-reward path consults this. The actor and container paths do
    // not need it: what a bandit is wearing came from a leveled list already.
    std::size_t IndexLeveledItems();

    // Whether this base record appears in a leveled item list -- one of the
    // many rather than one of the one. False until IndexLeveledItems has run,
    // which is why that runs at kDataLoaded and this is only asked afterwards.
    [[nodiscard]] bool IsGeneric(const RE::TESBoundObject* a_object);

    [[nodiscard]] std::size_t GenericCount();

    // Whether this magic effect is one the affix table can produce.
    //
    // ★THIS IS THE STRUCTURAL HALF OF "IS THIS ENCHANTMENT OURS". An enchantment
    // every one of whose effects came from our own table, carrying the
    // never-drain signature this file stamps, is ours whatever any bookkeeping
    // says -- and unlike a FormID recorded in the co-save, it cannot be lost by
    // an older build, a format change, or a created-object id the engine
    // renumbered. The enchanting table needs that certainty: a false negative
    // there is an item the player cannot enchant.
    [[nodiscard]] bool IsAffixEffect(const RE::EffectSetting* a_effect);

    // What an item can carry, as a roll::Slot mask. kNone means "not something
    // this system affixes".
    [[nodiscard]] std::uint32_t SlotsOf(RE::TESBoundObject* a_object);

    // Whether affixing this item is safe. Quest items and anything already
    // carrying an alias instance are refused: the base swap changes an item's
    // identity, and a quest that hands over a specific record and later checks
    // for it would break.
    [[nodiscard]] bool IsEligible(RE::TESBoundObject* a_object, RE::ExtraDataList* a_xList);

    struct Applied
    {
        RE::EnchantmentItem* enchantment{ nullptr };
        std::string          name;

        explicit operator bool() const { return enchantment != nullptr; }
    };

    // Builds the enchantment for a roll and attaches it to one item instance.
    // a_object is the item's base form and a_xList its per-instance data.
    //
    // Vanilla-enchanted items are declined outright: ExtraEnchantment is inert
    // when the record carries an EITM, and swapping the base is live inventory
    // surgery whose payoff is decoration on items that already have character.
    Applied ToItem(const roll::RolledItem& a_rolled, RE::TESBoundObject* a_object,
        RE::ExtraDataList* a_xList);

    // The same thing for an item with NO per-instance data yet -- a quest reward
    // handed straight over by a script, which is the one case ToItem cannot
    // serve because there is nothing to give it.
    //
    // ★THE ITEM IS DROPPED AND PICKED BACK UP, and that is not a hack for want
    // of a better idea -- it is the only route where the ENGINE builds both the
    // ExtraDataList and the inventory entry, which is precisely what everything
    // else got wrong.
    //
    // A dropped reference is a real TESObjectREFR with an ExtraDataList of its
    // own, made by the engine at the moment of the drop. The enchantment goes
    // onto that with SetEnchantment -- the same call Distribute makes for every
    // affixed item in the save. Actor::PickUpObject then performs the engine's
    // ordinary pickup, moving the reference and its extra data into the
    // inventory and doing the entry accounting itself. Nothing here constructs
    // an entry, a count, or an extra list.
    //
    // ★RECORDED SO IT IS NOT REDISCOVERED: what was tried before this, and what
    // it did. Measured over four builds in a live game.
    //
    //   InventoryChanges::EnchantObject(obj, nullptr, ench, 0) -- the arcane
    //   enchanter's own function, which does create a list and does put the
    //   enchantment on it, but leaves the ENTRY inconsistent. The player saw two
    //   rows for one sword: a plain one, and an enchanted one whose item card
    //   listed no effect. Removing "the duplicate" left ZERO swords, which is
    //   the measurement that settles it -- there was only ever one item, and the
    //   second row was the entry disagreeing with itself.
    //
    //   InventoryChanges::GetItemCount -- added to reconcile that count, and the
    //   only engine call present in both crashing builds and absent from the one
    //   that ran clean. Removing it removed the crashes. Do not call it here.
    //
    // a_refr must be an Actor whose inventory holds the item.
    Applied ToNewInstance(const roll::RolledItem& a_rolled, RE::TESBoundObject* a_object,
        RE::TESObjectREFR* a_refr);

    // Releases a created enchantment previously attached by ToItem, decrementing
    // the manager's refcount rather than merely unhooking the extra data.
    //
    // ★Use this and never RemoveByType alone. Detaching without releasing leaves
    // the manager counting a reference nothing holds, the save records it, and
    // the load throws.
    void Release(RE::ExtraDataList* a_xList, bool a_isWeapon);
}
