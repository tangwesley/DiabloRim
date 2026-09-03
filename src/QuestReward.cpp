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
    //
    // ★THIS IS THE ONE WITH A CRASH BEHIND IT, so it moves cautiously and it
    // moves alone. 500ms is still thirty frames against a mutation that is done
    // in one or two, and InventoryIsBusy covers the menu-driven bursts on its
    // own account -- but if anything ever faults in this path again, this is the
    // first number to put back to 1000 and the last one to touch afterwards.
    constexpr auto kSettle = std::chrono::milliseconds{ 500 };

    // How often the drain looks. Independent of kSettle on purpose: the tick
    // rate decides latency, the settle time decides safety, and tangling them
    // means a change to one silently moves the other.
    //
    // ★AND THIS ONE IS PURE DEAD TIME, WHICH IS WHY IT IS SMALL. It buys no
    // safety at all: an entry that is ready waits here for no reason but the
    // sleep. It was 500ms, and that was the whole of the visible delay in the
    // ordinary case -- by the time a dialogue has been read and closed the
    // settle has long since elapsed, so what the player was waiting on was the
    // thread getting round to looking. It is also what put a plain item in front
    // of anyone who opened their inventory to check: the drain cannot run while
    // that menu is up, so the roll landed a further half-second after it closed.
    //
    // The cost of looking often is a mutex and an empty test; the thread does
    // not wake the main thread at all unless something is owed.
    constexpr auto kTickInterval = std::chrono::milliseconds{ 100 };

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
    // Whether the player's inventory says to leave this alone, AND WHICH OF THE
    // REASONS IT WAS.
    //
    // ★THE REASON IS RETURNED RATHER THAN COLLAPSED TO A BOOL, and that is worth
    // the enum. These refusals have nothing to do with each other -- one is the
    // quest protecting its own item, one is the player having pinned a hotkey,
    // one is the item having left the inventory before it settled -- and folding
    // them into a single "ineligible" counter meant a reward that silently did
    // not roll gave the log no way to say why. That is exactly the question a
    // plain reward raises, so the log has to be able to answer it.
    //
    // ★ASKED OF THE ENTRY, NOT OF EXTRA DATA. InventoryEntryData::IsQuestObject
    // covers quest items more thoroughly than a kAliasInstanceArray probe -- the
    // same reason Distribute uses it -- and it is the guard that keeps this path
    // off an item a quest is going to ask for back. A reward normally carries no
    // alias at all, so this refuses rarely; when it does, the quest had a reason
    // to name that instance and we are not going to argue with it.
    //
    // ★BOTH ARE ASKED OF THE WHOLE STACK, AND ONLY THESE TWO ARE. An entry
    // covers every instance of a base record the player holds, and a script
    // grant arrives with no extra data of its own, so there is nothing here to
    // distinguish the new one by; Apply::ToNewInstance cannot aim either, since
    // it removes ONE of the record and the engine chooses which. Refusing the
    // whole stack is therefore the only answer available BEFORE the drop, and it
    // is kept for the two cases where the drop itself would already have done
    // the damage:
    //
    //   a quest alias -- an instance a quest is going to ask for back
    //   a hotkey      -- dropping a favourited item clears the favourite, and
    //                    handing it back does not restore it, so there is no
    //                    such thing as looking and then undoing
    //
    // ★AND NOT "ALREADY ENCHANTED", WHICH USED TO BE HERE AND WAS TOO BROAD. It
    // skipped every reward of a record the player already carried an affixed
    // copy of -- rings and common armour above all, which are exactly the
    // records that duplicate. Measured: a Silver Garnet Ring rolled and a Silver
    // Ring did not, one minute apart, because the second one had a companion in
    // the pack. That decision does not have to be made blind: a dropped
    // reference brings its own ExtraDataList, so ToNewInstance reads it and
    // returns the instance untouched if it drew the wrong one. See Apply.h.
    //
    // Safe to walk here and NOT in the event, which is the distinction the
    // crashes taught: by drain time the engine has finished with the list.
    enum class Refusal
    {
        kNone,
        kQuestObject,
        kFavourited,
        kGone
    };

    const char* Describe(Refusal a_refusal)
    {
        switch (a_refusal) {
        case Refusal::kQuestObject:
            return "a quest owns an instance of it";
        case Refusal::kFavourited:
            return "an instance of it is favourited";
        case Refusal::kGone:
            return "no longer in the inventory";
        default:
            return "";
        }
    }

    Refusal InspectEntry(RE::TESObjectREFR* a_refr, RE::TESBoundObject* a_object)
    {
        // NO-INIT. The init path populates inventory data as a side effect -- it
        // is a writer, not a reader -- and this function only asks questions.
        auto* changes = a_refr->GetInventoryChanges(true);
        if (!changes || !changes->entryList) {
            return Refusal::kNone;
        }
        for (auto* entry : *changes->entryList) {
            if (!entry || entry->object != a_object) {
                continue;
            }
            if (entry->IsQuestObject()) {
                return Refusal::kQuestObject;
            }
            if (entry->IsFavorited()) {
                return Refusal::kFavourited;
            }
            return Refusal::kNone;
        }
        // Not in the inventory any more -- sold or dropped while it settled.
        return Refusal::kGone;
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

        // ★NAMED IN THE LOG, EVERY TIME. A reward that rolls says so; until now a
        // reward that did NOT roll said nothing at all, and the only symptom the
        // player got was a plain item and no way to tell a refusal from a bug.
        const auto refused = [&](const char* a_why, std::uint64_t QuestReward::Stats::* a_field) {
            bump(a_field);
            logger::info("quest reward: {:08X} '{}' not rolled -- {}", a_object->GetFormID(),
                a_object->GetName(), a_why);
        };

        switch (const auto refusal = InspectEntry(player, a_object)) {
        case Refusal::kQuestObject:
            refused(Describe(refusal), &QuestReward::Stats::questObject);
            return;
        case Refusal::kFavourited:
            refused(Describe(refusal), &QuestReward::Stats::favourited);
            return;
        case Refusal::kGone:
            refused(Describe(refusal), &QuestReward::Stats::gone);
            return;
        default:
            break;
        }

        if (!Apply::IsEligible(a_object, nullptr)) {
            refused("not a weapon or armour record", &QuestReward::Stats::ineligible);
            return;
        }

        const auto slots = Apply::SlotsOf(a_object);
        if (slots == roll::kNone) {
            refused("carries no slot this system affixes", &QuestReward::Stats::ineligible);
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
            // Declined is the surgery reporting that the instance the engine
            // handed it was not one it may touch -- the item is back in the
            // inventory, unharmed, and this is an ordinary outcome. Only a
            // genuine failure gets the warning.
            if (applied.declined) {
                refused("the engine drew an instance that was already enchanted",
                    &QuestReward::Stats::alreadyEnchanted);
            } else {
                bump(&QuestReward::Stats::attachFailed);
            }
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

    // ------------------------------------------------- the activation ledger
    //
    // ★WHAT THE PLAYER IS ABOUT TO PICK UP, held just long enough for the grant
    // that follows to be recognised as a pickup. QuestReward.h has the full
    // reasoning; the short version is that TESContainerChangedEvent cannot tell
    // a pickup from a script grant, and TESActivateEvent can, because it names
    // the world reference and it fires FIRST.
    //
    // Matched on the BASE object rather than the reference: the reference is
    // gone by the time the grant arrives -- it was consumed by the pickup -- and
    // the base object is the only thing the two events have in common.
    struct Activated
    {
        RE::FormID                            baseObj{ 0 };
        std::chrono::steady_clock::time_point at{};
    };

    std::mutex             g_activatedLock;
    std::vector<Activated> g_activated;

    // ★CONSUMED ON MATCH, not merely looked up. One activation excuses one
    // grant. A window alone would let a single pickup shield every reward of the
    // same base record for as long as it lasted, and "you had just picked up an
    // iron sword" is not a reason to leave the iron sword a quest handed you
    // unrolled.
    constexpr auto kActivationWindow = std::chrono::milliseconds{ 1500 };

    // Activations that never became a pickup -- a locked chest, a door, a lever
    // -- expire on their own. Small on purpose: the list is walked on every
    // grant, and anything the window has outlived is dead weight.
    constexpr std::size_t kMaxActivated = 32;

    void NoteActivation(RE::FormID a_baseObj)
    {
        const auto       now = std::chrono::steady_clock::now();
        std::scoped_lock lock{ g_activatedLock };

        std::erase_if(g_activated,
            [&](const Activated& a_e) { return now - a_e.at >= kActivationWindow; });

        if (g_activated.size() >= kMaxActivated) {
            g_activated.erase(g_activated.begin());
        }
        g_activated.push_back({ a_baseObj, now });
    }

    bool TakeActivation(RE::FormID a_baseObj)
    {
        const auto       now = std::chrono::steady_clock::now();
        std::scoped_lock lock{ g_activatedLock };

        for (auto it = g_activated.begin(); it != g_activated.end(); ++it) {
            if (it->baseObj == a_baseObj && now - it->at < kActivationWindow) {
                g_activated.erase(it);
                return true;
            }
        }
        return false;
    }

    // Every activation the player performs, whatever it turns out to mean. A
    // door, a lever and a locked chest all land here and all expire unused; the
    // filtering that matters happens on the grant side, where the base object is
    // known to be one this path would otherwise have rolled.
    class ActivateSink : public RE::BSTEventSink<RE::TESActivateEvent>
    {
    public:
        static ActivateSink* GetSingleton()
        {
            static ActivateSink singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event,
            RE::BSTEventSource<RE::TESActivateEvent>*) override
        {
            if (!a_event || !g_enabled.load()) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Only the player's own activations. An NPC opening a door raises
            // this too, and nothing an NPC activates can become a grant into the
            // player's inventory.
            auto* actor = a_event->actionRef.get();
            constexpr RE::FormID kPlayer = 0x14;
            if (!actor || actor->GetFormID() != kPlayer) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* target = a_event->objectActivated.get();
            auto* base   = target ? target->GetBaseObject() : nullptr;
            if (!base) {
                return RE::BSEventNotifyControl::kContinue;
            }

            NoteActivation(base->GetFormID());
            return RE::BSEventNotifyControl::kContinue;
        }
    };

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

            // ★OUR OWN HAND, FIRST AND BEFORE THE COUNTERS. Apply::ToNewInstance
            // drops the item and picks it straight back up, and the pickup half
            // of that arrives here looking exactly like a fresh grant. Left in,
            // every roll fed one phantom grant back into this sink -- which is
            // what the entry guards were quietly refusing, one for one, for as
            // long as this file has existed.
            if (Apply::InSurgery()) {
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
            //
            // ★AND NOT `reference`, WHICH LOOKS LIKE IT BELONGS HERE AND DOES
            // NOT. It reads as "the world reference this came from", so it was
            // used to exclude ground pickups -- but on a pickup it arrives
            // EMPTY, so the clause never refused anything and every item taken
            // off the ground was rolled as a reward. QuestReward.h has the
            // evidence. The ledger below is what actually does that job.
            //
            // What is left is "something put this in your inventory out of
            // nothing", which is a quest reward and also, still, a pickup.
            constexpr RE::FormID kPlayer = 0x14;
            if (a_event->newContainer != kPlayer || a_event->oldContainer != 0) {
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

            // ★THE PLAYER PICKED THIS UP, AND THAT IS NOT A REWARD. Loose gear
            // lying in the world has already been through Distribute, which
            // rolled it -- or deliberately did not -- when the cell loaded. A
            // second roll on the way into the pocket is this path reaching past
            // its own boundary, and it is what the player saw.
            //
            // ★ASKED BEFORE THE GEAR TEST, so the ledger entry is consumed by
            // the pickup that created it whatever the item turned out to be. An
            // activation left behind by a picked-up potion would otherwise sit
            // there waiting to excuse the next real grant of one.
            if (TakeActivation(a_event->baseObj)) {
                std::scoped_lock lock{ g_statsLock };
                ++g_stats.fromWorld;
                return RE::BSEventNotifyControl::kContinue;
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
    // ★THE ACTIVATE SINK FIRST, and the order is not cosmetic. Both sinks are
    // live from the moment they are added; registering the one that RECOGNISES a
    // pickup after the one that ACTS on it leaves a window, however small, where
    // a grant can arrive with no ledger behind it.
    holder->AddEventSink<RE::TESActivateEvent>(ActivateSink::GetSingleton());
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
    {
        // The ledger goes with it, for the same reason: an activation belongs to
        // the world it happened in, and the save being opened did not make it.
        std::scoped_lock lock{ g_activatedLock };
        g_activated.clear();
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
    logger::info("    from world     {}   (the player picked it up off the ground)",
        stats.fromWorld);
    logger::info("    not gear       {}", stats.notGear);
    logger::info("    stacked        {}   (more than one at a time)", stats.stacked);
    logger::info("    from crafting  {}", stats.fromCrafting);
    logger::info("    not generic    {}   (in no leveled list -- a unique)", stats.notGeneric);
    logger::info("    quest object   {}   (a quest owns an instance of it)", stats.questObject);
    logger::info("    favourited     {}   (an instance carries a hotkey)", stats.favourited);
    logger::info("    enchanted      {}   (the drop drew an already-enchanted instance)",
        stats.alreadyEnchanted);
    logger::info("    gone           {}   (sold or dropped before it settled)", stats.gone);
    logger::info("    ineligible     {}   (no slot this system affixes)", stats.ineligible);
    if (stats.attachFailed) {
        logger::warn("    ATTACH FAILED  {}", stats.attachFailed);
    }
    logger::info("  still settling:  {}", outstanding);

    const auto tallied = stats.affixed + stats.rolledWhite + stats.fromWorld + stats.notGear +
        stats.stacked + stats.fromCrafting + stats.notGeneric + stats.questObject +
        stats.favourited + stats.alreadyEnchanted + stats.gone + stats.ineligible +
        stats.attachFailed + outstanding;
    if (tallied != stats.granted) {
        logger::warn("  ACCOUNTING GAP: {} granted but {} accounted for", stats.granted, tallied);
    }
}
