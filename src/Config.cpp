// =============================================================================
//  Settings. See Config.h.
// =============================================================================

#include "PCH.h"

#include "Config.h"

#include "GridTint.h"
#include "roll/Roll.h"

#include <algorithm>
#include <fstream>
#include <string>

namespace
{
    constexpr const char* kPath = "Data/SKSE/Plugins/DiabloLoot.ini";

    bool g_distribution = true;
    bool g_questRewards = true;
    bool g_discovery = false;
    bool g_verboseSurvey = false;

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
        "verbose survey {}, tier marker {} ({})",
        kPath, applied, g_distribution ? "on" : "OFF", g_questRewards ? "on" : "OFF",
        g_discovery ? "ON" : "off", g_verboseSurvey ? "ON" : "off",
        g_tierMarker.empty() ? std::string{ "(none)" } : "'" + g_tierMarker + "'",
        g_tierMarkerInName ? "in item names" : "on hover only (names left alone)");
}

bool Config::DistributionEnabled()
{
    return g_distribution;
}

bool Config::QuestRewardsEnabled()
{
    return g_questRewards;
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
