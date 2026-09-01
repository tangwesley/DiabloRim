// =============================================================================
//  Settings, read from SKSE/Plugins/DiabloLoot.ini.
// =============================================================================
//  Deliberately tiny: key=value, '#' or ';' starts a comment, sections ignored.
//  No INI library, because the alternative is a dependency shipped to every
//  user in order to parse eight lines.
//
//  ★EVERY SETTING HAS A SAFE DEFAULT AND THE FILE IS OPTIONAL. A missing or
//  malformed INI must never stop the mod working -- it is configuration, not
//  data, and the difference is that data being wrong is worth failing over.
// =============================================================================
#pragma once

#include <string_view>

namespace Config
{
    // Reads the file if present. Call once, on kDataLoaded, before anything
    // asks a question.
    void Load();

    // Whether distribution runs at all. Default true -- the mod was installed
    // deliberately, and refusing to work until prompted is a worse default.
    [[nodiscard]] bool DistributionEnabled();

    // Whether a non-unique item handed over by a quest gets rolled -- on the
    // first equip, not on the hand-over; see QuestReward.h for why the two are
    // not the same. Gated by Distribution: it is the same feature reaching one
    // more place, not a second one.
    //
    // ★THE SWITCH EXISTS BECAUSE THIS PATH TOUCHES THE PLAYER'S OWN INVENTORY,
    // which nothing else in the mod does -- it removes and re-adds the item to
    // get per-instance data the engine built (Apply::ToNewInstance). That works,
    // and it is still the one thing here whose blast radius is the player's own
    // pack rather than a bandit's, so an off switch that needs no rebuild is
    // worth more than the line it costs.
    [[nodiscard]] bool QuestRewardsEnabled();

    // How much loot a container hands over, as a multiplier on its LEVELED
    // entries. 1.0 is vanilla and the default; 3.0 resolves the leveled lists a
    // chest is built from three times over instead of once.
    //
    // ★A MULTIPLIER ON THE ROLL, NOT ON THE CONTENTS, and the difference is the
    // whole safety argument. Hand-placed entries -- the specific sword a level
    // designer put in that specific chest -- are never touched, because
    // duplicating one is duplicating something the world was authored around.
    // Only entries that were random to begin with are rolled again, which is
    // the same thing the engine already did once.
    //
    // Fractional values work and are the honest way to say "a bit more": 1.5 is
    // a second resolution half the time, not half an item.
    //
    // Clamped to [1, 10] on read. Below 1 is not implemented -- taking loot
    // away means deciding WHICH item to drop, and that is a different feature
    // wearing the same setting.
    [[nodiscard]] float ContainerLootMultiplier();

    // ★Off by default, and it is a TOOL rather than a feature.
    //
    // Discovery walks every third-party enchantment effect in the load order and
    // writes a starter affix table for them. That is something you run once when
    // adding a mod like Summermyst, then turn off -- it costs a file write and a
    // wall of log on every launch otherwise, to produce a file you already have.
    [[nodiscard]] bool DiscoveryEnabled();

    // Whether the magic-effect survey prints its full ground-truth listing.
    // Useful while the affix table is being built, noise once it is settled.
    [[nodiscard]] bool VerboseSurvey();

    // The glyph repeated once per band at the end of a rolled item's name --
    // one for blue, four for orange. Empty means no marker at all.
    //
    // ★THE VALUE IS TAKEN VERBATIM, whatever it is: a diamond, a star, an
    // asterisk, a word. Which glyphs actually draw depends on the player's
    // fonts and menu replacers, so there is no list of blessed characters this
    // file could offer that would be right on every setup.
    //
    // Load() hands this straight to roll::SetTierMark, so the roller stays free
    // of INIs; the accessor is here for logging and for anyone who wants to ask
    // without reaching into the roll library.
    [[nodiscard]] std::string_view TierMarker();

    // Whether the marker is written into the item's stored name. On by default,
    // and currently the only surface that can carry it.
    //
    // ★WORTH TURNING OFF WITH GRID INVENTORY INSTALLED. The tint already
    // colours the item by band there, so the glyphs repeat what the player can
    // already see -- while still joining every list's sort key and every save.
    [[nodiscard]] bool TierMarkerInName();
}
