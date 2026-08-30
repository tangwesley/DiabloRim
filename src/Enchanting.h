// =============================================================================
//  The arcane enchanter -- making affixed items enchantable.
// =============================================================================
//  An affixed item carries its affixes as an ExtraEnchantment, and to the
//  enchanting table that is indistinguishable from an item the player has
//  already enchanted: the menu refuses it. Every piece of loot this mod
//  produces is therefore locked out of enchanting entirely, which is a much
//  bigger tax than the affixes are worth.
//
//  ★THE FIX IS TO MAKE THE STATEMENT TRUE RATHER THAN TO ARGUE WITH IT. While
//  the enchanting menu is open the affix enchantment is DETACHED, so the item
//  the menu inspects genuinely is unenchanted -- by whatever test the menu
//  applies, now and in every runtime. Nothing is hooked and no address is
//  guessed; the thing every check reads is simply not there.
//
//  On close the affixes come back:
//
//    * untouched item  -- the same enchantment is reattached, and because it was
//      never released the created-object refcount never moved.
//    * item the player enchanted -- the two enchantments are MERGED into one new
//      created enchantment carrying both sets of effects, and the two originals
//      are released. This is the half that cannot be skipped: without it the
//      craft silently overwrites the affix enchantment and leaves the manager
//      counting a reference nothing holds, which is the save-load crash
//      Apply::Release exists to prevent.
//
//  ★NOTHING IS EVER PERSISTED WHILE DETACHED. Extra data reaches the save only
//  when a save is written, and Skyrim cannot write one with the crafting menu
//  open -- no manual save, no quicksave, no autosave trigger. A crash at the
//  table therefore costs nothing: the last save still has the affixes on it.
//
//  What this deliberately does NOT do: re-enchanting a merged item. Once the
//  player's own enchantment is on there the item is a player-enchanted item and
//  vanilla's one-enchantment rule applies to it, same as any other.
// =============================================================================
#pragma once

namespace Enchanting
{
    // Installs the furniture and crafting-menu sinks. Call on kDataLoaded, after
    // Persist is live -- the ownership test reads the tier map.
    //
    // ★THE FURNITURE EVENT IS THE ONE THAT MATTERS. Stripping on the crafting
    // menu's open event is TOO LATE, measured: the sub-menu builds its item list
    // during construction, and MenuOpenCloseEvent is not dispatched until the
    // menu already exists. Entering the enchanter fires a whole animation
    // earlier. The menu event is kept as a backstop for tables reached without
    // furniture, and as the thing that puts the affixes back on the way out.
    void Install();

    // Drops any detached affix enchantments without touching them. For
    // kPreLoadGame: every pointer held across a session names an extra list in
    // the world that is about to be replaced.
    void Forget();
}
