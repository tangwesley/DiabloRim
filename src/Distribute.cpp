// =============================================================================
//  Phase 5 -- distribution. See Distribute.h.
// =============================================================================

#include "PCH.h"

#include "Distribute.h"

#include "Apply.h"
#include "Config.h"
#include "Persist.h"
#include "roll/Roll.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace
{
    const roll::AffixTable* g_table = nullptr;
    roll::Tuning            g_tuning;

    // Enabled by main once the table and effects are ready. Kept as an atomic
    // rather than a constant because the ticker thread reads it, and because a
    // kill switch that works mid-session is worth having even with no UI to
    // reach it: a future MCM or console command has somewhere to attach.
    std::atomic<bool> g_enabled{ false };

    std::mutex           g_statsLock;
    Distribute::Stats    g_stats;

    // ★A CAP, not an expectation. An NPC with a pathological inventory -- a
    // merchant chest, a modded follower carrying four hundred items -- must not
    // be able to stall a cell load. Hitting the cap is worth logging, because it
    // means somebody's loot is quietly incomplete.
    constexpr std::size_t kMaxItemsPerActor = 12;

    // A merchant chest is the one inventory the player reads top to bottom, so
    // it gets a cap sized for a shop rather than a bandit. Still a cap: a
    // modded vendor with a thousand rows must not stall the barter menu.
    constexpr std::size_t kMaxItemsPerVendor = 64;

    // Per-thread RNG. The roller is deterministic given its generator, and
    // sharing one across the task queue would make results depend on scheduling
    // -- reproducible bug reports matter more than a few bytes.
    roll::Rng& ThreadRng()
    {
        static thread_local roll::Rng rng{ std::random_device{}() };
        return rng;
    }

    // Defined below, beside the level policy each one applies.
    void RollActor(RE::Actor* a_actor, bool a_force);
    void RollContainer(RE::TESObjectREFR* a_refr, bool a_force, bool a_stock, std::size_t a_cap);

    struct Candidate
    {
        RE::TESBoundObject* object{ nullptr };
        RE::ExtraDataList*  xList{ nullptr };
    };
    // Everything on this reference worth affixing -- worn OR not.
    //
    // ★Worn gear always has an ExtraDataList -- the engine makes one to hold
    // ExtraWorn -- which is why the first version could get away with only
    // reading. Unequipped items may have none, and those are counted rather than
    // given one; see the note at the skip below.
    std::vector<Candidate> AffixableItems(RE::TESObjectREFR* a_refr, bool& a_hadInventory,
        std::size_t& a_noExtraList, std::size_t a_cap)
    {
        std::vector<Candidate> found;

        // NO-INIT. The default creates and populates inventory data, which froze
        // the game when it ran across a worldspace full of actors.
        auto* changes = a_refr->GetInventoryChanges(true);
        if (!changes || !changes->entryList) {
            a_hadInventory = false;
            return found;
        }
        a_hadInventory = true;

        for (auto* entry : *changes->entryList) {
            if (!entry || !entry->object) {
                continue;
            }
            if (!entry->object->Is(RE::FormType::Weapon) &&
                !entry->object->Is(RE::FormType::Armor)) {
                continue;
            }

            // ★The entry's own guards, which are better than reading extra data
            // by hand: IsQuestObject covers quest items more thoroughly than a
            // kAliasInstanceArray probe, and IsEnchanted catches an instance
            // enchantment as well as the record's EITM.
            if (entry->IsQuestObject() || entry->IsFavorited() || entry->IsEnchanted()) {
                continue;
            }

            // Reuse a list that has no enchantment on it yet -- the worn one for
            // equipped gear, or a tempered item's existing data.
            RE::ExtraDataList* target = nullptr;
            if (entry->extraLists) {
                for (auto* xList : *entry->extraLists) {
                    if (xList && !xList->HasType<RE::ExtraEnchantment>()) {
                        target = xList;
                        break;
                    }
                }
            }

            if (!target) {
                // ★NOT CREATED, and deliberately so -- for now.
                //
                // RE::ExtraDataList's constructor is declared but not implemented
                // in this CommonLibSSE build, so one cannot simply be new'd. It
                // could be faked: the class is a BaseExtraList plus a lock, so
                // zeroed memory from the GAME's allocator (never ours -- the
                // static-CRT split means the heaps differ) would probably serve.
                //
                // "Probably" is doing far too much work in that sentence. The
                // failure mode is corruption inside the player's inventory, and
                // there is no way to verify the engine's invariants short of
                // playing and hoping.
                //
                // So: count them instead. If most container items already carry
                // an ExtraDataList -- for ownership, count, or temper -- the
                // dangerous path is unnecessary. That is a measurement, and it
                // costs one run.
                ++a_noExtraList;
                continue;
            }

            found.push_back({ entry->object, target });
            if (found.size() >= a_cap) {
                logger::warn("distribute: {:08X} hit the {}-item cap; the rest is unaffixed",
                    a_refr->GetFormID(), a_cap);
                return found;
            }
        }

        return found;
    }

    void RollRef(RE::TESObjectREFR* a_refr, int a_itemLevel, bool a_forNpc, bool a_force,
        std::size_t a_cap = kMaxItemsPerActor)
    {
        if (!a_refr || !g_table || g_table->Empty()) {
            return;
        }

        // The player is never a distribution target: their gear is what they
        // chose, and rolling it would rewrite the character's equipment on the
        // first cell load.
        if (a_refr->IsPlayerRef()) {
            return;
        }

        const auto formID = a_refr->GetFormID();

        {
            std::scoped_lock lock{ g_statsLock };
            ++g_stats.actorsSeen;
        }

        if (!a_force && Persist::WasRolled(formID)) {
            std::scoped_lock lock{ g_statsLock };
            ++g_stats.skippedAlreadyRolled;
            return;
        }

        const auto started = std::chrono::steady_clock::now();

        // ★GATHER BEFORE MARKING, and this ordering is load-bearing.
        //
        // Sweeping the low process lists reaches actors whose inventory is not
        // populated yet. Marking one of those would be PERMANENT: it would come
        // into high process later, carrying real gear, and be skipped forever
        // because the set already remembers it. An actor with nothing to examine
        // is DEFERRED, not consumed.
        bool        hadInventory = false;
        std::size_t noExtraList = 0;
        const auto  gear = AffixableItems(a_refr, hadInventory, noExtraList, a_cap);
        if (gear.empty()) {
            std::scoped_lock lock{ g_statsLock };
            ++g_stats.deferredNoGear;
            if (hadInventory) {
                ++g_stats.deferredNotWornYet;
            }
            return;
        }

        // Test-and-set. The WasRolled above is only a cheap pre-filter; this is
        // the call that actually claims the actor, and it is atomic so two
        // sweeps cannot both roll the same one.
        if (!a_force && !Persist::MarkRolled(formID)) {
            std::scoped_lock lock{ g_statsLock };
            ++g_stats.skippedAlreadyRolled;
            return;
        }


        std::size_t affixed = 0;
        std::size_t ineligible = 0;
        std::size_t examined = 0;
        std::size_t white = 0;
        std::size_t enchantedSkip = 0;

        for (const auto& candidate : gear) {
            ++examined;
            if (!Apply::IsEligible(candidate.object, candidate.xList)) {
                ++ineligible;
                continue;
            }
            const auto slots = Apply::SlotsOf(candidate.object);
            if (slots == roll::kNone) {
                ++ineligible;
                continue;
            }

            const roll::RollContext ctx{ a_itemLevel, slots, a_forNpc };
            const auto rolled = roll::Roll(*g_table, ctx, g_tuning, ThreadRng());
            if (rolled.affixes.empty()) {
                ++white;  // a legitimate outcome, but it should be countable
                continue;
            }

            const auto applied = Apply::ToItem(rolled, candidate.object, candidate.xList);
            if (!applied) {
                // Vanilla-enchanted: skipped by design. Not an error, and not
                // pending work either -- those items keep their own identity.
                ++enchantedSkip;
            } else {
                ++affixed;
                // The band, recorded against the enchantment we just created so
                // another mod's UI can ask about it while it draws. Kept here
                // rather than inside ToItem because the BAND is a presentation
                // decision and ToItem's job is the enchantment -- and because
                // this is where rolled.tier is already in hand.
                Persist::NoteEnchTier(applied.enchantment->GetFormID(),
                    static_cast<std::uint8_t>(roll::BandOf(rolled.tier)));
            }
        }

        const auto micros = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started)
                .count());

        std::scoped_lock lock{ g_statsLock };
        ++g_stats.actorsRolled;
        g_stats.itemsAffixed += affixed;
        g_stats.skippedIneligible += ineligible;
        g_stats.itemsExamined += examined;
        g_stats.rolledWhite += white;
        g_stats.skippedEnchanted += enchantedSkip;
        g_stats.wantedExtraList += noExtraList;
        g_stats.totalMicros += micros;
        g_stats.worstMicros = std::max(g_stats.worstMicros, micros);
    }

    // ★THE RE-SWEEP, and its absence is what left half of Solitude vanilla.
    //
    // An actor deferred for having no worn gear is never revisited by anything:
    // it is already loaded, so TESObjectLoadedEvent will not fire again, and the
    // enable-time sweep has been and gone. It only becomes affixable when it
    // reaches HIGH process and the game applies its equipment -- an event we get
    // no notification for.
    //
    // So: poll, but only the high list. High process is the actors near the
    // player -- tens, not the 3200 the full sweep walks -- so this is cheap
    // enough to run on a timer and is exactly where the transition lands.
    std::atomic<bool> g_ticking{ false };

    // Long enough to be free, short enough that gear is affixed before the
    // player has walked up and swung at someone.
    constexpr auto kTickInterval = std::chrono::milliseconds{ 2000 };

    void SweepHighOnly()
    {
        auto* lists = RE::ProcessLists::GetSingleton();
        if (!lists) {
            return;
        }
        for (auto& handle : lists->highActorHandles) {
            auto actor = handle.get();
            if (actor) {
                RollActor(actor.get(), false);
            }
        }
    }

    // A TIMER THREAD, not a self-rearming task -- and this froze the game.
    //
    // The first version had the task re-queue itself: AddTask, run, AddTask,
    // run. SKSE drains its task queue within a single frame, so a task that
    // re-adds itself is executed again in the same drain. That is an infinite
    // loop which never yields the frame, and the game hangs on the frame after
    // the toggle. It cost two misdiagnosed builds, both blaming the sweep.
    //
    // Pacing from a separate thread makes each task a genuine one-shot: the
    // thread sleeps, posts ONE task, sleeps again. Nothing re-queues anything.
    void StartTicker()
    {
        bool expected = false;
        if (!g_ticking.compare_exchange_strong(expected, true)) {
            return;  // already running
        }

        std::thread([]() {
            while (true) {
                std::this_thread::sleep_for(kTickInterval);
                if (!g_enabled.load()) {
                    continue;  // idle, but stay alive so re-enabling is instant
                }
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([]() { SweepHighOnly(); });
                }
            }
        }).detach();
    }

    // An NPC rolls at its OWN level, so a leveled bandit chief outdrops a
    // mudcrab with no authoring at all.
    void RollActor(RE::Actor* a_actor, bool a_force)
    {
        if (!a_actor) {
            return;
        }
        RollRef(a_actor, static_cast<int>(a_actor->GetLevel()), true, a_force);
    }

    // The level a container's contents should be rolled at.
    //
    // ★READ FROM THE RUNTIME RECORD, which is what makes this obey the load
    // order rather than vanilla. BGSEncounterZone is looked up live, so whichever
    // plugin last overrode that zone -- a levelling overhaul, a difficulty mod,
    // a patch -- is the one whose numbers we get. Nothing here hardcodes a
    // vanilla value.
    int EncounterLevelFor(RE::TESObjectREFR* a_refr)
    {
        auto*     player = RE::PlayerCharacter::GetSingleton();
        const int playerLevel = player ? static_cast<int>(player->GetLevel()) : 1;

        auto* cell = a_refr ? a_refr->GetParentCell() : nullptr;

        // ★TWO PLACES TO LOOK, and both are needed.
        //
        // The cell's AUTHORED zone lives in its extraList as ExtraEncounterZone;
        // the loaded copy lives in LOADED_CELL_DATA, which only exists while the
        // cell is loaded. Reading only the latter works here -- a container that
        // just fired a load event is in a loaded cell -- but it would silently
        // return nothing for any other caller, so take whichever is there.
        RE::BGSEncounterZone* zone = nullptr;
        if (cell) {
            if (auto* fromExtra = cell->extraList.GetByType<RE::ExtraEncounterZone>()) {
                zone = fromExtra->zone;
            }
            // GetRuntimeData, not a bare member: loadedData sits at a different
            // offset on SE and AE, and the accessor is what picks the right one.
            if (!zone) {
                auto& runtime = cell->GetRuntimeData();
                if (runtime.loadedData) {
                    zone = runtime.loadedData->encounterZone;
                }
            }
        }

        if (!zone) {
            // Most exteriors have no zone at all. Player level is the honest
            // fallback: there is no authored intent to respect.
            return playerLevel;
        }

        // ★A ZONE THAT HAS ALREADY LOCKED IN WINS OUTRIGHT.
        //
        // Skyrim fixes an encounter zone's level the first time the player
        // enters it, and stores it in gameData.zoneLevel. Recomputing from the
        // record would ignore that and scale a dungeon you cleared at level 12
        // to your level 40 self on a return visit -- which is precisely what
        // encounter zones exist to prevent.
        if (zone->gameData.zoneLevel > 0) {
            return static_cast<int>(zone->gameData.zoneLevel);
        }

        const int minLevel = zone->data.minLevel;
        const int maxLevel = zone->data.maxLevel;

        int level = playerLevel;
        if (playerLevel < minLevel) {
            // The flag means "if the player is under the floor, match them
            // anyway" -- an easy zone staying easy rather than snapping up.
            level = zone->data.flags.any(RE::ENCOUNTER_ZONE_DATA::Flag::kMatchPCBelowMinimumLevel)
                ? playerLevel
                : minLevel;
        }
        if (maxLevel > 0) {
            level = (std::min)(level, maxLevel);
        }
        return (std::max)(level, 1);
    }

    // ★A CAP ON THE EXTRA, in the same spirit as kMaxItemsPerActor and for the
    // same reason. A multiplier applies to whatever the container was authored
    // with, and Skyrim has containers authored with a lot -- a merchant chest
    // resolves dozens of lists. Ten times dozens, added one call at a time
    // while the player waits for a menu, is a stutter with a number attached.
    constexpr std::size_t kMaxExtraItemsPerContainer = 64;

    // ★HOW MANY EXTRA RESOLUTIONS, from a multiplier that need not be a whole
    // number. 3.0 is two extra passes every time; 1.5 is one extra pass half
    // the time. The fractional part is a coin flip rather than a rounding,
    // because rounding 1.5 to "one extra pass, always" makes every value in
    // [1.5, 2.5) mean the same thing and quietly removes half the dial.
    int ExtraPasses(float a_multiplier)
    {
        const float extra = a_multiplier - 1.0f;
        if (extra <= 0.0f) {
            return 0;
        }
        int         passes = static_cast<int>(extra);
        const float frac = extra - static_cast<float>(passes);
        if (frac > 0.0f) {
            std::uniform_real_distribution<float> coin{ 0.0f, 1.0f };
            if (coin(ThreadRng()) < frac) {
                ++passes;
            }
        }
        return passes;
    }

    // ★MORE LOOT, by asking the engine for the same thing again.
    //
    // A container's base record holds what the level designer wrote: some plain
    // items and some LEVELED ITEM entries. Vanilla resolves each leveled entry
    // ONCE, when the reference's inventory is first initialised, and that
    // resolution is the whole random-loot system. Running it again with the same
    // level produces exactly what a second chest in that room would have held --
    // which is why this needs no balance table of its own. The engine's own
    // list, at the engine's own level, drawn again.
    //
    // ★ONLY THE LEVELED ENTRIES. A plain entry in the base record is a decision
    // somebody made about THIS container -- the Amulet of Kings in this chest,
    // this key, this note. Multiplying those does not make the game more
    // generous, it makes it incoherent, and for a quest item it makes it broken.
    // Anything that is not a TESLevItem is left exactly alone.
    //
    // Returns how many items were added, for the stats.
    std::size_t StockContainer(RE::TESObjectREFR* a_refr, int a_level)
    {
        const int passes = ExtraPasses(Config::ContainerLootMultiplier());
        if (passes <= 0) {
            return 0;
        }

        // ★A CORPSE IS NOT A CHEST, though the game shows you the same menu for
        // both -- ContainerMenu opens over a dead bandit exactly as it does over
        // a barrel, so this function sees actors whether it wants them or not.
        //
        // An NPC's base record carries leveled entries too, and rolling those
        // again hands the player three copies of the sword the bandit is still
        // holding: the entry that armed him and the entries that duplicate it
        // are the same entry. The setting says CONTAINER, so containers it is.
        if (a_refr->As<RE::Actor>()) {
            return 0;
        }

        auto* base = a_refr->GetContainer();
        if (!base) {
            return 0;  // not a container at all; nothing authored to re-roll
        }

        // ★VANILLA'S ROLL FIRST, EXPLICITLY, and the ordering is not cosmetic.
        //
        // An unopened chest has no ExtraContainerChanges: its contents are still
        // a promise held in the base record. AddObjectToContainer would create
        // that structure to hold OUR items -- and a container whose changes
        // already exist is a container the engine considers initialised, so the
        // authored contents could never arrive. The chest would hold the bonus
        // loot and nothing else, which is a multiplier that SUBTRACTS.
        //
        // GetInventoryChanges with init -- NOT the no-init form the affix pass
        // uses, and not InitInventoryIfRequired by hand either. The initialising
        // overload is the one that falls back to ForceInitInventoryChanges when
        // the first attempt declines, which is the case for a container the
        // engine does not think needs it yet. After this line the vanilla
        // contents are real and we are strictly adding to them.
        if (!a_refr->GetInventoryChanges()) {
            return 0;
        }

        const auto level = static_cast<std::uint16_t>(std::clamp(a_level, 1, 0xFFFF));

        std::size_t added = 0;
        base->ForEachContainerObject([&](RE::ContainerObject& a_entry) {
            if (!a_entry.obj) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            auto* list = a_entry.obj->As<RE::TESLevItem>();
            if (!list || a_entry.count <= 0) {
                return RE::BSContainer::ForEachResult::kContinue;  // hand-placed
            }

            for (int pass = 0; pass < passes; ++pass) {
                // Resolved fresh each pass rather than resolved once and added
                // N times: the point is another ROLL, not another copy. Two
                // passes on a bandit chest should be able to give a sword and a
                // helmet, not the same sword twice.
                RE::BSScrapArray<RE::CALCED_OBJECT> calced;
                list->CalculateCurrentFormList(level,
                    static_cast<std::int16_t>(a_entry.count), calced, 0, false);

                for (const auto& calc : calced) {
                    if (!calc.form || calc.count == 0) {
                        continue;
                    }
                    auto* bound = calc.form->As<RE::TESBoundObject>();
                    if (!bound) {
                        continue;
                    }
                    a_refr->AddObjectToContainer(bound, nullptr,
                        static_cast<std::int32_t>(calc.count), nullptr);
                    added += calc.count;

                    if (added >= kMaxExtraItemsPerContainer) {
                        logger::warn(
                            "distribute: {:08X} hit the {}-item bonus cap; the rest of the "
                            "multiplier is dropped",
                            a_refr->GetFormID(), kMaxExtraItemsPerContainer);
                        return RE::BSContainer::ForEachResult::kStop;
                    }
                }
            }

            return RE::BSContainer::ForEachResult::kContinue;
        });

        return added;
    }

    // A container has no level of its own, so it rolls at its encounter zone's --
    // which is what makes a Nordic ruin's loot match the ruin rather than the
    // player who wandered in. The same level feeds the bonus resolutions, so the
    // extra loot belongs to the dungeon on exactly the terms its own loot does.
    // a_stock: whether the loot multiplier applies. Off for a merchant chest --
    // a shop restocked three times over is a shop giving things away, and the
    // multiplier is about dungeons. a_cap: how many items the affix pass may
    // touch.
    void RollContainer(RE::TESObjectREFR* a_refr, bool a_force, bool a_stock, std::size_t a_cap)
    {
        if (!a_refr) {
            return;
        }

        // ★THE CLAIM IS TAKEN HERE, not left to RollRef, and moving it is what
        // makes the multiplier safe to add.
        //
        // RollRef deliberately does NOT mark a reference it found nothing to
        // affix on -- an actor out of high process has no gear YET, and marking
        // it would skip it forever. A container has no such "yet": we are about
        // to initialise its inventory, and what comes back is final. Left on
        // RollRef's terms, a chest holding only potions would go unmarked, and
        // an unmarked chest is RESTOCKED on every single open -- a duplication
        // bug that compounds for as long as the save lives.
        //
        // One claim, taken before anything is added, covering both halves.
        if (!a_force && !Persist::MarkRolled(a_refr->GetFormID())) {
            std::scoped_lock lock{ g_statsLock };
            ++g_stats.skippedAlreadyRolled;
            return;
        }

        const int level = EncounterLevelFor(a_refr);

        // Stocked BEFORE the affix pass, so the bonus items are in the inventory
        // when it walks them. The other order gives the player a fuller chest of
        // entirely plain gear.
        const auto added = a_stock ? StockContainer(a_refr, level) : 0;
        if (added) {
            std::scoped_lock lock{ g_statsLock };
            ++g_stats.containersStocked;
            g_stats.extraItemsAdded += added;
        }

        // Forced: the claim above already decided this container gets rolled,
        // and RollRef must not re-ask a question that is now answered "yes".
        const auto before = g_stats.actorsRolled;
        RollRef(a_refr, level, false, true, a_cap);
        if (g_stats.actorsRolled != before) {
            std::scoped_lock lock{ g_statsLock };
            ++g_stats.containersRolled;
        }
    }

    // ★CONTAINERS ROLL WHEN OPENED, not when loaded.
    //
    // The load-event path cannot reach them. AffixableItems asks for inventory
    // with no-init -- which is what stopped the game freezing across a
    // worldspace -- and an unopened chest HAS no InventoryChanges: its contents
    // sit in the base TESContainer until something touches them. Every container
    // therefore came back empty and was deferred forever.
    //
    // Opening the menu is the moment the engine materialises that inventory, so
    // it is both the earliest point the items exist and the last point before
    // the player sees them. It is also naturally lazy: Skyrim has tens of
    // thousands of containers and this touches only the ones actually opened.
    class ContainerMenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static ContainerMenuSink* GetSingleton()
        {
            static ContainerMenuSink singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent*      a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (!a_event || !a_event->opening || !g_enabled.load()) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (a_event->menuName == RE::BarterMenu::MENU_NAME) {
                RollVendor();
                return RE::BSEventNotifyControl::kContinue;
            }
            if (a_event->menuName != RE::ContainerMenu::MENU_NAME) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Rolled inline rather than deferred to a task: the menu is being
            // built right now, and a task would land a frame later -- after the
            // list the player is looking at has already been drawn.
            const auto handle = RE::ContainerMenu::GetTargetRefHandle();
            const auto ref = RE::TESObjectREFR::LookupByHandle(handle);
            if (ref) {
                {
                    std::scoped_lock lock{ g_statsLock };
                    ++g_stats.containersOpened;
                }
                RollContainer(ref.get(), false, true, kMaxItemsPerActor);
            }

            return RE::BSEventNotifyControl::kContinue;
        }

        // ★A VENDOR'S STOCK LIVES IN A CHEST THE PLAYER NEVER OPENS. The
        // barter menu reads from the merchant container hung off the vendor's
        // faction, in a cell nobody visits, so the container path above never
        // sees it. This reaches in by that route on every barter and rolls the
        // chest the same way -- minus the loot multiplier, and with a cap
        // sized for a shop.
        //
        // ★ROLLED AGAIN AFTER EVERY RESTOCK, and the restock is read from the
        // faction rather than guessed from a timer. The engine refills the
        // chest on its own schedule -- two days for most vendors -- and stamps
        // the day on the faction when it does. No reset event announces it,
        // so the rolled-once mark alone would leave every restock plain. The
        // day the chest was last rolled at is kept in the co-save beside the
        // mark; a different day now means fresh stock, and the chest is rolled
        // as if for the first time. Items that already carry an enchantment
        // are skipped by the affix pass, so a same-day repeat costs nothing.
        static void RollVendor()
        {
            if (!Config::VendorStockEnabled()) {
                return;
            }
            const auto handle = RE::BarterMenu::GetTargetRefHandle();
            const auto ref = RE::TESObjectREFR::LookupByHandle(handle);
            auto*      vendor = ref ? ref->As<RE::Actor>() : nullptr;
            if (!vendor) {
                return;
            }
            auto* faction = vendor->GetVendorFaction();
            auto* chest = faction ? faction->vendorData.merchantContainer : nullptr;
            if (!chest) {
                // A vendor selling from their own pockets. Those were rolled
                // when the actor loaded, like any other NPC's gear.
                return;
            }

            const auto chestID = chest->GetFormID();
            const auto day = faction->vendorData.lastDayReset;
            if (Persist::WasRolled(chestID) && Persist::VendorDay(chestID) == day) {
                std::scoped_lock lock{ g_statsLock };
                ++g_stats.skippedAlreadyRolled;
                return;
            }

            logger::info("distribute: vendor {:08X} ({}) restocked on day {}; rolling chest {:08X}",
                vendor->GetFormID(), vendor->GetName(), day, chestID);
            Persist::MarkRolled(chestID);
            Persist::NoteVendorDay(chestID, day);
            {
                std::scoped_lock lock{ g_statsLock };
                ++g_stats.containersOpened;
            }
            RollContainer(chest, true, false, kMaxItemsPerVendor);
        }
    };

    // ★THE WAY BACK OUT OF THE ROLLED SET.
    //
    // Everything else here is about rolling a reference exactly once. This is
    // the one thing that says "once" has expired: Skyrim resets references on
    // the encounter zone's respawn timer, regenerating a chest's contents from
    // its leveled lists while the reference keeps the FormID the mark is keyed
    // on. A dungeon can be rerun, so its chests should be rollable again --
    // otherwise the mod's own loot is a one-time event per container and every
    // subsequent visit is more vanilla than the last.
    //
    // ★THE ENGINE'S OWN NOTIFICATION, NOT A TIMER, and that choice is the whole
    // safety argument. Expiring marks on a guessed interval would eventually
    // clear one for a container that had NOT reset, and the next open would
    // stock it a second time on top of contents it still had -- duplication
    // that compounds for the life of the save. This event fires because the
    // reset happened; it cannot be early.
    class ResetSink : public RE::BSTEventSink<RE::TESResetEvent>
    {
    public:
        static ResetSink* GetSingleton()
        {
            static ResetSink singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESResetEvent*      a_event,
            RE::BSTEventSource<RE::TESResetEvent>*) override
        {
            if (!a_event || !a_event->object) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // ★NO g_enabled CHECK, and the omission is deliberate. Every other
            // sink here bails when distribution is off because it is about to
            // ROLL something. This one only forgets, and a mark left standing
            // through a reset because the mod happened to be switched off at
            // that moment is a mark that is wrong for the rest of the save --
            // nothing ever revisits it.
            const auto formID = a_event->object->GetFormID();
            if (Persist::ForgetRolled(formID)) {
                std::scoped_lock lock{ g_statsLock };
                ++g_stats.marksForgotten;
            }

            // ★ACTORS TOO, not just containers, and for the same reason rather
            // than a different one. A respawned bandit is a fresh inventory
            // wearing a familiar FormID; leaving him marked is how a rerun
            // dungeon fills with vanilla gear. RollRef still defers an actor
            // whose equipment has not been applied yet, so an early forget
            // costs nothing -- he is rolled when the ticker next sees him
            // carrying something.
            //
            // The mapping to enchantment tiers is deliberately NOT touched
            // here. Those entries are keyed on created enchantments the engine
            // is destroying along with the items; whether it also releases the
            // created-object refcount is not something this code knows, and
            // releasing one the engine has already released is a far worse
            // failure than a stale map entry. See Apply::Release.
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class ActorLoadSink : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
    {
    public:
        static ActorLoadSink* GetSingleton()
        {
            static ActorLoadSink singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent*      a_event,
            RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
        {
            if (!a_event || !a_event->loaded || !g_enabled.load()) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // ★CAPTURE THE FORMID, NOT THE POINTER, and re-look-it-up in the
            // task. The event fires during load; by the time the task runs the
            // reference may have been unloaded again, and a captured pointer
            // would be dangling. The event only carries an id for exactly this
            // reason.
            const auto formID = a_event->formID;

            // ★DEFERRED. This fires on a game thread in the middle of loading a
            // reference; mutating inventory here is how you get an intermittent
            // crash that reproduces on someone else's machine and not yours.
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([formID]() {
                    auto* form = RE::TESForm::LookupByID(formID);
                    if (!form) {
                        return;
                    }
                    if (auto* actor = form->As<RE::Actor>()) {
                        RollActor(actor, false);
                        return;
                    }
                    // Containers deliberately do NOT roll here. They fire this
                    // event, but at load time an unopened chest has no
                    // InventoryChanges to read -- see ContainerMenuSink, which
                    // catches them at the one moment the contents exist.
                });
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };
}

void Distribute::Install()
{
    auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
    if (!holder) {
        logger::error("distribute: no script event source holder; nothing will be distributed");
        return;
    }

    holder->AddEventSink<RE::TESObjectLoadedEvent>(ActorLoadSink::GetSingleton());
    holder->AddEventSink<RE::TESResetEvent>(ResetSink::GetSingleton());

    if (auto* ui = RE::UI::GetSingleton()) {
        ui->AddEventSink<RE::MenuOpenCloseEvent>(ContainerMenuSink::GetSingleton());
    } else {
        logger::error("distribute: no UI singleton; containers will not roll");
    }
    logger::info("distribute: actor-loaded sink installed (currently {})",
        g_enabled.load() ? "ENABLED" : "disabled -- turn it on deliberately");
}

void Distribute::SetTable(const roll::AffixTable& a_table)
{
    g_table = &a_table;
}

void Distribute::SweepLoaded()
{
    if (!g_enabled.load()) {
        return;
    }

    auto* lists = RE::ProcessLists::GetSingleton();
    if (!lists) {
        return;
    }

    std::size_t swept = 0;

    // HIGH AND MIDDLE-HIGH ONLY, not all four lists.
    //
    // The all-lists version walked every actor in the worldspace -- 3200 in a
    // city, far more outdoors -- and combined with the GetInventoryChanges
    // default above it froze the game on enable. It was also pointless work:
    // low-process actors have no worn equipment to read, so they were all
    // deferred anyway.
    //
    // The periodic high-process tick is what actually catches actors, as they
    // come into range. This sweep only exists so enabling mid-session does not
    // wait a tick for the people already standing in front of you.
    for (auto* list : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
        for (auto& handle : *list) {
            auto actor = handle.get();
            if (actor) {
                RollActor(actor.get(), false);
                ++swept;
            }
        }
    }

    logger::info("distribute: swept {} already-loaded actor(s)", swept);
}

void Distribute::SetEnabled(bool a_enabled)
{
    g_enabled.store(a_enabled);
    logger::info("distribute: {}", a_enabled ? "ENABLED" : "disabled");

    // Catch everyone already standing around, rather than leaving them vanilla
    // until they happen to unload.
    if (a_enabled) {
        SweepLoaded();
        StartTicker();
    }
}

bool Distribute::Enabled()
{
    return g_enabled.load();
}

Distribute::Stats Distribute::CurrentStats()
{
    std::scoped_lock lock{ g_statsLock };
    return g_stats;
}

void Distribute::LogStats()
{
    const auto stats = CurrentStats();
    logger::info("---- distribution ----");
    logger::info("  actors seen      {}", stats.actorsSeen);
    logger::info("  actors rolled    {}", stats.actorsRolled);
    logger::info("  skipped (rolled) {}", stats.skippedAlreadyRolled);
    logger::info("  deferred (no gear yet) {}   ({} had an inventory but nothing worn)",
        stats.deferredNoGear, stats.deferredNotWornYet);
    logger::info("  items examined   {}", stats.itemsExamined);
    logger::info("  skipped, no ExtraDataList to attach to: {}", stats.wantedExtraList);
    logger::info("  containers opened {}, rolled {}", stats.containersOpened,
        stats.containersRolled);
    logger::info("    stocked        {}   (+{} bonus items at x{:.2f})", stats.containersStocked,
        stats.extraItemsAdded, Config::ContainerLootMultiplier());
    logger::info("  marks forgotten  {}   (references the game reset -- respawned dungeons)",
        stats.marksForgotten);
    logger::info("    affixed        {}", stats.itemsAffixed);
    logger::info("    rolled white   {}", stats.rolledWhite);
    logger::info("    enchanted      {}   (vanilla enchantment, skipped by design)",
        stats.skippedEnchanted);
    logger::info("    ineligible     {}   (quest item, or slot already taken)", stats.skippedIneligible);

    // If these disagree, an item is taking a path nobody is counting -- which is
    // how a silent bug hides inside a plausible-looking number.
    const auto tallied = stats.itemsAffixed + stats.rolledWhite + stats.skippedEnchanted +
        stats.skippedIneligible;
    if (tallied != stats.itemsExamined) {
        logger::warn("  ACCOUNTING GAP: {} examined but {} accounted for", stats.itemsExamined,
            tallied);
    }
    if (stats.actorsRolled) {
        logger::info("  mean {} us/actor, worst {} us", stats.totalMicros / stats.actorsRolled,
            stats.worstMicros);
    }
}
