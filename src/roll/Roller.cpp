// =============================================================================
//  The roll. Pure -- see Roll.h.
// =============================================================================
//  Shape of a roll, in order:
//
//    1. how many affixes  -- interpolated from the item level against the count
//                            curve, so a mudcrab's drop and a boss's drop differ
//                            in COUNT before they differ in anything else
//    2. which affixes     -- weighted draw from the pool eligible for this
//                            item's slots, without replacement, so no item ever
//                            rolls the same attribute twice
//    3. which tier of each -- weighted by the tier curve, restricted to tiers
//                            the item level has unlocked
//    4. what value        -- uniform inside that tier's band
//
//  Item tier is the sum of the points, which is what makes it a power budget
//  rather than a label: 0-12 falls out of 0-4 affixes worth 1-3 each.
// =============================================================================

#include "Roll.h"

#include <algorithm>
#include <numeric>
#include <string>

namespace roll
{
    namespace
    {
        // Linear interpolation between the two bracketing bands. Below the first
        // band or above the last, the nearest band is used unchanged.
        // The member pointer is `std::array<int,N> Curve::*`, NOT
        // `const std::array<int,N>& Curve::*` -- there is no such thing as a
        // pointer to a reference member, and writing the const-ref form is an
        // easy way to get four cascading errors that name the wrong lines.
        template <std::size_t N>
        std::array<int, N> Interpolate(const std::vector<Curve>& a_bands, int a_level,
            std::array<int, N> Curve::*a_member)
        {
            if (a_bands.empty()) {
                return {};
            }
            if (a_level <= a_bands.front().level) {
                return a_bands.front().*a_member;
            }
            if (a_level >= a_bands.back().level) {
                return a_bands.back().*a_member;
            }

            for (std::size_t i = 1; i < a_bands.size(); ++i) {
                const auto& hi = a_bands[i];
                if (a_level > hi.level) {
                    continue;
                }
                const auto& lo = a_bands[i - 1];
                const int   span = hi.level - lo.level;
                const float t = span > 0 ? static_cast<float>(a_level - lo.level) / static_cast<float>(span) : 0.0f;

                std::array<int, N> out{};
                for (std::size_t k = 0; k < N; ++k) {
                    const float a = static_cast<float>((lo.*a_member)[k]);
                    const float b = static_cast<float>((hi.*a_member)[k]);
                    out[k] = static_cast<int>(a + (b - a) * t + 0.5f);
                }
                return out;
            }
            return a_bands.back().*a_member;
        }

        // Picks an index in proportion to its weight. Returns -1 when every
        // weight is zero, which is a real case -- an item level below every
        // affix's gate -- and must not be a crash.
        int WeightedPick(const std::vector<std::uint32_t>& a_weights, Rng& a_rng)
        {
            const std::uint64_t total = std::accumulate(a_weights.begin(), a_weights.end(), std::uint64_t{ 0 });
            if (total == 0) {
                return -1;
            }
            std::uniform_int_distribution<std::uint64_t> dist{ 0, total - 1 };
            std::uint64_t                                roll = dist(a_rng);
            for (std::size_t i = 0; i < a_weights.size(); ++i) {
                if (roll < a_weights[i]) {
                    return static_cast<int>(i);
                }
                roll -= a_weights[i];
            }
            return static_cast<int>(a_weights.size()) - 1;
        }

        template <std::size_t N>
        int WeightedPickArray(const std::array<int, N>& a_weights, Rng& a_rng)
        {
            std::vector<std::uint32_t> weights;
            weights.reserve(N);
            for (const int weight : a_weights) {
                weights.push_back(weight > 0 ? static_cast<std::uint32_t>(weight) : 0u);
            }
            return WeightedPick(weights, a_rng);
        }
    }

    Band BandOf(int a_tier) noexcept
    {
        if (a_tier <= 0) {
            return Band::kWhite;
        }
        if (a_tier <= 3) {
            return Band::kBlue;
        }
        if (a_tier <= 6) {
            return Band::kYellow;
        }
        if (a_tier <= 9) {
            return Band::kPurple;
        }
        return Band::kOrange;
    }

    const char* BandName(Band a_band) noexcept
    {
        switch (a_band) {
        case Band::kWhite:  return "white";
        case Band::kBlue:   return "blue";
        case Band::kYellow: return "yellow";
        case Band::kPurple: return "purple";
        case Band::kOrange: return "orange";
        }
        return "?";
    }

    // The tier marker, repeated once per band.
    //
    // ★ONE GLYPH, ONE PLACE, AND THE PLAYER PICKS IT. The default is written
    // as explicit UTF-8 bytes so the source file's encoding cannot change what
    // ships; the INI overrides it, because whether a glyph draws or comes out a
    // box depends on the player's fonts and menu replacers -- which is not
    // something this file can know.
    //
    // U+25C6 BLACK DIAMOND by default. Grid Inventory merges Segoe UI Symbol
    // across 0x2010-0x2BFF (UIRoot.cpp), so anything in that range renders
    // there. Vanilla's Scaleform menus are the risk: if a barter or container
    // screen draws a box, set TierMarker in the INI to a plain ASCII character
    // -- or to nothing at all, which drops the marker entirely.
    constexpr const char* kDefaultTierMark = "\xE2\x97\x86";

    // ★A COPY, not a string_view onto the caller's memory. What arrives is a
    // slice of a line parsed out of an INI, and that buffer is long gone by the
    // time the first item rolls.
    std::string& TierMarkStorage()
    {
        static std::string mark{ kDefaultTierMark };
        return mark;
    }

    bool& TierMarkInNameStorage() noexcept
    {
        // ★ON BY DEFAULT, because the name is currently the only surface that
        // can carry the marker at all.
        //
        // Drawing it on hover instead was tried and reverted: it wants a detour
        // on RE::ItemCard::SetItem, and CommonLibSSE's trampoline offers no way
        // to write one. Trampoline::write_branch<5> looks like the tool and is
        // not -- it PATCHES AN EXISTING CALL INSTRUCTION, decoding the four
        // bytes before its end as the displacement of the original target.
        // Aimed at a function's first byte it reads the prologue as a
        // displacement, hands back an address that is not code, and clobbers
        // the prologue on the way past. Measured, on a forge:
        // EXCEPTION_ACCESS_VIOLATION executing 0x7FF67556D1A0.
        //
        // Doing it properly needs the address-library id of a CALL SITE, which
        // is not something this file can derive.
        static bool inName = true;
        return inName;
    }

    // Built per call rather than cached, because the mark is no longer a
    // compile-time constant -- and this runs once per item card, which is
    // nowhere near hot enough to be worth a table that could go stale.
    //
    // ★THE BAND IS THE COUNT. Band::kBlue is 1, kOrange is 4, and the enum's
    // order is the repeat count -- so this is a cast rather than a switch, and
    // adding a band in the middle would change the marks without touching this
    // function. That is intended: the mark IS the band's ordinal.
    std::string BandMark(Band a_band)
    {
        const auto& mark = TierMarkStorage();
        const auto  count = static_cast<std::size_t>(a_band);

        std::string out;
        if (mark.empty() || count == 0) {
            return out;  // marker switched off, or a white item
        }
        out.reserve(mark.size() * count);
        for (std::size_t i = 0; i < count; ++i) {
            out += mark;
        }
        return out;
    }

    void SetTierMark(std::string_view a_mark)
    {
        TierMarkStorage().assign(a_mark);
    }

    std::string_view TierMark() noexcept
    {
        return TierMarkStorage();
    }

    void SetTierMarkInName(bool a_enabled) noexcept
    {
        TierMarkInNameStorage() = a_enabled;
    }

    bool TierMarkInName() noexcept
    {
        return TierMarkInNameStorage();
    }

    std::string RolledItem::Name(std::string_view a_baseName) const
    {
        // ★ONE FRAGMENT PER AFFIX. An affix defines both a prefix and a suffix,
        // but it may only ever contribute ONE of them -- otherwise a single-affix
        // item comes out as "Honed Iron Sword of Wielding", where both halves are
        // the same affix talking about itself twice.
        //
        // The shape follows Skyrim first and Diablo second: with one affix you
        // get "Iron Sword of Burning", exactly as vanilla names its own gear. A
        // prefix only appears once a SECOND affix is there to earn it, so the
        // name grows with the item rather than being uniformly ornate.
        const RolledAffix* forSuffix = nullptr;
        const RolledAffix* forPrefix = nullptr;

        for (const auto& rolled : affixes) {
            if (!rolled.affix) {
                continue;
            }
            if (!rolled.affix->suffix.empty() &&
                (!forSuffix || rolled.points > forSuffix->points)) {
                forSuffix = &rolled;
            }
        }

        for (const auto& rolled : affixes) {
            if (!rolled.affix || &rolled == forSuffix) {
                continue;  // already spoke
            }
            if (!rolled.affix->prefix.empty() &&
                (!forPrefix || rolled.points > forPrefix->points)) {
                forPrefix = &rolled;
            }
        }

        std::string out;
        if (forPrefix) {
            out += forPrefix->affix->prefix;
            out += ' ';
        }
        out += a_baseName;
        if (forSuffix) {
            out += ' ';
            out += forSuffix->affix->suffix;
        }

        // ★TIER, AS A REPEATED MARKER, TRAILING -- AND OPTIONAL.
        //
        // BAND, not raw tier. Twelve marks would be a wall of glyphs, and one
        // mark per band maps 1:1 onto white/blue/yellow/purple/orange.
        //
        // TRAILING, because a leading marker reorders every alphabetical item
        // list in the game.
        //
        // OPTIONAL, because a name is an expensive place to put it: the marker
        // joins the list's sort key, it is what a narrow barter column
        // truncates, and it goes into the save as a per-instance override. With
        // Grid Inventory installed the tint already carries the band and none
        // of that is worth paying -- see TierMarkInNameStorage for why the
        // cheaper surface, the item card, is not on offer.
        if (TierMarkInName()) {
            if (const std::string mark = BandMark(BandOf(tier)); !mark.empty()) {
                out += ' ';
                out += mark;
            }
        }
        return out;
    }

    RolledItem Roll(const AffixTable& a_table, const RollContext& a_ctx,
        const Tuning& a_tuning, Rng& a_rng)
    {
        RolledItem result;
        if (a_table.Empty() || a_ctx.itemSlots == kNone) {
            return result;
        }

        auto countWeights = Interpolate(a_tuning.bands, a_ctx.itemLevel, &Curve::countWeights);

        if (a_ctx.noWhite) {
            countWeights[0] = 0;

            // ★THE FALLBACK IS NOT DEAD CODE. The shipped curve offers a
            // non-zero weight at every band, so this cannot fire today -- but
            // the curve is data a user is invited to edit, and a band tuned to
            // "white or nothing" would otherwise leave every weight at zero.
            // WeightedPick has to return something at that point, and what it
            // returns is not this function's decision to leave to it.
            const int total = std::accumulate(countWeights.begin(), countWeights.end(), 0);
            if (total <= 0) {
                countWeights[1] = 1;
            }
        }

        const int wanted = WeightedPickArray(countWeights, a_rng);
        if (wanted <= 0) {
            return result;  // a plain white item, and that is a legitimate roll
        }

        const auto tierWeights = Interpolate(a_tuning.bands, a_ctx.itemLevel, &Curve::tierWeights);

        // Candidates: eligible for this slot, not excluded, and with at least one
        // tier the item level has unlocked.
        std::vector<const Affix*> pool;
        for (const auto& affix : a_table.Affixes()) {
            if ((affix.slots & a_ctx.itemSlots) == 0) {
                continue;
            }
            if (a_ctx.forNpc && affix.npcExclude) {
                continue;
            }
            const bool anyUnlocked = std::any_of(affix.tiers.begin(), affix.tiers.end(),
                [&](const AffixTier& a_tier) { return a_ctx.itemLevel >= a_tier.minItemLevel; });
            if (anyUnlocked) {
                pool.push_back(&affix);
            }
        }
        if (pool.empty()) {
            return result;
        }

        result.affixes.reserve(static_cast<std::size_t>(wanted));

        for (int picked = 0; picked < wanted && !pool.empty(); ++picked) {
            // Draw without replacement: an affix already on the item is removed
            // from the pool, which is the "no duplicate attributes" rule from
            // your design notes enforced structurally rather than by retrying.
            std::vector<std::uint32_t> weights;
            weights.reserve(pool.size());
            for (const auto* affix : pool) {
                // An affix's drop weight is taken from its lowest unlocked tier,
                // so weight stays a property of the affix rather than drifting
                // with which tier happens to roll.
                std::uint32_t weight = 0;
                for (const auto& tier : affix->tiers) {
                    if (a_ctx.itemLevel >= tier.minItemLevel) {
                        weight = tier.weight;
                        break;
                    }
                }
                weights.push_back(weight);
            }

            const int chosen = WeightedPick(weights, a_rng);
            if (chosen < 0) {
                break;
            }
            const Affix* affix = pool[static_cast<std::size_t>(chosen)];
            pool.erase(pool.begin() + chosen);

            // Which tier. Restricted to unlocked tiers, weighted by the curve --
            // and note the curve is indexed by TIER NUMBER, not by position, so
            // Paralysis skipping Tier I costs it nothing.
            std::vector<std::uint32_t> tierPick;
            std::vector<const AffixTier*> unlocked;
            for (const auto& tier : affix->tiers) {
                if (a_ctx.itemLevel < tier.minItemLevel) {
                    continue;
                }
                const std::size_t slot = static_cast<std::size_t>(tier.tier - 1);
                const int         weight = slot < tierWeights.size() ? tierWeights[slot] : 1;
                tierPick.push_back(weight > 0 ? static_cast<std::uint32_t>(weight) : 1u);
                unlocked.push_back(&tier);
            }
            if (unlocked.empty()) {
                continue;
            }

            const int tierIndex = WeightedPick(tierPick, a_rng);
            if (tierIndex < 0) {
                continue;
            }
            const AffixTier& tier = *unlocked[static_cast<std::size_t>(tierIndex)];

            // Value: uniform inside the band. min and max may both be negative
            // (cost reduction), so normalise rather than assuming min < max.
            const float lo = std::min(tier.minValue, tier.maxValue);
            const float hi = std::max(tier.minValue, tier.maxValue);
            std::uniform_real_distribution<float> valueDist{ lo, hi };

            RolledAffix rolled;
            rolled.affix = affix;
            rolled.tier = tier.tier;
            rolled.points = tier.points;
            rolled.value = (lo == hi) ? lo : valueDist(a_rng);
            rolled.duration = tier.duration;
            rolled.area = tier.area;

            result.tier += tier.points;
            result.affixes.push_back(rolled);
        }

        if (a_ctx.forNpc && result.tier > a_tuning.npcMaxTier) {
            // Trim from the weakest end so the ceiling costs the item its least
            // interesting affix rather than its best one.
            std::sort(result.affixes.begin(), result.affixes.end(),
                [](const RolledAffix& a_lhs, const RolledAffix& a_rhs) { return a_lhs.points < a_rhs.points; });
            while (result.tier > a_tuning.npcMaxTier && !result.affixes.empty()) {
                result.tier -= result.affixes.front().points;
                result.affixes.erase(result.affixes.begin());
            }
        }

        return result;
    }
}
