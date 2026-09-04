// =============================================================================
//  Wielder buffs -- the half of a weapon affix the weapon cannot deliver.
// =============================================================================
//  A weapon enchantment fires on HIT, at the thing that was hit. That is the
//  whole of what the engine does with it. So a Fortify One-Handed effect on a
//  sword would reach the enemy for zero seconds and the wielder never; the
//  item card would promise +30% and the swing would not have it.
//
//  This module keeps that promise from the other side. While an affixed weapon
//  is equipped, the actor holding it carries an ABILITY -- a constant-effect,
//  self-delivered spell built at runtime from the enchantment's wielder-facing
//  effects -- and loses it when the weapon comes off. The enchantment keeps the
//  effect too, so the card still reads correctly; on hit it is harmless.
//
//  ★NO STATE OF ITS OWN ACROSS THE SAVE. The abilities are runtime forms with
//  no record behind them, so a save names them and a load cannot find them --
//  the engine drops the dangling entry, which is the outcome wanted. Rather
//  than remembering who was given what, Reconcile reads the truth back from
//  the actor: which weapons are worn, which of our abilities are in the spell
//  list, and fixes the difference. The same routine serves equip, unequip and
//  post-load, and a missed event costs one wrong frame rather than a stuck
//  bonus.
// =============================================================================
#pragma once

namespace Wielder
{
    // Installs the equip sink. Call on kDataLoaded, after Apply has resolved
    // the effects -- the sink asks Apply which effects are ours.
    void Install();

    // Brings one actor's abilities in line with what they are wielding. Safe to
    // call at any time from the main thread; idempotent.
    void Reconcile(RE::Actor* a_actor);

    // The player and every loaded actor. For the post-load sweep, when no equip
    // event will fire for gear that is already on.
    void ReconcileLoaded();

    // Whether this enchantment carries anything a wielder should feel.
    [[nodiscard]] bool WantsWielder(const RE::EnchantmentItem* a_ench);
}
