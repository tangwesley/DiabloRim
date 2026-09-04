// =============================================================================
//  Settings. See Config.h.
// =============================================================================

#include "PCH.h"

#include "Config.h"

#include "GridTint.h"
#include "roll/Roll.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <string>

namespace
{
    constexpr const char* kPath = "Data/SKSE/Plugins/DiabloLoot.ini";

    bool g_distribution = true;
    bool g_questRewards = true;
    bool g_discovery = false;
    bool g_verboseSurvey = false;

    // 1.0 is vanilla. Clamped rather than validated: an INI is edited by hand,
    // and a fat-fingered 30 should give a very full chest, not a hang.
    float g_containerLoot = 1.0f;

    // Starts as whatever the roller ships with, so that not writing the setting
    // and deleting the whole file come out the same. (The roller ships U+25C6.)
    std::string g_tierMarker{ roll::TierMark() };

    // ★THIS ONE HAS NO FIXED DEFAULT, and cannot sensibly have one: the right
    // answer depends on whether anything ELSE is drawing the band.
    //
    // With Grid Inventory present the tooltip carries the marker and the grid
    // colours the item, so putting the glyphs in the name too is the same fact
    // three times -- while still joining every list's sort key and every save.
    // Without it the name is the only surface there is, and defaulting to off
    // would silently show the player nothing.
    //
    // So the default is decided at the bottom of Load(), from whether the host
    // is there. An explicit INI line always wins, which is what the `set` flag
    // is for -- "0" and "absent" must not mean the same thing.
    bool g_tierMarkerInName = false;
    bool g_tierMarkerInNameSet = false;

    // Off, and the three numbers only matter once it is on. See Config.h.
    bool g_weaponCharge = false;
    int  g_weaponChargeCost = 10;
    int  g_weaponChargeCostPerPoint = 2;

    // Indexed by roll::Band. White is a placeholder that is never read: a white
    // item carries no enchantment, so there is nothing to charge.
    std::uint16_t g_weaponChargeAmount[roll::kBandCount] = { 0, 1000, 1500, 2000, 2500, 3000 };

    std::string Trim(std::string a_text)
    {
        const auto first = a_text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        const auto last = a_text.find_last_not_of(" \t\r\n");
        return a_text.substr(first, last - first + 1);
    }

    // "1", "true", "yes", "on" -- anything else is false.
    //
    // Permissive on purpose: someone editing an INI by hand should not have to
    // guess which spelling of true this particular mod wanted.
    bool AsBool(std::string a_value)
    {
        std::transform(a_value.begin(), a_value.end(), a_value.begin(),
            [](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });
        return a_value == "1" || a_value == "true" || a_value == "yes" || a_value == "on";
    }

    // ★A BAD NUMBER KEEPS THE DEFAULT AND SAYS SO. std::stof throws on garbage,
    // and an exception escaping Load() over a typo would take the whole plugin
    // down at kDataLoaded -- for a setting whose entire job is optional.
    float AsFloat(const std::string& a_value, float a_fallback, float a_min, float a_max)
    {
        try {
            std::size_t used = 0;
            const float parsed = std::stof(a_value, &used);
            if (used != a_value.size()) {
                logger::warn("{}: '{}' has trailing junk; using {}", kPath, a_value, a_fallback);
                return a_fallback;
            }
            return std::clamp(parsed, a_min, a_max);
        } catch (const std::exception&) {
            logger::warn("{}: '{}' is not a number; using {}", kPath, a_value, a_fallback);
            return a_fallback;
        }
    }

    // Reads one band's charge amount off an INI line. Sixteen bits is the
    // engine's own ceiling on an instance's charge.
    void ReadChargeAmount(roll::Band a_band, const std::string& a_value)
    {
        auto& slot = g_weaponChargeAmount[static_cast<int>(a_band)];
        slot = static_cast<std::uint16_t>(
            AsFloat(a_value, static_cast<float>(slot), 1.0f, 65535.0f));
    }
}

void Config::Load()
{
    std::ifstream file{ kPath };
    if (!file) {
        logger::info("no {} -- using defaults (distribution on, discovery off)", kPath);
        return;
    }

    std::string line;
    std::size_t applied = 0;
    bool        first = true;

    while (std::getline(file, line)) {
        // ★THE BYTE-ORDER MARK, EATEN. Saving this file as UTF-8 is the
        // documented advice for a non-ASCII tier marker, and Notepad writes a
        // BOM when it does. Left in place it glues itself to the first line --
        // which is either an ignored comment or, if someone reorders the file,
        // a setting name that silently stops matching.
        if (first) {
            first = false;
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }

        const auto trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';' || trimmed[0] == '[') {
            continue;
        }

        const auto eq = trimmed.find('=');
        if (eq == std::string::npos) {
            logger::warn("{}: ignoring line without '=': {}", kPath, trimmed);
            continue;
        }

        auto key = Trim(trimmed.substr(0, eq));
        const auto value = Trim(trimmed.substr(eq + 1));
        std::transform(key.begin(), key.end(), key.begin(),
            [](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });

        if (key == "distribution") {
            g_distribution = AsBool(value);
        } else if (key == "questrewards") {
            g_questRewards = AsBool(value);
        } else if (key == "containerlootmultiplier") {
            g_containerLoot = AsFloat(value, 1.0f, 1.0f, 10.0f);
        } else if (key == "discovery") {
            g_discovery = AsBool(value);
        } else if (key == "verbosesurvey") {
            g_verboseSurvey = AsBool(value);
        } else if (key == "tiermarker") {
            // ★VERBATIM, AND AN EMPTY VALUE IS A REAL ANSWER. "TierMarker="
            // means the player wants no marker, which is different from leaving
            // the line out -- so this cannot fall back to the default on empty.
            // No case folding either: the value is text to display, not a
            // keyword to match.
            g_tierMarker = value;
        } else if (key == "tiermarkerinname") {
            g_tierMarkerInName = AsBool(value);
            g_tierMarkerInNameSet = true;
        } else if (key == "weaponcharge") {
            g_weaponCharge = AsBool(value);
        } else if (key == "weaponchargeblue") {
            ReadChargeAmount(roll::Band::kBlue, value);
        } else if (key == "weaponchargeyellow") {
            ReadChargeAmount(roll::Band::kYellow, value);
        } else if (key == "weaponchargepurple") {
            ReadChargeAmount(roll::Band::kPurple, value);
        } else if (key == "weaponchargeorange") {
            ReadChargeAmount(roll::Band::kOrange, value);
        } else if (key == "weaponchargered") {
            ReadChargeAmount(roll::Band::kRed, value);
        } else if (key == "weaponchargecost") {
            g_weaponChargeCost = static_cast<int>(AsFloat(value, 10.0f, 1.0f, 10000.0f));
        } else if (key == "weaponchargecostperpoint") {
            g_weaponChargeCostPerPoint = static_cast<int>(AsFloat(value, 2.0f, 0.0f, 1000.0f));
        } else {
            // Named, because a silently ignored setting is how someone spends an
            // evening wondering why their edit did nothing.
            logger::warn("{}: unknown setting '{}'", kPath, key);
            continue;
        }
        ++applied;
    }

    // ★THE ROLLER IS TOLD ONCE, HERE. src/roll/ links against nothing and
    // knows nothing about files; pushing the value in at load keeps it that way,
    // and keeps the balance harness able to link the same code.
    // ★THE HOST HAS ALREADY ANNOUNCED ITSELF BY NOW, and the ordering is what
    // makes this legal: Grid Inventory broadcasts at kPostLoad and Load() runs
    // at kDataLoaded, which is strictly later. Asking any earlier would read
    // "absent" for every player who has it.
    if (!g_tierMarkerInNameSet) {
        g_tierMarkerInName = !GridTint::Registered();
    }

    roll::SetTierMark(g_tierMarker);
    roll::SetTierMarkInName(g_tierMarkerInName);

    logger::info(
        "{}: {} setting(s) applied -- distribution {}, quest rewards {}, discovery {}, "
        "verbose survey {}, tier marker {} ({}), container loot x{:.2f}, weapon charge {}",
        kPath, applied, g_distribution ? "on" : "OFF", g_questRewards ? "on" : "OFF",
        g_discovery ? "ON" : "off", g_verboseSurvey ? "ON" : "off",
        g_tierMarker.empty() ? std::string{ "(none)" } : "'" + g_tierMarker + "'",
        g_tierMarkerInName ? "in item names" : "on hover only (names left alone)",
        g_containerLoot,
        g_weaponCharge ? std::format("ON ({}/{}/{}/{}/{} charge blue..red, {} + {}/point per hit)",
                             g_weaponChargeAmount[static_cast<int>(roll::Band::kBlue)],
                             g_weaponChargeAmount[static_cast<int>(roll::Band::kYellow)],
                             g_weaponChargeAmount[static_cast<int>(roll::Band::kPurple)],
                             g_weaponChargeAmount[static_cast<int>(roll::Band::kOrange)],
                             g_weaponChargeAmount[static_cast<int>(roll::Band::kRed)],
                             g_weaponChargeCost, g_weaponChargeCostPerPoint)
                       : std::string{ "off (affixes never drain)" });
}

bool Config::DistributionEnabled()
{
    return g_distribution;
}

bool Config::QuestRewardsEnabled()
{
    return g_questRewards;
}

float Config::ContainerLootMultiplier()
{
    return g_containerLoot;
}

bool Config::DiscoveryEnabled()
{
    return g_discovery;
}

bool Config::VerboseSurvey()
{
    return g_verboseSurvey;
}

std::string_view Config::TierMarker()
{
    return g_tierMarker;
}

bool Config::TierMarkerInName()
{
    return g_tierMarkerInName;
}

bool Config::WeaponChargeEnabled()
{
    return g_weaponCharge;
}

std::uint16_t Config::WeaponChargeAmount(roll::Band a_band)
{
    const auto index = static_cast<int>(a_band);
    if (index < 0 || index >= roll::kBandCount) {
        return 0;
    }
    return g_weaponChargeAmount[index];
}

int Config::WeaponChargeCost()
{
    return g_weaponChargeCost;
}

int Config::WeaponChargeCostPerPoint()
{
    return g_weaponChargeCostPerPoint;
}
