// =============================================================================
//  Phase 5 -- the persistence spine.
// =============================================================================
//  The set of actors whose gear has already been rolled, carried in the SKSE
//  co-save. Without it every cell reload re-rolls the same bandit, and the
//  created-objects block grows for as long as the save lives.
//
//  This is the piece that can damage a save rather than merely misbehave, so
//  three rules hold throughout:
//
//    * every FormID crossing the save boundary goes through ResolveFormID.
//      A raw FormID's high byte is the plugin's load-order INDEX, so a save
//      reopened after the user reorders their mods would otherwise mark the
//      wrong actors -- or mark forms that are now something else entirely.
//    * the revert callback CLEARS. Loading save B while holding save A's set
//      would silently skip every actor A had already rolled.
//    * a record whose version we do not recognise is skipped loudly, never
//      guessed at. Reading a future layout as if it were this one is how a
//      co-save turns into corruption.
// =============================================================================
#pragma once

#include <cstdint>

namespace Persist
{
    // Registers the serialization callbacks. Call once, from SKSEPluginLoad --
    // not from a message handler, since the interface must be claimed before
    // the first save or load can happen.
    void Install();

    // Marks an actor rolled. Returns true if this call is what marked it, so
    // the caller can use it as a test-and-set: rolling exactly once per actor
    // without a separate lookup racing the insert.
    bool MarkRolled(RE::FormID a_actor);

    [[nodiscard]] bool        WasRolled(RE::FormID a_actor);
    [[nodiscard]] std::size_t RolledCount();

    // Drops one mark, so the reference is eligible again. Returns true if there
    // was one to drop.
    //
    // ★THE ONLY WAY BACK OUT OF THE SET, and it exists for one reason: Skyrim
    // RESETS references. A dungeon respawns on its encounter zone's timer, the
    // chest's contents are regenerated from scratch, and the reference keeps
    // the FormID it always had -- so the mark, which is keyed on exactly that,
    // outlives the loot it was describing. Without this, a chest is Diablo loot
    // once and vanilla loot for the rest of the save, which is backwards: the
    // dungeon can be rerun, so its rewards should be rollable again.
    //
    // Called from the engine's own reset notification rather than a timer. An
    // interval guessed short would re-stock a container that never actually
    // reset, and that duplication compounds for as long as the save lives;
    // TESResetEvent cannot be wrong about whether the reset happened.
    bool ForgetRolled(RE::FormID a_actor);

    // ---- affix band, keyed by the created enchantment ---------------------
    //
    // What an item rolled, in a form another mod's UI can ask about while it
    // draws. Kept HERE rather than beside the tint code because it is exactly
    // the same kind of thing as the rolled-actor set: state that must survive
    // the save, remap on load, and be dropped when the world is replaced.
    //
    // Keyed on the CREATED ENCHANTMENT, which is the only per-instance handle
    // that survives the journey. The base form is shared by every copy of the
    // item, and an ExtraDataList pointer is rewritten by the engine on every
    // container move -- neither can name one sword among three.
    //
    // The band is roll::Band as an integer: 0 white, 1 blue, 2 yellow,
    // 3 purple, 4 orange, 5 red. Zero is never stored; it means "no opinion".
    void NoteEnchTier(RE::FormID a_enchantment, std::uint8_t a_band);

    // 0 when nothing was recorded -- an unrolled item, a vanilla enchantment,
    // or a save written before this record existed.
    //
    // HOT PATH: called once per visible inventory tile per frame by the grid.
    [[nodiscard]] std::uint8_t EnchTier(RE::FormID a_enchantment);

    // Drops one mapping. Pairs with Apply::Release, which destroys the
    // enchantment the mapping is keyed on.
    void ForgetEnchTier(RE::FormID a_enchantment);

    [[nodiscard]] std::size_t EnchTierCount();

    // ---- "this enchantment is ours", keyed by the created enchantment -------
    //
    // The set of created enchantments Apply built and attached. It exists for
    // exactly one reader: the enchanting table, which has to know whether the
    // enchantment on an item is an affix set it may detach and put back, or
    // somebody else's work it must leave alone.
    //
    // ★A SECOND SET RATHER THAN A FLAG ON THE TIER MAP, because the two do not
    // agree about merged items. When the player enchants an affixed item the
    // affixes are folded into a new enchantment that keeps the BAND -- it is
    // still coloured as the roll it came from -- but is NOT ours to strip: it
    // carries the player's own effects, and stripping it would let the table
    // enchant the item twice. The tier map says "band 3"; this set says "no".
    //
    // Why a record at all, when the enchantment's shape used to be enough: the
    // shape was "override flag set with cost zero", which is precisely what
    // finite weapon charge takes away. The structural half -- every effect on
    // it is one the affix table can produce -- still stands, and Enchanting
    // requires it as well as this set; a stale entry whose id the engine has
    // handed to some other created object cannot match on its own.
    //
    // Same remap, same revert, same reuse hazard as the tier map, and the same
    // answer to it: an entry is dropped when its enchantment is destroyed.
    void NoteAffixEnch(RE::FormID a_enchantment);

    [[nodiscard]] bool IsAffixEnch(RE::FormID a_enchantment);

    void ForgetAffixEnch(RE::FormID a_enchantment);

    [[nodiscard]] std::size_t AffixEnchCount();

    // ---- vendor restock day, keyed by the merchant chest ---------------------
    //
    // The faction's lastDayReset as it stood when the chest was last rolled.
    // A merchant chest is refilled by the engine on its own schedule with no
    // event to say so; comparing the day now to the day recorded is how
    // Distribute knows the stock is new. Same remap and revert as the rest.
    void NoteVendorDay(RE::FormID a_chest, std::uint32_t a_day);

    // 0 when nothing was recorded.
    [[nodiscard]] std::uint32_t VendorDay(RE::FormID a_chest);

    [[nodiscard]] std::size_t VendorDayCount();

    // Forgets everything. Used by the revert callback and available for a
    // console-driven reset while testing.
    void Clear();
}
