// =============================================================================
//  Grid Inventory tint -- telling another mod's UI what an item rolled.
// =============================================================================
//  Grid Inventory draws the inventory; this mod decides what an item is worth.
//  Neither can do the other's job, so the two meet over GridInventoryAPI.h: the
//  grid asks "what tier is this unit?" while it draws, and gets back a number
//  it colours the item's name and rarity wedge with -- and asks again, while a
//  tooltip is built, for anything else we want said about that unit.
//
//  THREE TABLES, AND NONE IS THE PROVIDER ONE. The host keeps ONE provider
//  slot -- badges, tooltip lines and drop routing all ride it -- and a socket
//  mod may already hold it. Tinter, Annotator and Pricer are separate slots
//  with separate handshakes, so colouring items, annotating them and pricing
//  them costs nobody else their badges.
//
//  THE PRICER IS HERE BECAUSE THE HOOK IN Pricing.cpp CANNOT REACH THE GRID'S
//  SHOP. That hook patches the call sites of the engine's item-value routine
//  inside SkyrimSE.exe; the host's shop window calls that routine from its own
//  DLL through an indirect call, and hides the vanilla barter menu the hook
//  reads direction from. So the host asks us what a unit is worth on its
//  counter, and we answer with the same band multiples the vanilla menus get.
//
//  AND THE PROVIDER'S OWN TOOLTIP HOOK COULD NOT HAVE DONE IT ANYWAY. That one
//  is keyed by ItemKey, whose uid is ExtraUniqueID -- 0 for every renamed or
//  enchanted unit, which is every item we have ever touched. Annotator is
//  handed the ExtraDataList, exactly as the tint query is. See the Annotator
//  note in api/GridInventoryAPI.h.
//
//  AND THE GRID ASKS US, rather than us pushing state at it. Our tier is
//  per-INSTANCE and an instance has no name the two sides can share: ItemKey's
//  uid is ExtraUniqueID, which the engine never assigns to a merely renamed or
//  enchanted list -- which is to say, to any item we have touched. So the host
//  hands over the ExtraDataList pointer it is already holding, and we read the
//  ExtraEnchantment we put there ourselves.
//
//  Absent Grid Inventory this file does nothing at all: no host announcement
//  arrives, we never dispatch, and nothing else in the mod notices.
// =============================================================================
#pragma once

namespace GridTint
{
    // Registers the ABI listener. Call once, from SKSEPluginLoad.
    //
    // A SECOND listener, and it must NOT be the lifecycle one. Message type
    // numbers live in the SENDER's namespace, so on an unfiltered listener
    // another plugin's "type 4" is indistinguishable from kPostLoadGame. The
    // lifecycle handler stays filtered to sender "SKSE"; this one takes every
    // sender and acts only on Grid Inventory's 4CC types.
    void Install();

    // Whether the host accepted our table. False when Grid Inventory is not
    // installed, which is the ordinary case and not an error.
    [[nodiscard]] bool Registered();
}
