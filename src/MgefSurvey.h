// =============================================================================
//  Magic-effect survey -- Phase 1 scoping. See MgefSurvey.cpp.
// =============================================================================
#pragma once

namespace roll { class AffixTable; }

namespace MgefSurvey
{
    // Reports, for every affix in the table, which magic effects in the load
    // order could implement it -- and which have none and must be authored.
    // Call on kDataLoaded, after the affix table has loaded.
    void Run(const roll::AffixTable& a_table);
}
