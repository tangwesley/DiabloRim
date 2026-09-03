// =============================================================================
//  Phase 5 -- the persistence spine. See Persist.h.
// =============================================================================

#include "PCH.h"

#include "Persist.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // ★A four-character code, and it must be unique across every SKSE plugin the
    // user has installed. Collide with someone else's and both plugins read each
    // other's records as their own, which is a save-corrupting class of bug that
    // presents as "your mod broke my game" months later.
    constexpr std::uint32_t kUniqueID = 'DISK';

    // Record types inside our own namespace, so these only have to be unique
    // among ourselves.
    constexpr std::uint32_t kRolledActors = 'ARLL';

    // A SECOND RECORD TYPE, not a wider first one. An old build meeting a new
    // save skips this by its own "unknown record type" branch and carries on
    // with the actor set intact -- where widening kRolledActors would have made
    // the whole record unreadable and re-rolled the world.
    constexpr std::uint32_t kEnchTiers = 'ETIR';

    // ★Bump this whenever the LAYOUT changes, never for content changes. The
    // loader refuses versions it does not know rather than guessing, so an old
    // build meeting a new save skips the record and re-rolls -- annoying, and
    // vastly better than misreading the bytes.
    constexpr std::uint32_t kVersion = 1;

    // Serialization callbacks run on the game's save/load thread; MarkRolled
    // runs from the task interface. Different threads, same container.
    std::mutex                       g_lock;
    std::unordered_set<RE::FormID>   g_rolled;

    // A LOCK OF ITS OWN, deliberately not g_lock. EnchTier is read once per
    // visible tile per frame on the render thread while the inventory is open;
    // MarkRolled is written from the task interface as actors load. Sharing one
    // mutex would put a UI redraw behind a distribution sweep for no reason --
    // the two sets are never touched together.
    std::mutex                                     g_tierLock;
    std::unordered_map<RE::FormID, std::uint8_t>   g_enchTier;

    void SaveRolledActors(SKSE::SerializationInterface* a_intfc)
    {
        std::scoped_lock lock{ g_lock };

        if (!a_intfc->OpenRecord(kRolledActors, kVersion)) {
            logger::error("save: could not open the rolled-actor record; this save will "
                          "re-roll every actor when loaded");
            return;
        }

        const auto count = static_cast<std::uint32_t>(g_rolled.size());
        if (!a_intfc->WriteRecordData(count)) {
            logger::error("save: failed writing the rolled-actor count");
            return;
        }

        for (const auto formID : g_rolled) {
            if (!a_intfc->WriteRecordData(formID)) {
                logger::error("save: failed writing a rolled-actor id; the record is now "
                              "short and will be rejected on load");
                return;
            }
        }

        logger::info("save: wrote {} rolled actor(s)", count);
    }

    void SaveEnchTiers(SKSE::SerializationInterface* a_intfc)
    {
        std::scoped_lock lock{ g_tierLock };

        if (!a_intfc->OpenRecord(kEnchTiers, kVersion)) {
            logger::error("save: could not open the enchantment-tier record; affixed items "
                          "in this save will draw uncoloured");
            return;
        }

        const auto count = static_cast<std::uint32_t>(g_enchTier.size());
        if (!a_intfc->WriteRecordData(count)) {
            logger::error("save: failed writing the enchantment-tier count");
            return;
        }

        for (const auto& [formID, band] : g_enchTier) {
            if (!a_intfc->WriteRecordData(formID) || !a_intfc->WriteRecordData(band)) {
                logger::error("save: failed writing an enchantment tier; the record is now "
                              "short and will be rejected on load");
                return;
            }
        }

        logger::info("save: wrote {} enchantment tier(s)", count);
    }

    void SaveCallback(SKSE::SerializationInterface* a_intfc)
    {
        // One record each, written in turn. Kept as two calls rather than one
        // body so a failure part-way through the actor set cannot also cost the
        // tier map -- each record's early returns end only its own record.
        SaveRolledActors(a_intfc);
        SaveEnchTiers(a_intfc);
    }

    // One kEnchTiers record. Split out so the dispatch in LoadCallback stays a
    // list of record types rather than two interleaved parsers.
    void LoadEnchTiers(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        if (a_version != kVersion) {
            logger::warn("load: enchantment-tier record is version {}, this build understands {}. "
                         "Skipping it -- affixed items in this save will draw uncoloured.",
                a_version, kVersion);
            return;
        }

        std::uint32_t count = 0;
        if (!a_intfc->ReadRecordData(count)) {
            logger::error("load: could not read the enchantment-tier count");
            return;
        }

        std::size_t restored = 0;
        std::size_t dropped = 0;

        std::scoped_lock lock{ g_tierLock };
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID   oldID = 0;
            std::uint8_t band = 0;
            if (!a_intfc->ReadRecordData(oldID) || !a_intfc->ReadRecordData(band)) {
                logger::error("load: enchantment-tier record ended after {} of {} entries",
                    i, count);
                break;
            }

            // THE SAME REMAP THE ACTOR SET GETS, and it matters more here, not
            // less. These are DYNAMIC forms -- created enchantments living in
            // the save's own created-object block -- and the engine renumbers
            // that block as objects come and go. ResolveFormID is what follows
            // one across the reload; a raw id would name a different
            // enchantment, and colour the wrong sword.
            RE::FormID newID = 0;
            if (!a_intfc->ResolveFormID(oldID, newID)) {
                // The enchantment did not survive: its item was destroyed, or
                // a plugin it depended on is gone. Nothing left to colour.
                ++dropped;
                continue;
            }
            if (band != 0) {
                g_enchTier[newID] = band;
                ++restored;
            }
        }

        logger::info("load: restored {} enchantment tier(s){}", restored,
            dropped ? std::format(", dropped {} that no longer resolve", dropped) : "");
    }

    void LoadCallback(SKSE::SerializationInterface* a_intfc)
    {
        std::uint32_t type = 0;
        std::uint32_t version = 0;
        std::uint32_t length = 0;

        std::size_t restored = 0;
        std::size_t dropped = 0;

        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (type == kEnchTiers) {
                LoadEnchTiers(a_intfc, version);
                continue;
            }
            if (type != kRolledActors) {
                logger::warn("load: skipping unknown record type {:08X}", type);
                continue;
            }
            if (version != kVersion) {
                // Loud, and deliberately not a guess. Re-rolling is recoverable;
                // misreading a different layout is not.
                logger::warn("load: rolled-actor record is version {}, this build understands {}. "
                             "Skipping it -- actors in this save will be re-rolled.",
                    version, kVersion);
                continue;
            }

            std::uint32_t count = 0;
            if (!a_intfc->ReadRecordData(count)) {
                logger::error("load: could not read the rolled-actor count");
                continue;
            }

            std::scoped_lock lock{ g_lock };
            for (std::uint32_t i = 0; i < count; ++i) {
                RE::FormID oldID = 0;
                if (!a_intfc->ReadRecordData(oldID)) {
                    logger::error("load: record ended after {} of {} ids", i, count);
                    break;
                }

                // ★THE LOAD-ORDER REMAP, and the whole reason this is not a raw
                // set of integers. A FormID's high byte is the plugin index at
                // the time of saving; the user may have added, removed or
                // reordered mods since. ResolveFormID rewrites it for the
                // current order and fails for forms whose plugin is simply gone.
                RE::FormID newID = 0;
                if (!a_intfc->ResolveFormID(oldID, newID)) {
                    // The actor's plugin was removed. Forgetting it is correct:
                    // there is nothing left to re-roll.
                    ++dropped;
                    continue;
                }
                g_rolled.insert(newID);
                ++restored;
            }
        }

        logger::info("load: restored {} rolled actor(s){}", restored,
            dropped ? std::format(", dropped {} whose plugin is gone", dropped) : "");
    }

    void RevertCallback(SKSE::SerializationInterface*)
    {
        std::size_t hadTiers = 0;
        {
            // The tier map is save-scoped exactly as the actor set is. Save A's
            // created enchantments do not exist in save B, and their FormIDs
            // are reused by B's own created objects -- so keeping the map would
            // not merely be stale, it would colour unrelated items.
            std::scoped_lock tierLock{ g_tierLock };
            hadTiers = g_enchTier.size();
            g_enchTier.clear();
        }

        std::scoped_lock lock{ g_lock };
        const auto       had = g_rolled.size();
        g_rolled.clear();

        // ★THIS IS NOT HOUSEKEEPING. Revert fires before a load replaces the
        // world. Skipping it would leave save A's rolled set in memory while
        // save B loads, and every actor A had already rolled would be silently
        // skipped in B -- a bug that only appears when someone loads two saves
        // in one session, which is to say, constantly.
        logger::info("revert: cleared {} rolled actor(s) and {} enchantment tier(s)",
            had, hadTiers);
    }
}

void Persist::Install()
{
    auto* intfc = SKSE::GetSerializationInterface();
    if (!intfc) {
        logger::error("no serialization interface; rolled actors will NOT persist and every "
                      "cell reload will re-roll");
        return;
    }

    intfc->SetUniqueID(kUniqueID);
    intfc->SetSaveCallback(SaveCallback);
    intfc->SetLoadCallback(LoadCallback);
    intfc->SetRevertCallback(RevertCallback);

    logger::info("serialization registered (id '{}{}{}{}', record version {})",
        static_cast<char>((kUniqueID >> 24) & 0xFF), static_cast<char>((kUniqueID >> 16) & 0xFF),
        static_cast<char>((kUniqueID >> 8) & 0xFF), static_cast<char>(kUniqueID & 0xFF),
        kVersion);
}

bool Persist::MarkRolled(RE::FormID a_actor)
{
    if (a_actor == 0) {
        return false;
    }
    std::scoped_lock lock{ g_lock };
    return g_rolled.insert(a_actor).second;
}

bool Persist::WasRolled(RE::FormID a_actor)
{
    std::scoped_lock lock{ g_lock };
    return g_rolled.contains(a_actor);
}

bool Persist::ForgetRolled(RE::FormID a_actor)
{
    if (a_actor == 0) {
        return false;
    }
    std::scoped_lock lock{ g_lock };
    return g_rolled.erase(a_actor) > 0;
}

std::size_t Persist::RolledCount()
{
    std::scoped_lock lock{ g_lock };
    return g_rolled.size();
}

void Persist::Clear()
{
    std::scoped_lock lock{ g_lock };
    g_rolled.clear();
    std::scoped_lock tierLock{ g_tierLock };
    g_enchTier.clear();
}

void Persist::NoteEnchTier(RE::FormID a_enchantment, std::uint8_t a_band)
{
    // Band 0 is "no opinion" and is the answer for anything absent, so storing
    // it would cost a hash entry to say what an empty map already says.
    if (a_enchantment == 0 || a_band == 0) {
        return;
    }
    std::scoped_lock lock{ g_tierLock };
    g_enchTier[a_enchantment] = a_band;
}

std::uint8_t Persist::EnchTier(RE::FormID a_enchantment)
{
    if (a_enchantment == 0) {
        return 0;
    }
    std::scoped_lock lock{ g_tierLock };
    const auto       it = g_enchTier.find(a_enchantment);
    return it == g_enchTier.end() ? std::uint8_t{ 0 } : it->second;
}

void Persist::ForgetEnchTier(RE::FormID a_enchantment)
{
    std::scoped_lock lock{ g_tierLock };
    g_enchTier.erase(a_enchantment);
}

std::size_t Persist::EnchTierCount()
{
    std::scoped_lock lock{ g_tierLock };
    return g_enchTier.size();
}
