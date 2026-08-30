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
    // 3 purple, 4 orange. Zero is never stored; it means "no opinion".
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

    // Forgets everything. Used by the revert callback and available for a
    // console-driven reset while testing.
    void Clear();
}
