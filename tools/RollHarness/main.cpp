// =============================================================================
//  Roll harness -- a console program, not part of the plugin.
// =============================================================================
//  Rolls a large sample at several item levels and prints what actually comes
//  out: the tier histogram, the colour-band split, how often each affix appears,
//  and how many items roll nothing at all.
//
//  This exists because loot balance is not a thing you can reason your way to.
//  You change a number, look at the distribution, and change it again -- and
//  doing that inside Skyrim costs a launch and a dungeon per attempt.
//
//    RollHarness [csv] [--samples N] [--seed N] [--levels 1,12,25,40] [--npc]
// =============================================================================

#include "roll/Roll.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
    struct Options
    {
        std::string      csv{ "data/DiabloLoot_affixes.csv" };
        int              samples{ 100000 };
        unsigned         seed{ 20260826 };
        std::vector<int> levels{ 1, 12, 25, 40 };
        bool             forNpc{ false };
    };

    std::vector<int> ParseLevels(const char* a_text)
    {
        std::vector<int> out;
        std::string      token;
        for (const char* p = a_text;; ++p) {
            if (*p == ',' || *p == '\0') {
                if (!token.empty()) {
                    out.push_back(std::atoi(token.c_str()));
                    token.clear();
                }
                if (*p == '\0') {
                    break;
                }
            } else {
                token += *p;
            }
        }
        return out;
    }

    void Bar(int a_count, int a_total, int a_width = 44)
    {
        const int filled = a_total > 0 ? (a_count * a_width) / a_total : 0;
        for (int i = 0; i < a_width; ++i) {
            std::fputc(i < filled ? '#' : ' ', stdout);
        }
    }

    // A representative spread of item slots, so the numbers reflect a mixed loot
    // stream rather than one archetype. Weapons and armor dominate real drops;
    // jewellery is rarer, and its affix pool is different enough to matter.
    struct SlotSample
    {
        const char*   label;
        std::uint32_t mask;
        int           share;
    };

    constexpr SlotSample kSlots[]{
        { "weapon", roll::kWeapon, 40 },
        { "body armor", roll::kArmor | roll::kBody, 25 },
        { "shield", roll::kArmor | roll::kShield, 10 },
        { "boots", roll::kArmor | roll::kFeet, 8 },
        { "gauntlets", roll::kArmor | roll::kHands, 7 },
        { "ring", roll::kRing, 6 },
        { "amulet", roll::kAmulet, 4 },
    };
}

int main(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--samples" && i + 1 < argc) {
            options.samples = std::atoi(argv[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            options.seed = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (arg == "--levels" && i + 1 < argc) {
            options.levels = ParseLevels(argv[++i]);
        } else if (arg == "--npc") {
            options.forNpc = true;
        } else if (!arg.empty() && arg[0] != '-') {
            options.csv = arg;
        }
    }

    roll::AffixTable         table;
    std::vector<std::string> errors;
    if (!table.Load(options.csv, errors)) {
        std::fprintf(stderr, "FAILED to load %s\n", options.csv.c_str());
        for (const auto& error : errors) {
            std::fprintf(stderr, "  %s\n", error.c_str());
        }
        return 1;
    }

    std::printf("affix table: %s\n", options.csv.c_str());
    std::printf("  %zu affixes, %zu tier rows\n", table.Affixes().size(), table.TierRowCount());
    if (!errors.empty()) {
        std::printf("  %zu validation complaint(s):\n", errors.size());
        for (const auto& error : errors) {
            std::printf("    %s\n", error.c_str());
        }
    }
    std::printf("  rolling %d items per level, seed %u%s\n\n",
        options.samples, options.seed, options.forNpc ? ", as NPC loot" : "");

    const roll::Tuning tuning;

    // Cumulative slot picker.
    int slotTotal = 0;
    for (const auto& slot : kSlots) {
        slotTotal += slot.share;
    }

    for (const int level : options.levels) {
        roll::Rng rng{ options.seed + static_cast<unsigned>(level) };

        std::array<int, 13>        tierCounts{};
        std::map<std::string, int> affixCounts;
        long long                  totalAffixes = 0;

        for (int i = 0; i < options.samples; ++i) {
            int pick = static_cast<int>(rng() % static_cast<unsigned>(slotTotal));
            std::uint32_t mask = kSlots[0].mask;
            for (const auto& slot : kSlots) {
                if (pick < slot.share) {
                    mask = slot.mask;
                    break;
                }
                pick -= slot.share;
            }

            const roll::RollContext ctx{ level, mask, options.forNpc };
            const auto              rolled = roll::Roll(table, ctx, tuning, rng);

            tierCounts[static_cast<std::size_t>(std::clamp(rolled.tier, 0, 12))]++;
            totalAffixes += static_cast<long long>(rolled.affixes.size());
            for (const auto& affix : rolled.affixes) {
                affixCounts[affix.affix->name]++;
            }
        }

        std::printf("================ item level %d ================\n", level);

        int peak = 0;
        for (const int count : tierCounts) {
            peak = std::max(peak, count);
        }

        std::array<int, 5> bands{};
        for (int tier = 0; tier <= 12; ++tier) {
            const int count = tierCounts[static_cast<std::size_t>(tier)];
            bands[static_cast<std::size_t>(roll::BandOf(tier))] += count;
            std::printf("  tier %2d %-7s %6d %5.1f%%  ", tier,
                roll::BandName(roll::BandOf(tier)), count,
                100.0 * count / options.samples);
            Bar(count, peak);
            std::printf("\n");
        }

        std::printf("  ---- colour bands ----\n");
        for (int b = 0; b < 5; ++b) {
            std::printf("    %-7s %6d  %5.1f%%\n", roll::BandName(static_cast<roll::Band>(b)),
                bands[static_cast<std::size_t>(b)], 100.0 * bands[static_cast<std::size_t>(b)] / options.samples);
        }
        std::printf("  mean affixes/item: %.2f\n", static_cast<double>(totalAffixes) / options.samples);

        std::vector<std::pair<std::string, int>> sorted{ affixCounts.begin(), affixCounts.end() };
        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a_lhs, const auto& a_rhs) { return a_lhs.second > a_rhs.second; });
        std::printf("  most common: ");
        for (std::size_t i = 0; i < sorted.size() && i < 5; ++i) {
            std::printf("%s(%d) ", sorted[i].first.c_str(), sorted[i].second);
        }

        // A handful of actual names. The distribution says whether the NUMBERS
        // are right; only reading the names says whether the ITEMS are.
        std::printf("\n  sample names:\n");
        roll::Rng          nameRng{ options.seed + 9973u + static_cast<unsigned>(level) };
        static const char* kBases[]{ "Iron Sword", "Steel Cuirass", "Elven Bow",
            "Gold Ring", "Leather Boots", "Orcish Shield" };
        for (int shown = 0, tries = 0; shown < 6 && tries < 400; ++tries) {
            const auto&             slot = kSlots[static_cast<std::size_t>(tries) % std::size(kSlots)];
            const roll::RollContext ctx{ level, slot.mask, options.forNpc };
            const auto              rolled = roll::Roll(table, ctx, tuning, nameRng);
            if (rolled.affixes.empty()) {
                continue;
            }
            std::printf("    T%-2d %-7s %s\n", rolled.tier,
                roll::BandName(roll::BandOf(rolled.tier)),
                rolled.Name(kBases[static_cast<std::size_t>(shown) % std::size(kBases)]).c_str());
            ++shown;
        }
        std::printf("\n  affixes never rolled: ");
        int never = 0;
        for (const auto& affix : table.Affixes()) {
            if (!affixCounts.count(affix.name)) {
                if (never < 6) {
                    std::printf("%s ", affix.name.c_str());
                }
                ++never;
            }
        }
        std::printf("%s(%d total)\n\n", never > 6 ? "... " : "", never);
    }

    return 0;
}
