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

#include "roll/Roll.h"

#include <cstdint>
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
    // one for blue, five for red. Empty means no marker at all.
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

    // ---- weapon charge -----------------------------------------------------
    //
    // Whether a rolled WEAPON gets a finite charge, like a vanilla enchanted
    // weapon: a meter that drains per hit and refills from a soul gem. Off by
    // default -- affixes have always been free, and a save full of weapons that
    // suddenly need feeding is a change the player should choose.
    //
    // ★WEAPONS ONLY, by the engine's rules rather than ours. Armour enchantments
    // are constant-effect and have no charge concept at all, so the setting
    // cannot mean anything for them.
    //
    // ★ONLY ITEMS ROLLED FROM HERE ON. The per-hit cost is baked into the
    // created enchantment when it is built and the charge into the instance
    // when it is attached; nothing already in a save is revisited in either
    // direction. Turning this off again leaves the finite weapons finite.
    [[nodiscard]] bool WeaponChargeEnabled();

    // The maximum charge a freshly rolled weapon carries, by the BAND it rolled
    // -- a red starts with more in the tank than a blue, the way a better
    // vanilla weapon ships with a bigger charge. The engine stores an
    // instance's charge in sixteen bits, so [1, 65535]. Defaults 1000 blue,
    // 1500 yellow, 2000 purple, 2500 orange, 3000 red. White never gets here:
    // a white item has no enchantment to charge.
    [[nodiscard]] std::uint16_t WeaponChargeAmount(roll::Band a_band);

    // Charge drawn per hit: a flat part plus a part per affix POINT of the roll
    // (1..15, the same sum of points that decides the band), so a blue and a
    // red drain at different rates the way a weak and a strong vanilla
    // enchantment do. Defaults 10 and 2: a one-point blue lands about 83 hits
    // from its 1000 and a fifteen-point red about 75 from its 3000.
    [[nodiscard]] int WeaponChargeCost();
    [[nodiscard]] int WeaponChargeCostPerPoint();

    // Whether a weapon-skill affix on a WORN weapon is applied to the actor
    // holding it. Default on. Off, the affix still rolls and still reads on
    // the card; it just does nothing, which is what a weapon enchantment can
    // manage on its own. See Wielder.h.
    [[nodiscard]] bool WielderBuffsEnabled();

    // Whether a vendor's stock is rolled when the barter menu opens. Gated by
    // Distribution like everything else that rolls. Default on.
    [[nodiscard]] bool VendorStockEnabled();

    // What a merchant asks for a rolled item, as a multiple of the engine's
    // own value, by colour band. 1 for white; the defaults climb from 1.5 for
    // blue to 10 for red. See Pricing.h.
    [[nodiscard]] float PriceMult(roll::Band a_band);

    // Whether the same multiple applies to what a merchant PAYS the player.
    // Default off: a loot mod should not quietly change the player's income.
    [[nodiscard]] bool SellPricesScaled();

    // Whether the price hook is installed at all. Default on. The one switch
    // here that exists for safety rather than taste: the hook patches code
    // found by scanning, and if that ever goes wrong on some build this turns
    // it off without a rebuild. Read at kDataLoaded, before the install.
    [[nodiscard]] bool PriceHookEnabled();

    // ★A DIAGNOSTIC, off by default. Logs what a container holds the instant
    // its menu opens -- before and after this mod's pass -- and every item
    // that enters or leaves a non-player container with nothing on the other
    // end of the move, which is what a quest alias filling a chest, or a
    // script emptying one, looks like from here. A line per item per open, so
    // only for a bug report. Read once, at Distribute::Install.
    [[nodiscard]] bool ContainerTrace();
}
