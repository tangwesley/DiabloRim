// =============================================================================
//  Pricing -- what a rolled item is worth at a merchant's counter.
// =============================================================================
//  A rolled item's enchantment is created with no cost of its own, so the
//  engine values a red greatsword at exactly what it values the plain one.
//  This module multiplies the engine's answer by the item's colour band --
//  1.5x for blue up to 10x for red -- and it does so ONLY on the buying side
//  by default: what a merchant asks for one goes up, what a merchant pays for
//  one does not, because a loot mod that quietly inflates the player's gold
//  income is a different mod from this one. An INI switch turns the selling
//  side on for people who want it.
//
//  ★HOW: A HOOK ON THE ENGINE'S ITEM-VALUE FUNCTION, reached through its
//  callers. InventoryEntryData::GetValue is the one function every price in
//  the game goes through -- the item card, the barter list, the transaction.
//  It has no vtable, and detouring a function's first bytes needs a
//  disassembler to move them out of the way. Its CALL SITES are a different
//  matter: every direct call is five bytes -- E8 and a displacement -- and the
//  displacement identifies the target exactly, so the code segment is scanned
//  once at load for calls whose target is GetValue, and each one is redirected
//  here. The hook calls the real function and scales the answer. A site
//  another mod has already redirected is left alone, since its displacement
//  no longer points at GetValue -- their hook wins there, and the price at
//  that site is unscaled rather than broken.
//
//  Direction comes from the barter menu: while it is open and showing the
//  vendor's goods, a value is a buying price. Anything else -- the player's
//  side of the barter, the inventory card, a script asking -- is the selling
//  side, scaled only when the INI says so.
// =============================================================================
#pragma once

namespace Pricing
{
    // Scans for and patches the call sites. Call from SKSEPluginLoad, after
    // the trampoline is allocated; the settings it reads are consulted at
    // price time, not here, so it does not need the INI yet.
    void Install();
}
