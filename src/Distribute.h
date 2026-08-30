// =============================================================================
//  Phase 5 -- distribution.
// =============================================================================
//  The only code in this project that runs unattended, on every actor the game
//  loads. That changes the cost of a mistake: a bug here is not one bad item,
//  it is every item in a playthrough, discovered hours later and baked into a
//  save. Three things follow from that and are worth stating rather than
//  discovering:
//
//    * an off switch that works at runtime, so a bad build can be stopped
//      without quitting to desktop
//    * a hard cap on work per actor, so a pathological inventory cannot stall
//      a cell load
//    * timing, always on, because "is this fast enough" is a measurement and
//      the answer changes with the load order
// =============================================================================
#pragma once

#include <cstdint>

namespace roll { class AffixTable; }

namespace Distribute
{
    // Registers the actor-loaded sink. Call on kDataLoaded, once the affix
    // table and effect resolution are done -- rolling before those exist would
    // mark actors as rolled while producing nothing.
    void Install();

    // Borrowed; must outlive the session. Call before Install.
    void SetTable(const roll::AffixTable& a_table);

    // Runtime kill switch. main turns this on at kDataLoaded, once the affix
    // table has loaded and its effects resolve -- rolling before that would mark
    // actors as done while producing nothing.
    void SetEnabled(bool a_enabled);
    [[nodiscard]] bool Enabled();

    // ★Rolls every actor CURRENTLY LOADED, respecting the already-rolled set.
    //
    // TESObjectLoadedEvent only fires when a reference LOADS. Actors already
    // standing in the world when the mod is installed -- or when distribution is
    // switched on -- never fire it, and stay vanilla until they unload and come
    // back. That is the whole "existing saves get loot retroactively" case, so
    // the sweep runs on game load and whenever distribution is enabled.
    void SweepLoaded();

    struct Stats
    {
        std::uint64_t actorsSeen{ 0 };
        std::uint64_t actorsRolled{ 0 };
        std::uint64_t itemsAffixed{ 0 };
        std::uint64_t skippedAlreadyRolled{ 0 };
        std::uint64_t skippedIneligible{ 0 };
        // ★These two exist so the item accounting CLOSES. Without them an item
        // that passed eligibility but produced nothing -- an enchanted base with
        // no plain template to swap to -- vanished from the stats entirely, and
        // "85 affixed" gave no hint how many were examined to get there.
        std::uint64_t itemsExamined{ 0 };
        std::uint64_t rolledWhite{ 0 };
        // Items skipped ONLY because they had no ExtraDataList to hang an
        // enchantment on. This number decides whether constructing one is worth
        // the risk: small means container loot is already reachable, large means
        // the safe path covers almost nothing.
        std::uint64_t wantedExtraList{ 0 };
        // Containers are counted apart from actors: the two reach the roller by
        // completely different paths, and lumping them together is what hid the
        // fact that containers were never arriving at all.
        std::uint64_t containersOpened{ 0 };
        std::uint64_t containersRolled{ 0 };
        // Vanilla-enchanted items, skipped by design.
        std::uint64_t skippedEnchanted{ 0 };
        // Actors reached before their inventory existed. NOT marked, so they get
        // another chance once they are in high process carrying real gear.
        std::uint64_t deferredNoGear{ 0 };
        // Of those, how many HAD an inventory but nothing flagged as worn. That
        // split says whether the actor is too far out to have an inventory at
        // all, or close enough to have one but not yet wearing it.
        std::uint64_t deferredNotWornYet{ 0 };
        std::uint64_t totalMicros{ 0 };
        std::uint64_t worstMicros{ 0 };
    };

    [[nodiscard]] Stats CurrentStats();
    void                LogStats();
}
