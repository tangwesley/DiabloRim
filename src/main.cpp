// =============================================================================
//  Diablo In Skyrim -- randomized affix loot
// =============================================================================
//  An SKSE plugin that rolls Diablo-style affixes onto items at NPC spawn and
//  on container open. It is a SEPARATE DLL from Grid Inventory: it owns gameplay
//  data (what an item rolled), Grid Inventory owns pixels, and the two meet over
//  GridInventoryAPI.h when there is something to draw.
//
//  This file is currently the loader and nothing else. The next thing to land
//  here is the Phase 0 spike -- see the marked block in SKSEPluginLoad.
// =============================================================================

#include "PCH.h"

#include <algorithm>
#include <filesystem>

#include "Apply.h"
#include "Config.h"
#include "Distribute.h"
#include "Enchanting.h"
#include "GridTint.h"
#include "Persist.h"
#include "QuestReward.h"
#include "Notify.h"
#include "MgefSurvey.h"

#include "roll/Roll.h"

namespace
{
    // The affix table, loaded once at kDataLoaded and read-only thereafter.
    roll::AffixTable g_affixes;

    // Phase 2: load the CSV and complain loudly rather than quietly misbehaving.
    //
    // Users editing a balance CSV is how this mod gets patched without shipping a
    // DLL, which means a malformed row is a NORMAL event, not an exceptional one.
    // Every complaint names its line number and says what the consequence is, and
    // a bad row is skipped rather than fatal -- one typo should cost one affix,
    // not the whole mod.
    void LoadAffixTable()
    {
        const std::string file = "Data/SKSE/Plugins/DiabloLoot_affixes.csv";

        std::vector<std::string> errors;
        const bool               ok = g_affixes.Load(file, errors);

        if (!ok) {
            logger::error("AFFIX TABLE FAILED TO LOAD from {}", file);
            for (const auto& error : errors) {
                logger::error("  {}", error);
            }
            logger::error("  no affixes are available; nothing will roll");
            return;
        }

        // ADD-ON FILES, merged after the base table.
        //
        // Anything matching DiabloLoot_affixes_*.csv in the same folder is merged
        // in load order. That is how "Summermyst affixes for DiabloLoot" ships as
        // a drop-in file rather than a fork of the base table: the base stays
        // regenerable, and a patch survives every update to it.
        //
        // Alphabetical, so the order is predictable rather than filesystem luck.
        std::vector<std::filesystem::path> addons;
        std::error_code                    ec;
        for (const auto& entry :
            std::filesystem::directory_iterator{ "Data/SKSE/Plugins", ec }) {
            const auto name = entry.path().filename().string();
            if (name.rfind("DiabloLoot_affixes_", 0) == 0 && entry.path().extension() == ".csv") {
                addons.push_back(entry.path());
            }
        }
        std::sort(addons.begin(), addons.end());

        for (const auto& addon : addons) {
            std::vector<std::string> addonErrors;
            g_affixes.MergeFile(addon.string(), addonErrors);
            logger::info("  merged add-on {}", addon.filename().string());
            for (const auto& err : addonErrors) {
                logger::warn("    {}", err);
            }
        }

        logger::info("affix table: {} affixes, {} tier rows from {}{}",
            g_affixes.Affixes().size(), g_affixes.TierRowCount(), file,
            addons.empty() ? "" : std::format(" + {} add-on file(s)", addons.size()));

        if (!errors.empty()) {
            logger::warn("  {} row(s) were rejected:", errors.size());
            for (const auto& error : errors) {
                logger::warn("    {}", error);
            }
        }

        // A slot no item can present, or a tier no level unlocks, means an affix
        // that silently never appears. Cheaper to say so here than to wonder in
        // six months why nothing ever rolls Muffle.
        for (const auto& affix : g_affixes.Affixes()) {
            if (affix.mgef.empty()) {
                logger::debug("  '{}' has no mgef yet; it cannot be applied to an item", affix.id);
            }
        }
    }

    void InitializeLog()
    {
        auto path = SKSE::log::log_directory();
        if (!path) {
            return;
        }
        *path /= "DiabloInSkyrim.log";

        // ★ROTATING, not truncating and not unbounded.
        //
        // Truncate-on-open (the template default) wipes the file every launch,
        // which destroyed the evidence for the first persistence test: the whole
        // question spans a process restart, so the proof lives in the PREVIOUS
        // session's log. Plain append fixed that and then grew without limit.
        //
        // Rotation keeps both properties. Sessions still accumulate across
        // restarts -- so a save/quit/reload round trip is still readable end to
        // end -- but the file cannot run away. Old runs age out into .1/.2
        // instead of being lost on the next launch.
        constexpr std::size_t kMaxLogBytes = 4 * 1024 * 1024;
        constexpr std::size_t kMaxLogFiles = 3;
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            path->string(), kMaxLogBytes, kMaxLogFiles);
        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
#ifndef NDEBUG
        log->set_level(spdlog::level::trace);
        log->flush_on(spdlog::level::trace);
#else
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
#endif
        spdlog::set_default_logger(std::move(log));
        // Milliseconds and thread id both earn their width here: an affix roll
        // fires from a game thread on an event, and "which thread, how long
        // after the event" is the first question every such bug asks.
        spdlog::set_pattern("[%H:%M:%S.%e] [%t] [%l] %v");
    }

    // Lifecycle, filtered to sender "SKSE" by the one-argument RegisterListener.
    //
    // ★The ABI handshake with Grid Inventory has its OWN, UNFILTERED listener
    // in GridTint.cpp -- message type numbers live in the sender's namespace,
    // so another plugin's "type 1" is indistinguishable from kPostLoad here.
    // GridInventoryAPI.h documents that trap at length; the short version is
    // that the two listeners use different sender keys and both are required.
    // Do not try to serve both roles from this one.
    void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg) {
            return;
        }
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            // Every form in the load order exists by now. The affix table's
            // FormID resolution and its loud validation pass belong here.
            logger::info("kDataLoaded: load order is resolvable");
            Config::Load();
            LoadAffixTable();
            MgefSurvey::Run(g_affixes);
            Apply::ResolveEffects(g_affixes);
            // After the table and effects: rolling before those exist would mark
            // actors as rolled while producing nothing.
            Distribute::SetTable(g_affixes);
            Distribute::Install();
            // ★ON BY DEFAULT now that the spike is gone. It was off while the only
            // way to start it was a hotkey nobody would find by accident; with
            // the mod installed deliberately, refusing to work until prompted is
            // just a worse default.
            Distribute::SetEnabled(Config::DistributionEnabled());
            // ★THE LEVELED INDEX BEFORE THE REWARD SINK, always. The sink's
            // whole notion of "non-unique" is that index; installed the other
            // way round it would read every reward as an artifact and quietly
            // roll nothing, which looks exactly like the feature not working.
            Apply::IndexLeveledItems();
            QuestReward::SetTable(g_affixes);
            Notify::Install();
            QuestReward::Install();
            // Gated by distribution as well as its own switch: turning the mod's
            // rolling off and still having quest rewards roll would be a
            // surprise, and "off" should mean off.
            QuestReward::SetEnabled(
                Config::DistributionEnabled() && Config::QuestRewardsEnabled());
            // ★INSTALLED EVEN WITH DISTRIBUTION OFF, and that is not an
            // oversight. Turning distribution off stops new rolls; it does not
            // remove the affixes already on a save's items, and those items
            // still have to be enchantable.
            Enchanting::Install();
            break;

        // These two exist so the Phase 0 timeline is unambiguous in an appended
        // log. "rolled, saved, restarted, loaded, absent" and "rolled, never
        // saved, restarted, loaded, absent" look identical without them, and
        // they mean opposite things.
        case SKSE::MessagingInterface::kSaveGame:
            logger::info(">>>>>>>> SAVE WRITTEN <<<<<<<<");
            // The hotkey that used to dump these is gone, and a system running
            // unattended still has to be answerable. A save is the natural
            // checkpoint: infrequent, and already a moment the log records.
            Distribute::LogStats();
            QuestReward::LogStats();
            break;

        case SKSE::MessagingInterface::kPreLoadGame:
            logger::info(">>>>>>>> LOADING A SAVE <<<<<<<<");
            // Anything the enchanter has detached points into the world about to
            // be replaced. Letting go costs nothing -- the save being loaded has
            // its own copy -- and holding on would be a write through a dangling
            // pointer the next time a table closed.
            Enchanting::Forget();
            // Same reasoning one layer up: a reward queued in the world being
            // replaced must not be rolled into the world replacing it.
            QuestReward::Forget();
            break;

        case SKSE::MessagingInterface::kNewGame:
        case SKSE::MessagingInterface::kPostLoadGame:
            // A save just replaced the world. Anything keyed to actors or refs
            // -- the rolled-actor set above all -- is about to be restored by
            // the serialization callbacks and must not be trusted before then.
            logger::info("game loaded");
            // Actors already in the world at load time never fire
            // TESObjectLoadedEvent. Sweeping here is what makes an existing save
            // pick up loot retroactively rather than only as the player travels.
            SKSE::GetTaskInterface()->AddTask([]() { Distribute::SweepLoaded(); });
            break;

        default:
            break;
        }
    }
}

SKSEPluginInfo(
    .Version              = { 1, 0, 0, 0 },
    .Name                 = "DiabloInSkyrim",
    .Author               = "Wesley",
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    InitializeLog();
    // a_log = false. SKSE::Init installs a logger of its own when left at the
    // default, which silently replaces the one InitializeLog just configured --
    // pattern, level and all. Measured: the first line of the log came out in
    // SKSE's format, not ours.
    SKSE::Init(a_skse, false);

    // SKSEPlugin_Version is a PluginDeclaration (the NG API), not the older
    // PluginVersionData -- so the version comes from GetVersion(), not a
    // pluginVersion member.
    const auto version = SKSEPlugin_Version.GetVersion();
    // Banner, because the log now appends: without a hard separator, two
    // sessions run together and the restart -- the entire point of the Phase 0
    // measurement -- becomes invisible.
    logger::info("");
    logger::info("################ PROCESS START ################");
    logger::info("{} v{}.{}.{} loaded", SKSEPlugin_Version.GetName(),
                 version.major(), version.minor(), version.patch());

    SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);

    // ★From HERE, not from a message handler. The serialization interface has to
    // be claimed before the first save or load can occur, and kDataLoaded is
    // already too late for a game launched straight into a save.
    Persist::Install();

    // ★Also from HERE, and for the same class of reason. The host announces
    // itself at kPostLoad, which is dispatched after every plugin's load
    // function has run -- so a listener registered any later than this misses
    // the only announcement it will ever get, and the grid draws uncoloured
    // with nothing in either log to say why.
    GridTint::Install();

    // -------------------------------------------------------------------------
    // Phase 0's spike is gone, along with its hotkeys. What it established, and
    // what the rest of this plugin now rests on:
    //
    //  1. A created enchantment survives save -> quit to desktop -> reload with
    //     its effects and magnitudes intact. Persistence rides the arcane
    //     enchanter's own path; no new records, no ESP.
    //  2. costOverride = 0 does not merely stop the drain -- charge is
    //     irrelevant to such an enchantment. It fires from empty, and at charge
    //     zero draws no meter at all.
    //  3. Per-instance data survives the whole journey: corpse, loot, save,
    //     restart, reload, equip. Names included.
    //  4. Which affixes EPW4NPCs reaches is STILL OPEN, and now testable against
    //     the real affix table rather than stand-ins.
    //  5. ExtraEnchantment is inert whenever the record carries an EITM -- in
    //     combat as well as on the item card. Vanilla-enchanted items are
    //     therefore left alone by design.
    //
    // The engine also dedupes identical created enchantments and refcounts the
    // sharing itself, so releasing one requires DestroyEnchantment and not just
    // unhooking the extra data. Two saves crashed teaching us that.
    // -------------------------------------------------------------------------

    return true;
}
