// =============================================================================
//  The arcane enchanter. See Enchanting.h.
// =============================================================================

#include "PCH.h"

#include "Enchanting.h"

#include "Apply.h"
#include "Persist.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    // One affix enchantment, unhooked from its item for the duration of the
    // menu, and everything needed to put it back exactly as it was.
    struct Stashed
    {
        RE::TESBoundObject*  object{ nullptr };
        RE::ExtraDataList*   xList{ nullptr };
        RE::EnchantmentItem* ench{ nullptr };
        std::string          name;  // the override name we had set, if any
        std::uint16_t        charge{ 0 };
        std::uint8_t         band{ 0 };
        bool                 removeOnUnequip{ false };
        bool                 isWeapon{ false };
        bool                 handled{ false };
    };

    std::vector<Stashed> g_stashed;

    // Every extra list that ALREADY carried an enchantment when the menu opened.
    // On close, a list with an enchantment that is not in here is one the
    // enchanter just made -- which is how the merge finds its target when the
    // craft moves the item onto a new extra list instead of editing this one.
    std::unordered_set<const RE::ExtraDataList*> g_preexisting;

    bool g_open = false;

    bool IsWeapon(RE::TESBoundObject* a_object)
    {
        return a_object && a_object->Is(RE::FormType::Weapon);
    }

    // An arcane enchanter, by the furniture record's own workbench type rather
    // than by a keyword string. This is the field the engine reads to decide
    // which crafting sub-menu to build, so it is right for every modded
    // enchanter as well as the vanilla one.
    bool IsEnchantingFurniture(RE::TESObjectREFR* a_refr)
    {
        auto* base = a_refr ? a_refr->GetBaseObject() : nullptr;
        auto* furniture = base ? base->As<RE::TESFurniture>() : nullptr;
        if (!furniture) {
            return false;
        }

        using Bench = RE::TESFurniture::WorkBenchData::BenchType;
        const auto type = furniture->workBenchData.benchType;
        return type == Bench::kEnchanting || type == Bench::kEnchantingExperiment;
    }

    // ★"MINE." READ OFF THE ENCHANTMENT ITSELF, NOT OUT OF THE TIER MAP.
    //
    // The first version of this asked Persist::EnchTier -- the same handle
    // GridTint colours by -- and it was wrong, measured: of nine affixed items
    // in one save, the map knew two. The other seven had been rolled by earlier
    // builds, and every one of them stayed locked out of the enchanting table.
    // A record that can be absent for reasons this long after the fact cannot be
    // what decides whether the player is allowed to enchant something.
    //
    // What CAN decide it is the enchantment's own shape, and two facts together
    // make it unmistakable:
    //
    //   * costOverride zero WITH the override flag set. That pairing is what
    //     makes an affix never drain charge, it is stamped by Apply and by
    //     nothing else, and the two items the map did know prove it survives a
    //     save and reload intact.
    //   * every effect on it is one the affix table can produce.
    //
    // Neither alone would do -- a mod could plausibly ship a free enchantment,
    // and our effects are ordinary vanilla ones anybody may use -- but an
    // enchantment that is both is ours.
    //
    // A merged enchantment is excluded by both halves, on purpose: it carries
    // the player's cost and the player's effects. A return visit to the table
    // leaves it alone, and vanilla's one-enchantment-per-item rule takes over.
    bool IsOurs(RE::EnchantmentItem* a_ench)
    {
        if (!a_ench || !a_ench->IsDynamicForm()) {
            return false;
        }
        if (!a_ench->data.flags.all(RE::EnchantmentItem::EnchantmentFlag::kCostOverride) ||
            a_ench->data.costOverride != 0) {
            return false;
        }
        if (a_ench->effects.empty()) {
            return false;
        }

        for (const auto* effect : a_ench->effects) {
            if (!effect || !Apply::IsAffixEffect(effect->baseEffect)) {
                return false;
            }
        }
        return true;
    }

    // Is the crafting menu currently showing the ENCHANTING sub-menu? The same
    // menu serves the forge, the tanning rack and the cook pot, and stripping
    // inventory for those would be work done for nothing.
    //
    // Compared against the sub-menu's vtable rather than the furniture's
    // keyword: the keyword is what the engine reads to CHOOSE the sub-menu, so
    // the sub-menu's own type is the answer that keyword was asked for, and it
    // stays right for a modded enchanter built on some other furniture record.
    bool EnchantingSubMenuIsUp()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return false;
        }
        const auto menu = ui->GetMenu<RE::CraftingMenu>();
        if (!menu) {
            return false;
        }
        auto* sub = menu->GetCraftingSubMenu();
        if (!sub) {
            return false;
        }

        static const std::uintptr_t enchantVtbl =
            RE::CraftingSubMenus::EnchantConstructMenu::VTABLE[0].address();
        return enchantVtbl != 0 && *reinterpret_cast<std::uintptr_t*>(sub) == enchantVtbl;
    }

    // Copies of one magic item's effects, appended to a fresh Effect array.
    //
    // ★FIELD BY FIELD, and conditions are NOT carried across. Effect owns a
    // TESCondition, which is the head of a list the engine allocated; copying
    // the struct wholesale would hand the same nodes to a second owner, and the
    // second release of them is a crash. An enchantment effect with conditions
    // on it is rare, and losing one costs a wrong magnitude -- losing the heap
    // costs the save.
    void AppendEffects(RE::BSTArray<RE::Effect>& a_out, const RE::MagicItem* a_from)
    {
        if (!a_from) {
            return;
        }
        for (const auto* effect : a_from->effects) {
            if (!effect || !effect->baseEffect) {
                continue;
            }
            auto& copy = a_out.emplace_back();
            copy.baseEffect = effect->baseEffect;
            copy.effectItem = effect->effectItem;
            copy.cost = effect->cost;
        }
    }

    // The player's enchantment plus ours, as one created enchantment.
    //
    // The player's ENIT data is copied over afterwards, and that is what keeps
    // the item behaving like the thing they just made: their cost, their charge,
    // their base enchantment -- the record the effect descriptions and the item
    // card read from. Our own effects were built at cost 0, so an
    // auto-calculating enchantment still totals to the player's price.
    RE::EnchantmentItem* MergeEnchantments(RE::EnchantmentItem* a_player,
        RE::EnchantmentItem* a_ours, bool a_isWeapon)
    {
        auto* manager = RE::BGSCreatedObjectManager::GetSingleton();
        if (!manager || !a_player || !a_ours) {
            return nullptr;
        }

        RE::BSTArray<RE::Effect> effects;
        effects.reserve(
            static_cast<std::uint32_t>(a_player->effects.size() + a_ours->effects.size()));
        AppendEffects(effects, a_player);
        AppendEffects(effects, a_ours);
        if (effects.empty()) {
            return nullptr;
        }

        auto* merged = a_isWeapon ? manager->AddWeaponEnchantment(effects)
                                  : manager->AddArmorEnchantment(effects);
        if (!merged) {
            logger::error("enchanting: the manager refused a merged enchantment of {} effect(s)",
                effects.size());
            return nullptr;
        }

        // The manager dedupes identical effect sets, so this CAN come back as
        // one of its own inputs. It never should -- the merged set is strictly
        // larger than either -- but overwriting an input's data with its own and
        // then releasing it would be a very bad way to find out otherwise.
        if (merged == a_player || merged == a_ours) {
            logger::warn("enchanting: merge deduped onto an input ({:08X}); leaving it alone",
                merged->GetFormID());
            return nullptr;
        }

        merged->data = a_player->data;

        // ★A MERGE MUST NEVER LOOK LIKE AN AFFIX. IsOurs reads costOverride zero
        // with the override flag set as "this one is mine". The player's cost is
        // normally a real number, so copying their data is safe -- but if one
        // ever came through at zero with that flag, the merged item would be
        // stripped and merged again on the next visit, and its effect list would
        // double every time. The same shape of bug as the temper suffix above,
        // and made impossible the same way rather than argued about.
        if (merged->data.costOverride == 0) {
            merged->data.flags.reset(RE::EnchantmentItem::EnchantmentFlag::kCostOverride);
        }
        return merged;
    }

    void ReleaseOurs(RE::EnchantmentItem* a_ench, bool a_isWeapon)
    {
        if (!a_ench) {
            return;
        }
        // The id BEFORE the release, not after: the manager destroys the form on
        // the last decrement and GetFormID() would then be a read through a dead
        // pointer. Same ordering, and the same reason, as Apply::Release.
        const auto id = a_ench->GetFormID();
        if (auto* manager = RE::BGSCreatedObjectManager::GetSingleton()) {
            manager->DestroyEnchantment(a_ench, a_isWeapon);
        }
        Persist::ForgetEnchTier(id);
    }

    // -------------------------------------------------------------------------
    //  Opening: detach every affix enchantment the player is carrying.
    // -------------------------------------------------------------------------
    void HideAffixes(const char* a_trigger)
    {
        g_stashed.clear();
        g_preexisting.clear();

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* changes = player ? player->GetInventoryChanges() : nullptr;
        if (!changes || !changes->entryList) {
            return;
        }

        for (auto* entry : *changes->entryList) {
            if (!entry || !entry->object || !entry->extraLists) {
                continue;
            }
            if (!entry->object->Is(RE::FormType::Weapon) &&
                !entry->object->Is(RE::FormType::Armor)) {
                continue;
            }

            for (auto* xList : *entry->extraLists) {
                if (!xList) {
                    continue;
                }
                auto* xEnch = xList->GetByType<RE::ExtraEnchantment>();
                if (!xEnch || !xEnch->enchantment) {
                    continue;
                }

                // Recorded whether it is ours or not. What this set is for is
                // telling "was already enchanted when we got here" apart from
                // "the enchanter made this in the last thirty seconds".
                g_preexisting.insert(xList);

                if (!IsOurs(xEnch->enchantment)) {
                    // ★ONE LINE PER DECLINED ENCHANTMENT, and it earns its keep.
                    // Every failure of this feature so far has looked identical
                    // from in front of the table -- the item simply is not in
                    // the list -- and the difference between "we did not
                    // recognise it" and "we stripped it and the menu still said
                    // no" took a rebuild to tell apart each time. It does not
                    // any more.
                    logger::info("enchanting:   leaving [{:08X}] {} alone -- enchantment {:08X} "
                                 "is not ours (dynamic={}, cost={}/flags {:#x}, effects {})",
                        entry->object->GetFormID(), entry->GetDisplayName(),
                        xEnch->enchantment->GetFormID(), xEnch->enchantment->IsDynamicForm(),
                        xEnch->enchantment->data.costOverride,
                        xEnch->enchantment->data.flags.underlying(),
                        xEnch->enchantment->effects.size());
                    continue;
                }

                logger::info("enchanting:   stashing [{:08X}] {} (ench {:08X}, band {})",
                    entry->object->GetFormID(), entry->GetDisplayName(),
                    xEnch->enchantment->GetFormID(),
                    Persist::EnchTier(xEnch->enchantment->GetFormID()));

                Stashed stash;
                stash.object = entry->object;
                stash.xList = xList;
                stash.ench = xEnch->enchantment;
                stash.charge = xEnch->charge;
                stash.removeOnUnequip = xEnch->removeOnUnequip;
                stash.isWeapon = IsWeapon(entry->object);
                stash.band = Persist::EnchTier(xEnch->enchantment->GetFormID());

                // The affix name goes with the affixes. An item offered to the
                // table as unenchanted must not still be called "Ironclad Iron
                // Helmet" -- and if the player enchants it, they name it
                // themselves at the table.
                //
                // ★THE CUSTOM PART ONLY, NEVER THE WHOLE displayName. The engine
                // BAKES the temper suffix into that string and remembers how
                // much of it was ours in customNameLength. Storing the baked
                // string and setting it back as a custom name feeds the suffix
                // into its own input: measured, a tempered item gained one more
                // " (Fine)" on every single visit to the table --
                //
                //     Nulling Hunting Bow of Burning ★ (Fine)
                //     Nulling Hunting Bow of Burning ★ (Fine) (Fine)
                //     Nulling Hunting Bow of Burning ★ (Fine) (Fine) (Fine)
                //
                // -- and nothing about it ever stopped, because each visit read
                // back what the last one wrote.
                if (auto* xText = xList->GetByType<RE::ExtraTextDisplayData>()) {
                    if (const char* name = xText->displayName.c_str(); name && *name) {
                        std::size_t length = std::strlen(name);
                        if (xText->ownerInstance ==
                                RE::ExtraTextDisplayData::DisplayDataType::kCustomName &&
                            xText->customNameLength > 0 && xText->customNameLength < length) {
                            length = xText->customNameLength;
                        }
                        // The suffix arrives space-separated, so the custom part
                        // can end on the space that joined them.
                        while (length > 0 && name[length - 1] == ' ') {
                            --length;
                        }
                        stash.name.assign(name, length);
                    }
                }

                // ★DETACHED, NOT RELEASED. The manager's refcount does not move,
                // so the enchantment stays alive and valid while the menu is
                // open with nothing else holding it. Calling DestroyEnchantment
                // here would be the crash Apply::Release documents, reached from
                // the other direction.
                xList->RemoveByType(RE::ExtraDataType::kEnchantment);
                if (!stash.name.empty()) {
                    xList->RemoveByType(RE::ExtraDataType::kTextDisplayData);
                }

                g_stashed.push_back(std::move(stash));
            }
        }

        logger::info("enchanting: table opened on {} -- {} affixed item(s) offered as unenchanted",
            a_trigger, g_stashed.size());
    }

    // What the enchanting sub-menu ACTUALLY built, item by item.
    //
    // This is the only reading that settles the question. "The item is not in
    // the list" and "the item is in the list, disabled" and "the item was filed
    // under disenchant" all look identical from the other side of the screen and
    // have three different causes.
    void DumpMenuList(const char* a_when)
    {
        auto* ui = RE::UI::GetSingleton();
        const auto menu = ui ? ui->GetMenu<RE::CraftingMenu>() : nullptr;
        auto* sub = menu ? menu->GetCraftingSubMenu() : nullptr;
        if (!sub) {
            logger::info("enchanting: [{}] no crafting sub-menu to inspect", a_when);
            return;
        }

        // ★THE TYPE CHECK BELONGS HERE, AT THE CAST, not at the call site.
        //
        // CraftingMenu is ONE menu with several sub-menus behind it: a forge, a
        // tanning rack, a smelter, an alchemy bench and an enchanter all open
        // it. Only one of them is an EnchantConstructMenu, and the cast below
        // is unchecked -- so at a forge this read `listEntries` and
        // `enabledFilters` from a ConstructibleObjectMenu at the wrong offsets,
        // reported "10 entry(ies)" out of unrelated memory, and died walking
        // that array. Measured on 1.6.1170: EXCEPTION_ACCESS_VIOLATION reading
        // 0xFFFFFFFFFFFFFFFF, and the log line above the crash had ALREADY said
        // "enchanting sub-menu not up".
        //
        // Which is the lesson worth keeping: the caller knew, and asked anyway.
        // A function that performs an unchecked downcast has to be the one that
        // proves the type, because every future call site is a chance to forget.
        if (!EnchantingSubMenuIsUp()) {
            logger::info("enchanting: [{}] crafting menu is not the enchanter -- nothing to dump",
                a_when);
            return;
        }

        auto* ench = static_cast<RE::CraftingSubMenus::EnchantConstructMenu*>(sub);
        logger::info("enchanting: [{}] menu list has {} entry(ies), enabledFilters {:#x}", a_when,
            ench->listEntries.size(), ench->enabledFilters.underlying());

        for (auto& handle : ench->listEntries) {
            auto* entry = handle.get();
            if (!entry) {
                continue;
            }
            const char* name = entry->GetName();
            logger::info("enchanting:     flags {:#06x} enabled={} : {}",
                entry->filterFlag.underlying(), entry->enabled, name ? name : "<no name>");
        }
    }

    // -------------------------------------------------------------------------
    //  Closing: put them back, merging with whatever the player made.
    // -------------------------------------------------------------------------
    struct Live
    {
        RE::TESBoundObject* object{ nullptr };
        RE::ExtraDataList*  xList{ nullptr };
    };

    std::vector<Live> LivePlayerLists()
    {
        std::vector<Live> live;

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* changes = player ? player->GetInventoryChanges() : nullptr;
        if (!changes || !changes->entryList) {
            return live;
        }

        for (auto* entry : *changes->entryList) {
            if (!entry || !entry->object || !entry->extraLists) {
                continue;
            }
            for (auto* xList : *entry->extraLists) {
                if (xList) {
                    live.push_back({ entry->object, xList });
                }
            }
        }
        return live;
    }

    // ★★★A MERGE REWRITES THE ENCHANTMENT ON AN ITEM THE PLAYER MAY BE WEARING,
    // AND THE ACTOR DOES NOT NOTICE.
    //
    // The engine applies an item's enchantment when the item goes ON, and
    // nothing re-reads it afterwards. The strip/restore round-trip gets away
    // with that -- the same enchantment goes back on, so what the actor is
    // running is still right -- but a MERGE does not: the item ends up carrying
    // a new created enchantment with both effect sets while the player keeps
    // running the affix-only one they had at equip time, and the two originals
    // have just been released, so the effects on the actor belong to a form
    // nothing holds any more.
    //
    // Taking the item off and putting it straight back is the whole fix. That
    // is the one moment the engine reads the enchantment.
    //
    // ★DONE HERE, where the list that was rewritten is in hand and its hand is
    // readable off ExtraWorn/ExtraWornLeft. Everything else that could notice
    // this has to go looking for the change and then guess which unit moved.
    //
    // ★The list SURVIVES the unequip: the engine collapses an unworn unit into
    // an identical stack, and a unit carrying a created enchantment is identical
    // to nothing. Checked rather than assumed all the same -- equipping a freed
    // list is not a bug that would announce itself.
    void Reseat(RE::TESBoundObject* a_object, RE::ExtraDataList* a_xList)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* equipper = RE::ActorEquipManager::GetSingleton();
        if (!player || !equipper || !a_object || !a_xList) {
            return;
        }
        const bool left = a_xList->HasType<RE::ExtraWornLeft>();
        if (!left && !a_xList->HasType<RE::ExtraWorn>()) {
            return;   // in the pack: the next equip reads the new enchantment anyway
        }

        // Armour has no hand. A weapon has to go back to the one it came off --
        // equipping without a slot sends it to the right, so a merged dagger in
        // the left hand would jump hands as a side effect of enchanting it.
        const RE::BGSEquipSlot* slot = nullptr;
        if (IsWeapon(a_object)) {
            slot = RE::TESForm::LookupByID<RE::BGSEquipSlot>(left ? 0x13F43 : 0x13F42);
        }

        // Silent and immediate: the player is standing at a table watching a
        // menu close, not equipping anything.
        equipper->UnequipObject(player, a_object, a_xList, 1, slot,
            false, false, false, true);

        const auto live = LivePlayerLists();
        if (std::none_of(live.begin(), live.end(),
                [&](const Live& l) { return l.xList == a_xList; })) {
            logger::warn("enchanting: {} came off but its extra list did not survive -- "
                         "leaving it off rather than equipping a freed list",
                a_object->GetName());
            return;
        }

        equipper->EquipObject(player, a_object, a_xList, 1, slot,
            false, false, false, true);
        logger::info("enchanting: reseated worn {} ({} hand) so the merged enchantment applies",
            a_object->GetName(), IsWeapon(a_object) ? (left ? "left" : "right") : "no");
    }

    // Puts one stash back onto a list that now carries a player enchantment.
    // Returns whether the affixes actually made it onto the item; a_stash.handled
    // says whether the stash was consumed either way, and the two differ exactly
    // when the merge failed and the affixes had to be released instead.
    bool MergeOnto(Stashed& a_stash, RE::ExtraDataList* a_xList)
    {
        auto* xEnch = a_xList->GetByType<RE::ExtraEnchantment>();
        if (!xEnch || !xEnch->enchantment) {
            return false;
        }

        auto*      playerEnch = xEnch->enchantment;
        const auto charge = xEnch->charge;
        const bool removeOnUnequip = xEnch->removeOnUnequip;

        auto* merged = MergeEnchantments(playerEnch, a_stash.ench, a_stash.isWeapon);
        if (!merged) {
            // Nothing has been taken apart yet, so the honest outcome is the
            // player's enchantment intact and the affixes gone. Said out loud
            // rather than left as a silent hole in the item.
            logger::error("enchanting: could not merge affixes into the new enchantment on {}; "
                          "keeping the player enchantment and releasing the affixes",
                a_stash.object ? a_stash.object->GetName() : "?");
            ReleaseOurs(a_stash.ench, a_stash.isWeapon);
            a_stash.handled = true;
            return false;
        }

        const auto ourID = a_stash.ench->GetFormID();
        const auto playerID = playerEnch->GetFormID();
        const auto mergedID = merged->GetFormID();

        a_xList->RemoveByType(RE::ExtraDataType::kEnchantment);
        a_xList->SetEnchantment(merged, charge, removeOnUnequip);

        // Two references dropped, one taken. Building the merge incremented for
        // `merged`; the extra list we just rewrote held one for the player's
        // enchantment, and we have been holding one for ours since the menu
        // opened. Both of those are now unheld and have to be told so.
        if (auto* manager = RE::BGSCreatedObjectManager::GetSingleton()) {
            manager->DestroyEnchantment(playerEnch, a_stash.isWeapon);
            manager->DestroyEnchantment(a_stash.ench, a_stash.isWeapon);
        }
        Persist::ForgetEnchTier(ourID);
        Persist::NoteEnchTier(mergedID, a_stash.band);

        // The name is the player's now -- they typed it at the table. Ours is
        // deliberately not put back over the top of it.
        logger::info("enchanting: merged affixes into the player's enchantment on {} "
                     "({:08X} + {:08X} -> {:08X})",
            a_stash.object ? a_stash.object->GetName() : "?", playerID, ourID, mergedID);

        // ★The merge is the only path that changes WHICH enchantment an item
        // carries, so it is the only one that has to put a worn item back on.
        // (The restore path above reattaches the very same enchantment, which
        // the actor is already running.)
        Reseat(a_stash.object, a_xList);

        a_stash.handled = true;
        return true;
    }

    void RestoreAffixes(const char* a_trigger)
    {
        if (g_stashed.empty()) {
            g_preexisting.clear();
            return;
        }

        const auto live = LivePlayerLists();
        const auto isLive = [&](const RE::ExtraDataList* a_xList) {
            return std::any_of(live.begin(), live.end(),
                [&](const Live& l) { return l.xList == a_xList; });
        };

        std::size_t restored = 0;
        std::size_t mergedCount = 0;
        std::size_t lost = 0;

        // Pass 1 -- the list survived, which is the ordinary case whether or not
        // the player enchanted anything on it.
        for (auto& stash : g_stashed) {
            if (!stash.xList || !isLive(stash.xList)) {
                continue;
            }

            if (stash.xList->HasType<RE::ExtraEnchantment>()) {
                if (MergeOnto(stash, stash.xList)) {
                    ++mergedCount;
                } else if (stash.handled) {
                    ++lost;
                }
                continue;
            }

            stash.xList->SetEnchantment(stash.ench, stash.charge, stash.removeOnUnequip);
            if (!stash.name.empty()) {
                stash.xList->SetOverrideName(stash.name.c_str());
            }
            stash.handled = true;
            ++restored;
        }

        // Pass 2 -- the list is gone, so the craft moved the item onto a new one
        // and the affixes have to find it again.
        //
        // ★MATCHED ONLY WHERE THERE IS EXACTLY ONE CANDIDATE, per base object.
        // The pairing is a guess, and a wrong guess grafts one item's affixes
        // onto a different one the player happened to enchant in the same visit.
        // One unclaimed stash and one new enchantment on the same base form is
        // not a guess; anything else is, and an affix set lost cleanly beats an
        // item that quietly gains somebody else's.
        std::vector<Stashed*> orphans;
        for (auto& stash : g_stashed) {
            if (!stash.handled) {
                orphans.push_back(&stash);
            }
        }

        for (auto* orphan : orphans) {
            if (orphan->handled) {
                continue;
            }

            const auto claimants = std::count_if(orphans.begin(), orphans.end(),
                [&](const Stashed* s) { return !s->handled && s->object == orphan->object; });

            RE::ExtraDataList* candidate = nullptr;
            std::size_t        candidates = 0;
            for (const auto& l : live) {
                if (l.object != orphan->object || g_preexisting.contains(l.xList)) {
                    continue;
                }
                auto* xEnch = l.xList->GetByType<RE::ExtraEnchantment>();
                if (!xEnch || !xEnch->enchantment || !xEnch->enchantment->IsDynamicForm()) {
                    continue;
                }
                ++candidates;
                candidate = l.xList;
            }

            if (claimants == 1 && candidates == 1) {
                if (MergeOnto(*orphan, candidate)) {
                    ++mergedCount;
                    continue;
                }
                // ★The merge already released the affixes on its own failure
                // path. Falling through to the release below would decrement a
                // second time for a reference that no longer exists, which is
                // the same manager-refcount damage Apply::Release is written to
                // avoid -- just spelled backwards.
                if (orphan->handled) {
                    ++lost;
                    continue;
                }
            }

            logger::warn("enchanting: lost track of the affixed {} ({} unclaimed, {} candidate(s)); "
                         "releasing its enchantment rather than guessing",
                orphan->object ? orphan->object->GetName() : "?", claimants, candidates);
            ReleaseOurs(orphan->ench, orphan->isWeapon);
            orphan->handled = true;
            ++lost;
        }

        logger::info("enchanting: table closed on {} -- {} restored, {} merged, {} released",
            a_trigger, restored, mergedCount, lost);

        g_stashed.clear();
        g_preexisting.clear();
    }

    // -------------------------------------------------------------------------
    //  Session control. Two events can start one and two can end it, so the
    //  guard lives here rather than being repeated in each sink.
    // -------------------------------------------------------------------------
    void BeginSession(const char* a_trigger)
    {
        if (g_open) {
            return;
        }
        g_open = true;
        HideAffixes(a_trigger);
    }

    void EndSession(const char* a_trigger)
    {
        if (!g_open) {
            return;
        }
        g_open = false;
        RestoreAffixes(a_trigger);
    }

    // ★THE EARLY TRIGGER, AND THE ONE THAT ACTUALLY WORKS.
    //
    // Measured: stripping on the crafting menu's OPEN event is too late. The
    // sub-menu builds its item list while it is being constructed, and
    // MenuOpenCloseEvent is not dispatched until after the menu exists and has
    // been pushed -- so the list the player sees was decided from an inventory
    // that still had the affix enchantments on it. The log said "2 affixed
    // item(s) offered as unenchanted" and the table still showed none of them.
    //
    // Entering the furniture happens a whole animation earlier, before anything
    // has asked the inventory a question.
    class FurnitureSink : public RE::BSTEventSink<RE::TESFurnitureEvent>
    {
    public:
        static FurnitureSink* GetSingleton()
        {
            static FurnitureSink singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESFurnitureEvent*      a_event,
            RE::BSTEventSource<RE::TESFurnitureEvent>*) override
        {
            if (!a_event || !a_event->actor || !a_event->actor->IsPlayerRef()) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (!IsEnchantingFurniture(a_event->targetFurniture.get())) {
                return RE::BSEventNotifyControl::kContinue;
            }

            if (a_event->type == RE::TESFurnitureEvent::FurnitureEventType::kEnter) {
                BeginSession("furniture");
                return RE::BSEventNotifyControl::kContinue;
            }

            // ★Leaving the furniture only restores when the MENU never took
            // over. In the ordinary flow the menu closes first and this arrives
            // with nothing left to do; this branch exists for the interrupted
            // approach -- walked up, sat down, the menu never opened -- where
            // otherwise the affixes would stay detached and the next save would
            // be written without them.
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->IsMenuOpen(RE::CraftingMenu::MENU_NAME)) {
                return RE::BSEventNotifyControl::kContinue;
            }
            EndSession("furniture exit");
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class CraftingMenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static CraftingMenuSink* GetSingleton()
        {
            static CraftingMenuSink singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent*      a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (!a_event || a_event->menuName != RE::CraftingMenu::MENU_NAME) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // DIAGNOSTIC. Logged before any guard so the log shows the ORDER of
            // the furniture event and this one -- which of the two happens first
            // is the whole question, and neither event says so on its own.
            logger::info("enchanting: crafting menu {} -- enchanting sub-menu {}, stash {}",
                a_event->opening ? "OPENED" : "CLOSED",
                EnchantingSubMenuIsUp() ? "up" : "not up",
                g_open ? "held" : "empty");
            if (a_event->opening) {
                DumpMenuList("menu open");
            }

            if (a_event->opening) {
                // ★A BACKSTOP NOW, not the main path -- FurnitureSink has
                // normally already run and g_open is set. What is left for this
                // to catch is a table reached without furniture at all, which is
                // how the "enchant from anywhere" mods open the menu.
                //
                // It is late by construction: the list is already built. Asking
                // the sub-menu to redraw itself is the only lever available from
                // out here, and it either takes or the player closes and reopens
                // once. Better than the menu being empty with no explanation.
                if (g_open || !EnchantingSubMenuIsUp()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                BeginSession("menu open (late -- no furniture event)");
                if (!g_stashed.empty()) {
                    if (auto menu = RE::UI::GetSingleton()->GetMenu<RE::CraftingMenu>()) {
                        if (auto* sub = menu->GetCraftingSubMenu()) {
                            static_cast<RE::CraftingSubMenus::EnchantConstructMenu*>(sub)
                                ->UpdateInterface();
                        }
                    }
                }
            } else if (g_open) {
                // ★The close is UNCONDITIONAL once we have opened. Asking
                // EnchantingSubMenuIsUp() here would be interrogating a menu
                // that is being torn down, and a "no" would strand every stashed
                // enchantment: detached, unreleased, and absent from the next
                // save.
                EndSession("menu close");
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };
}

void Enchanting::Install()
{
    if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
        holder->AddEventSink<RE::TESFurnitureEvent>(FurnitureSink::GetSingleton());
    } else {
        logger::error("enchanting: no script event source holder; the furniture trigger is "
                      "unavailable and affixed items will stay unenchantable at a real table");
    }

    if (auto* ui = RE::UI::GetSingleton()) {
        ui->AddEventSink<RE::MenuOpenCloseEvent>(CraftingMenuSink::GetSingleton());
    } else {
        logger::error("enchanting: no UI singleton; nothing will put the affixes back");
    }

    logger::info("enchanting: furniture + crafting-menu sinks installed");
}

void Enchanting::Forget()
{
    if (!g_open && g_stashed.empty()) {
        return;
    }

    // ★DROPPED, NOT RESTORED. Every pointer held here names an extra list in the
    // world that is being replaced, and writing through one after the load would
    // be a write into freed memory. Nothing is lost by letting go: the save
    // being loaded still has its affix enchantments, because no save can be
    // written while they are detached.
    logger::warn("enchanting: a load arrived with {} affix enchantment(s) still detached; "
                 "dropping them -- the save being loaded carries its own",
        g_stashed.size());

    g_open = false;
    g_stashed.clear();
    g_preexisting.clear();
}
