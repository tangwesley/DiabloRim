// =============================================================================
//  Phase 5b -- quest rewards. See QuestReward.h.
// =============================================================================

#include "PCH.h"

#include "QuestReward.h"

#include "Apply.h"
#include "Notify.h"
#include "Persist.h"
#include "roll/Roll.h"

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

    std::atomic<bool> g_enabled{ false };

    std::mutex         g_statsLock;
    QuestReward::Stats g_stats;

    roll::Rng& ThreadRng()
    {
        static thread_local roll::Rng rng{ std::random_device{}() };
        return rng;
    }

    // ------------------------------------------------------------ the queue
    //
    // ★AN EVENT IS A NOTICE, NOT A MOMENT TO ACT ON, and learning that cost a
    // duplicated-looking item and then two crashes.
    //
    // TESContainerChangedEvent announces a change the engine has NOT finished
    // making. Acting inside it -- even from an SKSE task, which lands later in
    // the same frame -- means touching structures the engine is still
    // rebuilding. So the sink copies a FormID and returns, and the work happens
    // from a timer once the inventory has stopped moving.
    struct Pending
    {
        RE::FormID                            baseObj{ 0 };
        std::chrono::steady_clock::time_point at{};
    };

    std::mutex           g_pendingLock;
    std::vector<Pending> g_pending;

    // How long an entry must sit before it is touched. The mutation is over
    // within a frame or two; this is generous because being early costs an
    // inventory and being late costs a second.
    constexpr auto kSettle = std::chrono::milliseconds{ 1000 };

    // How often the drain looks. Independent of kSettle on purpose: the tick
    // rate decides latency, the settle time decides safety, and tangling them
    // means a change to one silently moves the other.
    constexpr auto kTickInterval = std::chrono::milliseconds{ 500 };

    constexpr std::size_t kMaxPending = 64;

    std::atomic<bool> g_ticking{ false };

    // Whether the engine is somewhere it is likely still moving items around.
    //
    // ★A SECOND LINE, NOT THE ONLY ONE. The settle time covers the ordinary
    // case; this covers the one where the player is standing in a menu that
    // moves items in bursts -- a dialogue granting several rewards, a barter,
    // a container transfer. Waiting for it to close costs only latency and
    // takes the whole class of "mid-handover" off the table.
    bool InventoryIsBusy()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return true;  // cannot tell: assume the worse of the two
        }
        return ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME) ||
               ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) ||
               ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME) ||
               ui->IsMenuOpen(RE::BarterMenu::MENU_NAME) ||
               ui->IsMenuOpen(RE::GiftMenu::MENU_NAME) ||
               ui->IsMenuOpen(RE::CraftingMenu::MENU_NAME);
    }

    // ---------------------------------------------------------- the guards
    //
    // Whether the player's inventory says to leave this alone.
    //
    // ★ASKED OF THE ENTRY, NOT OF EXTRA DATA. InventoryEntryData::IsQuestObject
    // covers quest items more thoroughly than a kAliasInstanceArray probe -- the
    // same reason Distribute uses it -- and it is the guard that keeps this path
    // off an item a quest is going to ask for back. A reward normally carries no
    // alias at all, so this refuses rarely; when it does, the quest had a reason
    // to name that instance and we are not going to argue with it.
    //
    // Safe to walk here and NOT in the event, which is the distinction the
    // crashes taught: by drain time the engine has finished with the list.
    bool RefusedByEntry(RE::TESObjectREFR* a_refr, RE::TESBoundObject* a_object)
    {
        // NO-INIT. The init path populates inventory data as a side effect -- it
        // is a writer, not a reader -- and this function only asks questions.
        auto* changes = a_refr->GetInventoryChanges(true);
        if (!changes || !changes->entryList) {
            return false;
        }
        for (auto* entry : *changes->entryList) {
            if (!entry || entry->object != a_object) {
                continue;
            }
            return entry->IsQuestObject() || entry->IsFavorited() || entry->IsEnchanted();
        }
        // Not in the inventory any more -- sold or dropped while it settled.
        return true;
    }

    void RollReward(RE::TESBoundObject* a_object)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !a_object || !g_table || g_table->Empty()) {
            return;
        }

        const auto bump = [](std::uint64_t QuestReward::Stats::* a_field) {
            std::scoped_lock lock{ g_statsLock };
            ++(g_stats.*a_field);
        };

        if (RefusedByEntry(player, a_object) || !Apply::IsEligible(a_object, nullptr)) {
            bump(&QuestReward::Stats::ineligible);
            return;
        }

        const auto slots = Apply::SlotsOf(a_object);
        if (slots == roll::kNone) {
            bump(&QuestReward::Stats::ineligible);
            return;
        }

        // ★PLAYER LEVEL, and there is no better number available. A container
        // rolls at its encounter zone's level because the zone is an authored
        // statement about how hard that place is; a reward has no such statement
        // attached, and the leveled list it came out of was already resolved
        // against the player when the quest handed it over.
        //
        // ★noWhite: A REWARD IS NEVER PLAIN. Everywhere else white is the
        // commonest outcome and should be -- a bandit's iron sword being just an
        // iron sword is what makes the marked ones worth finding. A reward is
        // the opposite: the player did something specific to be handed this
        // specific item, and an empty roll there reads as the mod being broken
        // rather than as luck.
        const roll::RollContext ctx{ static_cast<int>(player->GetLevel()), slots, false, true };
        const auto rolled = roll::Roll(*g_table, ctx, g_tuning, ThreadRng());
        if (rolled.affixes.empty()) {
            // Not a white roll -- noWhite has taken that outcome away. Reaching
            // here means no affix in the table fits this item's slots at this
            // level, which is a gap in the data rather than luck.
            bump(&QuestReward::Stats::rolledWhite);
            logger::info("quest reward: {:08X} has no affix available for slots {} at level {}",
                a_object->GetFormID(), roll::SlotsToString(slots), ctx.itemLevel);
            return;
        }

        const auto applied = Apply::ToNewInstance(rolled, a_object, player);
        if (!applied) {
            bump(&QuestReward::Stats::attachFailed);
            return;
        }

        Persist::NoteEnchTier(applied.enchantment->GetFormID(),
            static_cast<std::uint8_t>(roll::BandOf(rolled.tier)));

        {
            std::scoped_lock lock{ g_statsLock };
            ++g_stats.affixed;
        }

        logger::info("quest reward: {:08X} rolled tier {} -- {}", a_object->GetFormID(),
            rolled.tier, applied.name.empty() ? a_object->GetName() : applied.name.c_str());
    }

    void Drain()
    {
        if (!g_enabled.load() || InventoryIsBusy()) {
            return;  // try again next tick; the queue keeps
        }

        std::vector<Pending> ready;
        {
            const auto       now = std::chrono::steady_clock::now();
            std::scoped_lock lock{ g_pendingLock };
            for (auto it = g_pending.begin(); it != g_pending.end();) {
                if (now - it->at >= kSettle) {
                    ready.push_back(*it);
                    it = g_pending.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (ready.empty()) {
            return;
        }

        for (const auto& pending : ready) {
            auto* form = RE::TESForm::LookupByID(pending.baseObj);
            if (auto* object = form ? form->As<RE::TESBoundObject>() : nullptr) {
                RollReward(object);
            }
        }
    }

    // A TIMER THREAD posting one-shot tasks -- copied deliberately from
    // Distribute, including the reason it is not a self-rearming task. SKSE
    // drains its task queue within a frame, so a task that re-adds itself runs
    // again in the same drain and never yields; that hung the game once already
    // and there is no reason to rediscover it here.
    void StartTicker()
    {
        bool expected = false;
        if (!g_ticking.compare_exchange_strong(expected, true)) {
            return;
        }

        std::thread([]() {
            while (true) {
                std::this_thread::sleep_for(kTickInterval);
                if (!g_enabled.load()) {
                    continue;  // idle, but alive, so re-enabling is instant
                }
                {
                    std::scoped_lock lock{ g_pendingLock };
                    if (g_pending.empty()) {
                        continue;  // nothing owed; do not wake the main thread
                    }
                }
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([]() { Drain(); });
                }
            }
        }).detach();
    }

    // ★A REWARD IS ONE ITEM, NOT A PILE.
    //
    // A count above one is a supply drop -- twenty arrows, five potions -- and
    // the interesting case, the reward for finishing something, is a single
    // piece of gear. Everything else is counted and left alone.
    constexpr std::int32_t kMaxRewardCount = 1;

    class GrantSink : public RE::BSTEventSink<RE::TESContainerChangedEvent>
    {
    public:
        static GrantSink* GetSingleton()
        {
            static GrantSink singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
        {
            if (!a_event || !g_enabled.load()) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // ★THE WHOLE FILTER, AND EVERY CLAUSE EARNS ITS PLACE.
            //
            //   newContainer is the player   -- this path is only about rewards
            //   oldContainer is NOTHING      -- the item was CREATED here, not
            //                                   moved. Looting reports the
            //                                   corpse or chest; buying reports
            //                                   the merchant. Both are already
            //                                   served by Distribute.
            //   no source reference          -- picking an item up off the
            //                                   ground ALSO reports no old
            //                                   container, and is distinguished
            //                                   only by carrying the world
            //                                   reference it came from.
            //
            // What is left is "a script put this in your inventory", which is
            // what a quest reward is.
            constexpr RE::FormID kPlayer = 0x14;
            if (a_event->newContainer != kPlayer || a_event->oldContainer != 0 ||
                a_event->reference) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (a_event->itemCount <= 0 || a_event->itemCount > kMaxRewardCount) {
                if (a_event->itemCount > kMaxRewardCount) {
                    std::scoped_lock lock{ g_statsLock };
                    ++g_stats.granted;
                    ++g_stats.stacked;
                }
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* form = RE::TESForm::LookupByID(a_event->baseObj);
            if (!form) {
                return RE::BSEventNotifyControl::kContinue;
            }

            {
                std::scoped_lock lock{ g_statsLock };
                ++g_stats.granted;
            }

            if (!form->Is(RE::FormType::Weapon) && !form->Is(RE::FormType::Armor)) {
                std::scoped_lock lock{ g_statsLock };
                ++g_stats.notGear;
                return RE::BSEventNotifyControl::kContinue;
            }

            // ★SMITHING IS NOT A QUEST REWARD, and it reaches this event by
            // exactly the same route: the forge hands the finished item over
            // with AddItem, from nothing, into the player. Left in, every
            // crafted sword would roll -- a different feature with different
            // balance consequences, and one that should be decided on its own
            // merits rather than inherited by accident from this one.
            //
            // ★SAMPLED HERE, NOT LATER. The crafting menu is open at the instant
            // the forge hands the item over and may be closed by the time
            // anything deferred runs.
            if (auto* ui = RE::UI::GetSingleton();
                ui && ui->IsMenuOpen(RE::CraftingMenu::MENU_NAME)) {
                std::scoped_lock lock{ g_statsLock };
                ++g_stats.fromCrafting;
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* object = form->As<RE::TESBoundObject>();
            if (!object || !Apply::IsGeneric(object)) {
                std::scoped_lock lock{ g_statsLock };
                ++g_stats.notGeneric;
                return RE::BSEventNotifyControl::kContinue;
            }

            // ★QUEUED, NOT ROLLED. Nothing here touches the inventory: the
            // engine is still mid-way through the very change being announced,
            // and acting inside it cost two crashes. A FormID is all that leaves
            // this function; Drain does the work a second later.
            {
                std::scoped_lock lock{ g_pendingLock };
                if (g_pending.size() >= kMaxPending) {
                    logger::warn("quest reward: queue full at {}; dropping {:08X}", kMaxPending,
                        a_event->baseObj);
                    return RE::BSEventNotifyControl::kContinue;
                }
                g_pending.push_back({ a_event->baseObj, std::chrono::steady_clock::now() });
            }

            // ★ARMED HERE, BEFORE THE ENGINE ANNOUNCES THE ITEM. This runs
            // inside the container-changed event, which is the engine telling us
            // about the addition -- and the "Skyforge Steel Sword" notification
            // has not been queued yet. A frame later would be too late; the
            // player would have read it already.
            //
            // Arming with the BASE name covers both messages this sequence can
            // produce, and only the first one to arrive is swallowed. See
            // Notify.h for why "contains" rather than "equals".
            if (const char* name = object->GetName(); name && *name) {
                Notify::MuteNext(name);
            }

            logger::debug("reward queued: {:08X}", a_event->baseObj);
            return RE::BSEventNotifyControl::kContinue;
        }
    };

}

void QuestReward::Install()
{
    auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
    if (!holder) {
        logger::error("quest rewards: no script event source holder; rewards stay vanilla");
        return;
    }
    holder->AddEventSink<RE::TESContainerChangedEvent>(GrantSink::GetSingleton());

    logger::info("quest rewards: grant sink installed (currently {}), "
                 "{} generic record(s) known",
        g_enabled.load() ? "ENABLED" : "disabled", Apply::GenericCount());
}

void QuestReward::SetTable(const roll::AffixTable& a_table)
{
    g_table = &a_table;
}

void QuestReward::SetEnabled(bool a_enabled)
{
    g_enabled.store(a_enabled);
    logger::info("quest rewards: {}", a_enabled ? "ENABLED" : "disabled");
    if (a_enabled) {
        StartTicker();
    }
}

bool QuestReward::Enabled()
{
    return g_enabled.load();
}

void QuestReward::Forget()
{
    std::size_t dropped = 0;
    {
        std::scoped_lock lock{ g_pendingLock };
        dropped = g_pending.size();
        g_pending.clear();
    }
    if (dropped) {
        logger::info("quest rewards: dropped {} outstanding item(s) -- the world they belonged "
                     "to is being replaced",
            dropped);
    }
}

QuestReward::Stats QuestReward::CurrentStats()
{
    std::scoped_lock lock{ g_statsLock };
    return g_stats;
}

void QuestReward::LogStats()
{
    const auto stats = CurrentStats();
    if (!stats.granted) {
        return;  // nothing was handed over; a block of zeroes says less
    }

    // Queued but not yet settled. Small and short-lived, but it has to appear in
    // the tally or the accounting check fires every time a save lands inside the
    // settling second.
    std::size_t outstanding = 0;
    {
        std::scoped_lock lock{ g_pendingLock };
        outstanding = g_pending.size();
    }

    logger::info("---- quest rewards ----");
    logger::info("  granted          {}", stats.granted);
    logger::info("    affixed        {}", stats.affixed);
    logger::info("    no affix fits  {}   (nothing in the table matches the item's slots)",
        stats.rolledWhite);
    logger::info("    not gear       {}", stats.notGear);
    logger::info("    stacked        {}   (more than one at a time)", stats.stacked);
    logger::info("    from crafting  {}", stats.fromCrafting);
    logger::info("    not generic    {}   (in no leveled list -- a unique)", stats.notGeneric);
    logger::info("    ineligible     {}   (quest object, favourited, or gone)",
        stats.ineligible);
    if (stats.attachFailed) {
        logger::warn("    ATTACH FAILED  {}", stats.attachFailed);
    }
    logger::info("  still settling:  {}", outstanding);

    const auto tallied = stats.affixed + stats.rolledWhite + stats.notGear + stats.stacked +
        stats.fromCrafting + stats.notGeneric + stats.ineligible + stats.attachFailed +
        outstanding;
    if (tallied != stats.granted) {
        logger::warn("  ACCOUNTING GAP: {} granted but {} accounted for", stats.granted, tallied);
    }
}
