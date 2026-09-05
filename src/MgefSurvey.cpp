// =============================================================================
//  Magic-effect survey -- Phase 1 scoping.
// =============================================================================
//  Phase 1 says "author ~50 magic effects, only for affixes with no vanilla
//  equivalent," and guesses that roughly two-thirds of the list already exists.
//  That guess decides how much Creation Kit work the project needs, so it is
//  worth measuring instead: the base-swap survey turned the same kind of
//  assumption into a number and changed the plan.
//
//  For each affix in the table this walks every EffectSetting in the load order
//  and reports the ones that could implement it -- matched on what the engine
//  actually keys off (archetype, primary actor value, resistance) rather than on
//  names, which mods rewrite freely.
//
//  The output is a work list: which affixes are free, and which must be authored.
// =============================================================================

#include "PCH.h"

#include "MgefSurvey.h"

#include "Config.h"

#include "roll/Roll.h"

#include <algorithm>
#include <format>
#include <map>
#include <string>
#include <fstream>
#include <set>
#include <vector>

namespace
{
    using Archetype = RE::EffectSetting::Archetype;
    using AV = RE::ActorValue;

    constexpr auto kAnyAV = static_cast<AV>(-2);  // "do not test this field"

    // ★Detrimental is a THREE-state test, not a bool.
    //
    // The first version of this table demanded detrimental==true for Paralysis,
    // Banish and Turn Undead, and all three reported NEEDS AUTHORING even though
    // the spike had already harvested a working Turn Undead effect out of a
    // vanilla weapon enchantment. Asserting a flag whose value you have not
    // checked turns a survey into a way of confirming your own guesses.
    enum class Harm
    {
        kEither,
        kYes,
        kNo
    };

    // What would have to be true of a magic effect for it to implement a given
    // affix. Keyed on the affixId column of the CSV.
    //
    // ★avAlternates exists because of the single biggest error in the first run:
    // Skyrim's Fortify enchantments do NOT modify the bare skill actor value.
    // Fortify One-Handed keys off kOneHandedPowerModifier, not kOneHanded. The
    // first table asserted the bare values, so eleven of the most common
    // enchantments in the game reported as missing, and the spell-cost rows
    // matched unrelated effects that merely tag the school.
    struct Expectation
    {
        const char* affixId;
        Archetype   archetype;
        AV          primaryAV;
        AV          alt1{ kAnyAV };
        AV          alt2{ kAnyAV };
        AV          resist{ AV::kNone };  // kNone unless the effect is elemental
        Harm        harm{ Harm::kEither };
        const char* note{ "" };
    };

    constexpr Expectation kExpectations[]{
        // --- fortify-style value modifiers ------------------------------------
        // Each names the bare skill AV plus the Modifier/PowerModifier variants,
        // because vanilla Fortify uses the latter and mods use either.
        { "health", Archetype::kValueModifier, AV::kHealth },
        { "magicka", Archetype::kValueModifier, AV::kMagicka },
        { "stamina", Archetype::kValueModifier, AV::kStamina },
        { "carry_weight", Archetype::kValueModifier, AV::kCarryWeight },
        { "health_regen", Archetype::kValueModifier, AV::kHealRate, AV::kHealRateMult },
        { "magicka_regen", Archetype::kValueModifier, AV::kMagickaRate, AV::kMagickaRateMult },
        { "stamina_regen", Archetype::kValueModifier, AV::kStaminaRate, AV::kStaminaRateMult },
        { "one_handed", Archetype::kValueModifier, AV::kOneHanded, AV::kOneHandedModifier, AV::kOneHandedPowerModifier },
        { "two_handed", Archetype::kValueModifier, AV::kTwoHanded, AV::kTwoHandedModifier, AV::kTwoHandedPowerModifier },
        { "archery", Archetype::kValueModifier, AV::kArchery, AV::kMarksmanModifier, AV::kMarksmanPowerModifier },
        { "block", Archetype::kValueModifier, AV::kBlock, AV::kBlockModifier, AV::kBlockPowerModifier },
        { "heavy_armor", Archetype::kValueModifier, AV::kHeavyArmor, AV::kHeavyArmorModifier, AV::kHeavyArmorPowerModifier },
        { "light_armor", Archetype::kValueModifier, AV::kLightArmor, AV::kLightArmorModifier, AV::kLightArmorPowerModifier },
        { "sneak", Archetype::kValueModifier, AV::kSneak, AV::kSneakingModifier, AV::kSneakingPowerModifier },
        { "lockpicking", Archetype::kValueModifier, AV::kLockpicking, AV::kLockpickingModifier, AV::kLockpickingPowerModifier },
        { "pickpocket", Archetype::kValueModifier, AV::kPickpocket, AV::kPickpocketModifier, AV::kPickpocketPowerModifier },
        { "barter", Archetype::kValueModifier, AV::kSpeech, AV::kSpeechcraftModifier, AV::kSpeechcraftPowerModifier },
        { "alchemy", Archetype::kValueModifier, AV::kAlchemy, AV::kAlchemyModifier, AV::kAlchemyPowerModifier },
        { "smithing", Archetype::kValueModifier, AV::kSmithing, AV::kSmithingModifier, AV::kSmithingPowerModifier },
        { "armor", Archetype::kValueModifier, AV::kDamageResist },
        { "unarmed_damage", Archetype::kValueModifier, AV::kUnarmedDamage },

        // --- the two moved to apparel ------------------------------------------
        { "attack_speed", Archetype::kValueModifier, AV::kWeaponSpeedMult, kAnyAV, kAnyAV, AV::kNone, Harm::kEither,
            "vanilla ships no apparel enchantment for this" },
        { "critical_damage", Archetype::kValueModifier, AV::kCriticalChance, kAnyAV, kAnyAV, AV::kNone, Harm::kEither,
            "Skyrim exposes crit CHANCE, not crit DAMAGE" },

        // --- spell cost, per school --------------------------------------------
        // The bare school AV is the SKILL LEVEL; cost reduction rides the
        // modifier variants. Matching the bare value is what made the first run
        // pair destruction_cost with Friendly Fire and Instant Kill.
        { "alteration_cost", Archetype::kValueModifier, AV::kAlterationModifier, AV::kAlterationPowerModifier },
        { "conjuration_cost", Archetype::kValueModifier, AV::kConjurationModifier, AV::kConjurationPowerModifier },
        { "destruction_cost", Archetype::kValueModifier, AV::kDestructionModifier, AV::kDestructionPowerModifier },
        { "illusion_cost", Archetype::kValueModifier, AV::kIllusionModifier, AV::kIllusionPowerModifier },
        { "restoration_cost", Archetype::kValueModifier, AV::kRestorationModifier, AV::kRestorationPowerModifier },

        // --- resistances ---------------------------------------------------------
        { "fire_resist", Archetype::kValueModifier, AV::kResistFire },
        { "frost_resist", Archetype::kValueModifier, AV::kResistFrost },
        { "shock_resist", Archetype::kValueModifier, AV::kResistShock },
        { "magic_resist", Archetype::kValueModifier, AV::kResistMagic },
        { "poison_resist", Archetype::kValueModifier, AV::kPoisonResist },
        { "disease_resist", Archetype::kValueModifier, AV::kResistDisease },
        { "poison_immunity", Archetype::kValueModifier, AV::kPoisonResist },
        { "disease_immunity", Archetype::kValueModifier, AV::kResistDisease },

        // --- weapon on-hit -------------------------------------------------------
        { "fire_damage", Archetype::kValueModifier, AV::kHealth, kAnyAV, kAnyAV, AV::kResistFire, Harm::kYes },
        { "frost_damage", Archetype::kValueModifier, AV::kHealth, kAnyAV, kAnyAV, AV::kResistFrost, Harm::kYes },
        { "shock_damage", Archetype::kValueModifier, AV::kHealth, kAnyAV, kAnyAV, AV::kResistShock, Harm::kYes },
        { "damage_magicka", Archetype::kValueModifier, AV::kMagicka, kAnyAV, kAnyAV, AV::kNone, Harm::kYes },
        { "damage_stamina", Archetype::kValueModifier, AV::kStamina, kAnyAV, kAnyAV, AV::kNone, Harm::kYes },
        { "absorb_health", Archetype::kAbsorb, AV::kHealth },
        { "absorb_magicka", Archetype::kAbsorb, AV::kMagicka },
        { "absorb_stamina", Archetype::kAbsorb, AV::kStamina },

        // --- archetype-defined, AV irrelevant, harm left UNASSERTED ---------------
        { "fear", Archetype::kDemoralize, kAnyAV },
        { "turn_undead", Archetype::kTurnUndead, kAnyAV },
        { "banish", Archetype::kBanish, kAnyAV },
        { "paralysis", Archetype::kParalysis, kAnyAV },
        { "soul_trap", Archetype::kSoulTrap, kAnyAV },
        { "waterbreathing", Archetype::kValueModifier, AV::kWaterBreathing },
        { "muffle", Archetype::kValueModifier, AV::kMovementNoiseMult, kAnyAV, kAnyAV, AV::kNone, Harm::kEither,
            "vanilla Muffle keys off MovementNoiseMult" },
    };

    // ★Affixes that are unambiguously a BONUS. A detrimental effect matching one
    // of these is not a near-miss -- it is the opposite of the intent.
    //
    // Measured: the first emitted map paired health_regen with "Damage Health
    // Regeneration" and stamina_regen with "Damage Stamina Regeneration". Both
    // are real, both are enchantment-proven, and both would have made the affix
    // reduce the stat it advertises. Nothing downstream would have caught it --
    // the item would simply have felt bad.
    constexpr const char* kMustBeBeneficial[]{
        "health", "magicka", "stamina", "carry_weight", "health_regen", "magicka_regen",
        "stamina_regen", "one_handed", "two_handed", "archery", "block", "heavy_armor", "light_armor",
        "sneak", "lockpicking", "pickpocket", "barter", "alchemy", "smithing", "armor",
        "unarmed_damage", "alteration_cost", "conjuration_cost", "destruction_cost",
        "illusion_cost", "restoration_cost", "fire_resist", "frost_resist", "shock_resist",
        "magic_resist", "poison_resist", "disease_resist", "poison_immunity",
        "disease_immunity", "waterbreathing", "muffle",
    };

    bool MustBeBeneficial(const std::string& a_affixId)
    {
        return std::any_of(std::begin(kMustBeBeneficial), std::end(kMustBeBeneficial),
            [&](const char* a_id) { return a_affixId == a_id; });
    }

    // Where a form comes from, as a preference order. A generic effect out of
    // Skyrim.esm beats the same thing out of somebody's armour mod: it needs no
    // extra dependency and it is the one the whole ecosystem already patches.
    int SourceRank(RE::TESForm* a_form)
    {
        auto* file = a_form ? a_form->GetFile(0) : nullptr;
        if (!file) {
            return 9;
        }
        const std::string name{ file->GetFilename() };
        if (name == "Skyrim.esm") {
            return 0;
        }
        if (name == "Dawnguard.esm" || name == "HearthFires.esm" || name == "Dragonborn.esm") {
            return 1;
        }
        if (name.rfind("cc", 0) == 0) {
            return 2;  // Creation Club: official, but not everyone owns it
        }
        return 3;
    }

    const Expectation* ExpectationFor(const std::string& a_affixId)
    {
        for (const auto& expectation : kExpectations) {
            if (a_affixId == expectation.affixId) {
                return &expectation;
            }
        }
        return nullptr;
    }

    std::string SourceFile(RE::TESForm* a_form)
    {
        auto* file = a_form ? a_form->GetFile(0) : nullptr;
        return file ? std::string{ file->GetFilename() } : std::string{ "<runtime>" };
    }

    // Which magic effects are already wired into an enchantment, and of what
    // kind. An effect vanilla only ever uses on a potion is a weaker candidate
    // than one already proven on gear.
    enum class Usage
    {
        kUnused,
        kWeaponEnch,
        kApparelEnch,
        kBoth
    };

    const char* UsageName(Usage a_usage)
    {
        switch (a_usage) {
        case Usage::kUnused:      return "not used by any enchantment";
        case Usage::kWeaponEnch:  return "used by WEAPON enchantments";
        case Usage::kApparelEnch: return "used by APPAREL enchantments";
        case Usage::kBoth:        return "used by both";
        }
        return "?";
    }

    // How an effect is used, and by HOW MANY enchantments.
    //
    // ★The count is the useful signal. A generic effect like Fortify One-Handed
    // is referenced by dozens of enchantments; a one-off artifact effect like
    // "Ebony Mail Muffle" or "Red Eagle Turn Undead" by exactly one. Ranking on
    // it picks the workhorse over the curiosity without resorting to name
    // matching, which mods rewrite freely.
    struct UsageInfo
    {
        Usage kind{ Usage::kUnused };
        int   count{ 0 };

        // ★What timing vanilla actually ships this effect with, as histograms
        // rather than averages. Durations are discrete design choices -- 0, 1,
        // 30 -- so the MODE is the meaningful summary and a mean would invent a
        // value nobody authored. Keeping the whole histogram also shows when an
        // effect is used with several different durations, which is a signal
        // worth seeing rather than flattening.
        std::map<std::uint32_t, int> durations;
        std::map<std::uint32_t, int> areas;

        // The magnitude range the mod author actually shipped. Discovery uses it
        // to seed tier bands, so a generated affix starts from THEIR numbers
        // rather than from ours -- which is the whole reason to read the load
        // order instead of inventing values.
        float minMag{ 0.0f };
        float maxMag{ 0.0f };
        bool  sawMag{ false };
    };

    // The most common value, and whether it was unanimous.
    std::pair<std::uint32_t, bool> Mode(const std::map<std::uint32_t, int>& a_histogram)
    {
        std::uint32_t best = 0;
        int           bestCount = 0;
        int           total = 0;
        for (const auto& [value, count] : a_histogram) {
            total += count;
            if (count > bestCount) {
                bestCount = count;
                best = value;
            }
        }
        return { best, bestCount == total };
    }

    std::string Histogram(const std::map<std::uint32_t, int>& a_histogram)
    {
        std::string out;
        for (const auto& [value, count] : a_histogram) {
            if (!out.empty()) {
                out += ' ';
            }
            out += std::format("{}x{}", value, count);
        }
        return out;
    }

    std::map<RE::EffectSetting*, UsageInfo> BuildUsageMap(RE::TESDataHandler* a_handler)
    {
        std::map<RE::EffectSetting*, UsageInfo> usage;

        for (auto* ench : a_handler->GetFormArray<RE::EnchantmentItem>()) {
            if (!ench) {
                continue;
            }
            const bool weapon = ench->GetCastingType() == RE::MagicSystem::CastingType::kFireAndForget;
            for (auto* effect : ench->effects) {
                if (!effect || !effect->baseEffect) {
                    continue;
                }
                auto&      slot = usage[effect->baseEffect];
                const auto kind = weapon ? Usage::kWeaponEnch : Usage::kApparelEnch;
                slot.kind = (slot.kind == Usage::kUnused) ? kind : (slot.kind == kind ? slot.kind : Usage::kBoth);
                ++slot.count;
                ++slot.durations[effect->effectItem.duration];
                ++slot.areas[effect->effectItem.area];

                const float mag = effect->effectItem.magnitude;
                if (mag > 0.0f) {
                    slot.minMag = slot.sawMag ? (std::min)(slot.minMag, mag) : mag;
                    slot.maxMag = slot.sawMag ? (std::max)(slot.maxMag, mag) : mag;
                    slot.sawMag = true;
                }
            }
        }

        return usage;
    }
}

    // ★DISCOVERY: turn a third-party plugin's enchantment effects into affix rows.
    //
    // The mgef token has always accepted any plugin, so adding Summermyst affixes
    // was possible by hand -- but by hand means transcribing FormIDs out of
    // SSEEdit, which is the error-prone step every other emitter here exists to
    // remove. A mistyped token fails silently, as one affix that never rolls.
    //
    // Everything below is READ from the load order rather than invented:
    //
    //   token       the plugin and LOCAL id, so it survives a reorder
    //   slots       from how the mod itself uses the effect: weapon enchantments
    //               get WEAPON, apparel ones get the apparel slots
    //   duration    the mode across that mod's own enchantments
    //   magnitudes  the range the MOD AUTHOR shipped, cut into three tiers, so a
    //               generated affix starts from their balance and not from ours
    //
    // What is NOT read: points, weight, minItemLevel and the name fragments.
    // Those are design decisions, and the values below are a starting point that
    // wants editing rather than an answer.
    void EmitDiscovery(const std::map<RE::EffectSetting*, UsageInfo>& a_usage,
        const std::map<std::string, std::pair<RE::EffectSetting*, Usage>>& a_taken,
        const std::set<std::string>& a_baseIds)
    {
        // Effects the base table already uses. Emitting them again would produce
        // duplicate affixIds, and a merged add-on REPLACES a matching id -- so a
        // stray duplicate would quietly override a tuned vanilla affix.
        std::set<RE::EffectSetting*> alreadyUsed;
        for (const auto& [id, pick] : a_taken) {
            alreadyUsed.insert(pick.first);
        }

        std::map<std::string, std::vector<std::string>> byPlugin;
        std::size_t skipped = 0;
        std::size_t skippedScripted = 0;
        std::size_t skippedComponent = 0;
        std::size_t skippedFlat = 0;
        std::size_t skippedCollision = 0;

        for (const auto& [mgef, info] : a_usage) {
            if (SourceRank(mgef) <= 2) {
                continue;  // vanilla, DLC and Creation Club are the base table's job
            }
            auto* file = mgef->GetFile(0);
            if (!file) {
                continue;
            }
            if (alreadyUsed.contains(mgef)) {
                ++skipped;
                continue;
            }

            const char* rawName = mgef->GetName();
            if (!rawName || !*rawName) {
                ++skipped;
                continue;  // an unnamed effect cannot be given a sensible affixId
            }

            // SCRIPT ARCHETYPES ARE REFUSED, and this is the filter that makes
            // the output usable rather than a dump.
            //
            // Mods like Summermyst are largely script-driven: the behaviour lives
            // in a script attached to the ENCHANTMENT, and the effect magnitude is
            // a parameter that script reads. Lifting the bare effect onto our own
            // created enchantment leaves the script behind, so the affix either
            // does nothing or does something nobody balanced. Measured on one
            // load order: magnitudes ranging to 1500 on effects named Discharge,
            // Roulette and King of the Lost.
            if (mgef->data.archetype == Archetype::kScript) {
                ++skippedScripted;
                continue;
            }

            // Sub-components of a compound enchantment -- "Invisibility - Vs
            // Player", "Triptych: Attributes". They are halves of something, and
            // half an effect is not an affix.
            const std::string_view nameView{ rawName };
            if (nameView.find(" - ") != std::string_view::npos ||
                nameView.find(":") != std::string_view::npos) {
                ++skippedComponent;
                continue;
            }

            // affixId from the name: lowercase, runs of non-alphanumerics to '_'.
            std::string id;
            for (const char* p = rawName; *p; ++p) {
                const unsigned char ch = static_cast<unsigned char>(*p);
                if (std::isalnum(ch)) {
                    id += static_cast<char>(std::tolower(ch));
                } else if (!id.empty() && id.back() != '_') {
                    id += '_';
                }
            }
            while (!id.empty() && id.back() == '_') {
                id.pop_back();
            }
            if (id.empty()) {
                ++skipped;
                continue;
            }

            const bool  weapon = info.kind == Usage::kWeaponEnch || info.kind == Usage::kBoth;
            const char* slots = weapon ? "WEAPON" : "ARMOR|RING|AMULET";

            const auto durationPick = Mode(info.durations);
            const auto areaPick = Mode(info.areas);

            // Three tiers cut from the range the mod itself uses. An effect that
            // only ever appears at one magnitude has no range to split, so the
            // tiers step around it rather than pretending to a precision the
            // data does not have.
            // AN ID ALREADY IN THE BASE TABLE IS A HAZARD, not a duplicate.
            //
            // A merged add-on REPLACES a matching affixId, so a mod shipping its
            // own "Fire Damage" would silently overwrite the tuned vanilla row --
            // same name, different effect, different balance. The pointer-based
            // dedupe above cannot see this: the effects genuinely differ. Prefix
            // the id instead, so both survive.
            std::string finalId = id;
            if (a_baseIds.contains(id)) {
                finalId = "mod_" + id;
                ++skippedCollision;
            }

            // No usable range means no tiers. An effect that only ever ships at
            // one magnitude is a toggle in disguise; inventing a ladder around it
            // would fabricate three balance points from one data point.
            if (!info.sawMag || info.maxMag <= info.minMag) {
                ++skippedFlat;
                continue;
            }

            const float lo = info.minMag;
            const float hi = info.maxMag;
            const float step = (hi - lo) / 3.0f;

            const std::string token =
                std::format("{}|0x{:06X}", std::string{ file->GetFilename() }, mgef->GetLocalFormID());
            const std::string suffix = std::format("of {}", rawName);

            for (int tier = 1; tier <= 3; ++tier) {
                const float tierLo = lo + step * static_cast<float>(tier - 1);
                const float tierHi = lo + step * static_cast<float>(tier);
                const int   minLevel = (tier == 1) ? 1 : (tier == 2 ? 12 : 25);

                byPlugin[std::string{ file->GetFilename() }].push_back(
                    std::format("{},{},Modded,{},{},{:.0f},{:.0f},flat,{},{},{},40,{},{},0,,{}",
                        finalId, rawName, tier, tier, tierLo, tierHi, durationPick.first,
                        areaPick.first, slots, minLevel, token, suffix));
            }
        }

        if (byPlugin.empty()) {
            logger::info("discovery: no third-party enchantment effects found");
            return;
        }

        auto path = SKSE::log::log_directory();
        if (!path) {
            return;
        }
        *path /= "DiabloLoot_discovered.csv";
        std::ofstream out{ path->string() };
        if (!out) {
            logger::error("discovery: cannot write {}", path->string());
            return;
        }

        out << "# Discovered third-party enchantment effects, as affix rows.\n"
            << "#\n"
            << "# NOT a finished table. Tokens, slots, durations and magnitudes are read\n"
            << "# from the load order; points, weight, minItemLevel and the name fragments\n"
            << "# are defaults that want editing. Delete every row you do not want.\n"
            << "#\n"
            << "# To use: trim, then save beside the DLL as\n"
            << "#   SKSE/Plugins/DiabloLoot_affixes_<name>.csv\n"
            << "# Anything matching DiabloLoot_affixes_*.csv merges after the base table.\n"
            << "#\n"
            << "affixId,name,category,tier,points,minValue,maxValue,unit,duration,area,"
               "slots,weight,minItemLevel,mgef,npcExclude,prefix,suffix\n";

        std::size_t total = 0;
        for (const auto& [plugin, rows] : byPlugin) {
            out << "\n# ---- " << plugin << " (" << (rows.size() / 3) << " effects) ----\n";
            for (const auto& row : rows) {
                out << row << '\n';
            }
            total += rows.size();
            logger::info("discovery: {} -> {} affix(es)", plugin, rows.size() / 3);
        }

        logger::info("discovery: wrote {} rows across {} plugin(s) to {}", total,
            byPlugin.size(), path->string());
        logger::info("  filtered out: {} script-driven, {} sub-components, {} with no magnitude "
                     "range, {} already in the base table",
            skippedScripted, skippedComponent, skippedFlat, skipped);
        if (skippedCollision) {
            logger::warn("  {} id(s) collided with the base table and were prefixed mod_",
                skippedCollision);
        }
    }

void MgefSurvey::Run(const roll::AffixTable& a_table)
{
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        logger::error("mgef survey: no TESDataHandler");
        return;
    }

    const auto usage = BuildUsageMap(handler);
    const auto& effects = handler->GetFormArray<RE::EffectSetting>();

    logger::info("");
    logger::info("############ MAGIC EFFECT SURVEY ############");
    logger::info("{} magic effects in the load order, {} of them used by enchantments",
        effects.size(), usage.size());
    logger::info("Matching on archetype + actor value + resistance, not on names.");
    logger::info("");

    std::size_t covered = 0;
    std::size_t missing = 0;
    std::size_t weak = 0;
    // affixId -> the candidate the survey would pick, for the emitted map.
    std::map<std::string, std::pair<RE::EffectSetting*, Usage>> best;
    std::vector<std::string> weakList;
    std::vector<std::string> gaps;

    for (const auto& affix : a_table.Affixes()) {
        const auto* expectation = ExpectationFor(affix.id);
        if (!expectation) {
            logger::warn("  {:<20} NO EXPECTATION DEFINED -- survey cannot judge it", affix.id);
            continue;
        }

        struct Candidate
        {
            RE::EffectSetting* mgef;
            Usage              usage;
            int                uses;
            int                source;
            bool               visible;
        };
        std::vector<Candidate> candidates;

        for (auto* mgef : effects) {
            if (!mgef) {
                continue;
            }
            const auto& data = mgef->data;

            // ★kValueModifier and kPeakValueModifier are both accepted, and the
            // distinction is exactly what the previous run got wrong.
            //
            // Fortify POTIONS are kValueModifier (0). Fortify ENCHANTMENTS are
            // kPeakValueModifier (34). Demanding 0 meant every real Fortify
            // enchantment was invisible and the survey matched Boons and nerfs
            // instead -- reporting affixes as covered by effects no enchantment
            // has ever used. Accept either, and let the usage ranking below
            // decide which is actually worth anything.
            // ★kDualValueModifier belongs here too, and frost/shock are why.
            //
            // Fire Damage touches one actor value, so it is kValueModifier and
            // matched vanilla immediately. Frost also drains Stamina and Shock
            // also drains Magicka -- two values, so both are kDualValueModifier,
            // and demanding the single-value archetype made Skyrim's own Frost
            // Damage invisible. The survey then reached for a weapon mod's
            // effect for frost and a Dragonborn water HAZARD for shock.
            //
            // The AV and resistance tests below still do the discriminating; the
            // archetype was never the part carrying the meaning.
            const bool archMatch = data.archetype == expectation->archetype ||
                (expectation->archetype == Archetype::kValueModifier &&
                    (data.archetype == Archetype::kPeakValueModifier ||
                        data.archetype == Archetype::kDualValueModifier));
            if (!archMatch) {
                continue;
            }
            if (expectation->primaryAV != kAnyAV) {
                const bool avMatch = data.primaryAV == expectation->primaryAV ||
                    (expectation->alt1 != kAnyAV && data.primaryAV == expectation->alt1) ||
                    (expectation->alt2 != kAnyAV && data.primaryAV == expectation->alt2);
                if (!avMatch) {
                    continue;
                }
            }
            if (expectation->resist != AV::kNone && data.resistVariable != expectation->resist) {
                continue;
            }
            if (expectation->harm != Harm::kEither &&
                mgef->IsDetrimental() != (expectation->harm == Harm::kYes)) {
                continue;
            }
            if (MustBeBeneficial(affix.id) && mgef->IsDetrimental()) {
                continue;
            }

            const auto it = usage.find(mgef);
            const auto info = (it == usage.end()) ? UsageInfo{} : it->second;
            // Whether the item card will actually SAY anything about it. An
            // effect flagged kHideInUI, or one with no description text, applies
            // normally and displays nothing -- so the player sees a renamed item
            // with no stated reason. Ranked below visible effects rather than
            // excluded, because for some affixes it may be the only candidate.
            const bool visible =
                !mgef->data.flags.all(RE::EffectSetting::EffectSettingData::Flag::kHideInUI) &&
                mgef->magicItemDescription.c_str() && *mgef->magicItemDescription.c_str();
            candidates.push_back({ mgef, info.kind, info.count, SourceRank(mgef), visible });
        }

        // Proven beats unproven; among proven, the WORKHORSE beats the curiosity
        // (an effect used by thirty enchantments is the generic one, an effect
        // used by one is somebody's artifact); and among equals, Skyrim.esm beats
        // a mod, so the affix table does not quietly grow dependencies.
        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a_lhs, const Candidate& a_rhs) {
                const bool lProven = a_lhs.usage != Usage::kUnused;
                const bool rProven = a_rhs.usage != Usage::kUnused;
                if (lProven != rProven) {
                    return lProven;
                }
                // ★SOURCE BEFORE COUNT, and the order is the whole point.
                //
                // Ranking count first let NewArmoury.esp outvote Skyrim.esm on
                // frost and shock damage, purely because that mod adds a lot of
                // enchanted weapons. Popularity within one person's load order is
                // not a reason to make every user of this mod install theirs: a
                // third-party pick turns the affix into a hard dependency, and it
                // is dead for anyone without that plugin.
                //
                // Count still decides WITHIN a source, which is what picks the
                // generic Fortify Barter over the Amulet of Articulation's
                // one-off effect.
                // VISIBILITY OUTRANKS EVERYTHING BELOW PROVEN. An affix nobody
                // can read is worse than one from a less popular effect: it
                // produced "Bow of Turning" with a blank item card.
                if (a_lhs.visible != a_rhs.visible) {
                    return a_lhs.visible;
                }
                if (a_lhs.source != a_rhs.source) {
                    return a_lhs.source < a_rhs.source;
                }
                return a_lhs.uses > a_rhs.uses;
            });

        if (candidates.empty()) {
            ++missing;
            gaps.push_back(affix.id);
            logger::info("  {:<20} ★ NEEDS AUTHORING{}{}", affix.id,
                expectation->note[0] ? " -- " : "", expectation->note);
            continue;
        }

        // ★"Covered" means PROVEN IN AN ENCHANTMENT, not merely field-matched.
        //
        // A magic effect that exists but that no enchantment has ever used is
        // weak evidence: it may be a perk effect, a Boon, a disease, or a script
        // hook, and nothing says it behaves as a constant effect on worn gear.
        // Counting those as covered is how the previous run reported 46 of 50
        // affixes done while silently pointing eleven of them at junk.
        const bool proven = std::any_of(candidates.begin(), candidates.end(),
            [](const Candidate& a_candidate) { return a_candidate.usage != Usage::kUnused; });

        if (!proven) {
            ++weak;
            weakList.push_back(affix.id);
            logger::info("  {:<20} ⚠ WEAK -- {} match(es), but NONE used by any enchantment{}{}",
                affix.id, candidates.size(),
                expectation->note[0] ? "  -- " : "", expectation->note);
        } else {
            ++covered;
            best[affix.id] = { candidates.front().mgef, candidates.front().usage };
            logger::info("  {:<20} {} candidate(s){}{}", affix.id, candidates.size(),
                expectation->note[0] ? "  -- " : "", expectation->note);
        }
        for (std::size_t i = 0; i < candidates.size() && i < 3; ++i) {
            auto* mgef = candidates[i].mgef;
            const char* name = mgef->GetName();
            // ★VISIBILITY, on every candidate line. Two picks in a row turned
            // out to be hide-in-UI effects, and both times this dump was the
            // thing being read while choosing -- it showed how OFTEN an effect
            // was used and never whether a player could see it, so the ranking
            // looked well-evidenced and produced an invisible affix anyway.
            const char* cdesc = mgef->magicItemDescription.c_str();
            const bool  chidden =
                mgef->data.flags.all(RE::EffectSetting::EffectSettingData::Flag::kHideInUI);
            const char* vis = chidden          ? "  [HIDDEN -- unusable]"
                            : (!cdesc || !*cdesc) ? "  [no description -- unusable]"
                                                  : "";
            logger::info("      [{:08X}] {:<34} {} x{}  ({}){}", mgef->GetFormID(),
                (name && *name) ? name : "<unnamed>", UsageName(candidates[i].usage),
                candidates[i].uses, SourceFile(mgef), vis);
        }
    }

    // ------------------------------------------------------------------------
    // Ground truth: what apparel enchantments in THIS load order are actually
    // built from, grouped by the two fields the engine keys off.
    //
    // This exists so the table above stops being a guess. Every affix that
    // reports NEEDS AUTHORING should be checked against this list first -- if a
    // plausible group is sitting here, the expectation is wrong, not the game.
    // ------------------------------------------------------------------------
    // ★BOTH KINDS, not just apparel. The apparel-only dump answered the fortify
    // questions and left the weapon on-hit ones unlit, which is exactly where
    // frost and shock then went wrong -- nothing in the log contradicted the
    // guess, so the bad pick looked like a result.
    for (int pass = 0; Config::VerboseSurvey() && pass < 2; ++pass) {
        const bool wantApparel = (pass == 0);

        std::map<std::pair<int, int>, std::pair<int, RE::EffectSetting*>> groups;
        for (const auto& [mgef, info] : usage) {
            const bool isApparel = info.kind == Usage::kApparelEnch || info.kind == Usage::kBoth;
            const bool isWeapon = info.kind == Usage::kWeaponEnch || info.kind == Usage::kBoth;
            if (wantApparel ? !isApparel : !isWeapon) {
                continue;
            }
            const auto key = std::make_pair(static_cast<int>(mgef->data.archetype),
                static_cast<int>(mgef->data.primaryAV));
            auto& slot = groups[key];
            ++slot.first;
            if (!slot.second) {
                slot.second = mgef;
            }
        }

        logger::info("");
        logger::info("  ---- what {} enchantments are actually built from ----",
            wantApparel ? "APPAREL" : "WEAPON");
        logger::info("  {} distinct (archetype, actorValue) groups", groups.size());
        for (const auto& [key, value] : groups) {
            const auto  av = static_cast<AV>(key.second);
            const auto* info = RE::ActorValueList::GetActorValueInfo(av);
            const char* avName = (info && info->enumName) ? info->enumName : "?";
            const char* sample = value.second ? value.second->GetName() : "";
            logger::info("    archetype {:>2}  AV {:>3} {:<26} x{:<4} e.g. {}",
                key.first, key.second, avName, value.first,
                (sample && *sample) ? sample : "<unnamed>");
        }
    }

    // ------------------------------------------------------------------------
    // Emit the mgef map, so the CSV gets filled from what the game resolved
    // rather than from someone copying 48 FormIDs out of a log by hand.
    //
    // ★The token is "plugin|0xLOCALID", NOT a raw FormID. A raw FormID encodes
    // the plugin's position in THIS load order in its high byte, so it means
    // something different on every machine. GetLocalFormID strips that -- and
    // handles the ESL case, where the local part is 12 bits rather than 24.
    // ------------------------------------------------------------------------
    if (!best.empty()) {
        auto mapPath = SKSE::log::log_directory();
        if (mapPath) {
            *mapPath /= "DiabloLoot_mgef_map.csv";
            std::ofstream out{ mapPath->string() };
            if (out) {
                out << "# generated by MgefSurvey against this load order\n";
                out << "# duration/area are the MODE across every enchantment that uses the\n";
                out << "# effect -- what vanilla actually ships, not what seemed reasonable\n";
                out << "# affixId,mgef,duration,area,effectName,usage\n";
                for (const auto& [affixId, pick] : best) {
                    auto* mgef = pick.first;
                    auto* file = mgef->GetFile(0);
                    const char* name = mgef->GetName();

                    const auto  it = usage.find(mgef);
                    const auto  info = (it == usage.end()) ? UsageInfo{} : it->second;
                    const auto [duration, durAgreed] = Mode(info.durations);
                    const auto [area, areaAgreed] = Mode(info.areas);

                    out << affixId << ','
                        << (file ? std::string{ file->GetFilename() } : std::string{ "?" })
                        << '|' << std::format("0x{:06X}", mgef->GetLocalFormID()) << ','
                        << duration << ',' << area << ','
                        << ((name && *name) ? name : "<unnamed>") << ','
                        << UsageName(pick.second) << '\n';

                    // Disagreement is worth seeing rather than silently averaging
                    // away: an effect vanilla ships at several durations is one
                    // where the "right" answer depends on context we do not have.
                    if (!durAgreed) {
                        logger::warn("  '{}' duration varies across vanilla usages: {} -- taking {}",
                            affixId, Histogram(info.durations), duration);
                    }
                    if (!areaAgreed) {
                        logger::warn("  '{}' area varies across vanilla usages: {} -- taking {}",
                            affixId, Histogram(info.areas), area);
                    }
                }
                logger::info("");
                logger::info("  wrote {} mgef mappings to {}", best.size(), mapPath->string());
            } else {
                logger::error("  could not write the mgef map to {}", mapPath->string());
            }
        }
    }

    // Opt-in: a tool you run once when adding a mod, not a launch cost.
    if (Config::DiscoveryEnabled()) {
        std::set<std::string> baseIds;
        for (const auto& affix : a_table.Affixes()) {
            baseIds.insert(affix.id);
        }
        EmitDiscovery(usage, best, baseIds);
    }

    logger::info("");
    logger::info("  ---- Phase 1 work list ----");
    logger::info("  {} affixes have an effect PROVEN in an existing enchantment", covered);
    logger::info("  {} matched only effects no enchantment uses (verify by hand):", weak);
    for (const auto& item : weakList) {
        logger::info("      {}", item);
    }
    logger::info("  {} need authoring in the ESL:", missing);
    for (const auto& gap : gaps) {
        logger::info("      {}", gap);
    }
    logger::info("############################################");
    logger::info("");
}
