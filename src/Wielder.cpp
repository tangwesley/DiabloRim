#include "PCH.h"

#include "Wielder.h"

#include "Apply.h"
#include "Config.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // Every ability this module has ever built. The set is what makes
    // Reconcile stateless about actors: an entry in an actor's spell list that
    // is in here and not wanted is ours to remove, and nothing else is touched.
    std::unordered_set<RE::SpellItem*> g_ours;

    // One ability per enchantment, by the enchantment's id. ★VALIDATED ON EVERY
    // HIT, not trusted: created-object ids are reused when an enchantment is
    // destroyed and another made, and a save loaded mid-session can hand a
    // familiar id to a different roll. The cached ability is used only when its
    // effects still match what the enchantment carries now.
    std::unordered_map<RE::FormID, RE::SpellItem*> g_byEnch;

    // The effects of an enchantment that belong on the wielder rather than the
    // target: ours, and a weapon-skill or spell-cost fortify. Nothing else
    // qualifies -- a
    // vanilla weapon enchantment never carries one, and the merged enchantment
    // the arcane enchanter makes keeps ours alongside the player's, which is
    // exactly why this reads effects rather than ownership.
    std::vector<const RE::Effect*> WielderEffects(const RE::EnchantmentItem* a_ench)
    {
        std::vector<const RE::Effect*> out;
        if (!a_ench) {
            return out;
        }
        for (const auto* effect : a_ench->effects) {
            if (effect && effect->baseEffect && Apply::IsAffixEffect(effect->baseEffect) &&
                Apply::IsWielderEffect(effect->baseEffect)) {
                out.push_back(effect);
            }
        }
        return out;
    }

    bool Matches(const RE::SpellItem* a_spell, const std::vector<const RE::Effect*>& a_wanted)
    {
        if (!a_spell || a_spell->effects.size() != a_wanted.size()) {
            return false;
        }
        for (std::uint32_t i = 0; i < a_spell->effects.size(); ++i) {
            const auto* have = a_spell->effects[i];
            const auto* want = a_wanted[i];
            if (!have || have->baseEffect != want->baseEffect ||
                have->effectItem.magnitude != want->effectItem.magnitude) {
                return false;
            }
        }
        return true;
    }

    // The ability for an enchantment, built on first sight and reused after.
    // Null when the enchantment has nothing for the wielder.
    RE::SpellItem* AbilityFor(RE::EnchantmentItem* a_ench)
    {
        const auto wanted = WielderEffects(a_ench);
        if (wanted.empty()) {
            return nullptr;
        }

        const auto id = a_ench->GetFormID();
        if (const auto it = g_byEnch.find(id); it != g_byEnch.end() && Matches(it->second, wanted)) {
            return it->second;
        }

        auto* spell = RE::IFormFactory::Create<RE::SpellItem>();
        if (!spell) {
            logger::error("wielder: the form factory refused to create a spell");
            return nullptr;
        }

        // An ability: constant effect, on self, no cost, no cast. The engine
        // applies it the moment it is added to an actor and keeps it applied
        // until it is removed -- the same shape as a standing stone's blessing.
        spell->data.spellType = RE::MagicSystem::SpellType::kAbility;
        spell->data.castingType = RE::MagicSystem::CastingType::kConstantEffect;
        spell->data.delivery = RE::MagicSystem::Delivery::kSelf;
        spell->data.flags.reset();
        spell->data.costOverride = 0;
        spell->data.chargeTime = 0.0f;
        spell->data.castDuration = 0.0f;
        spell->data.range = 0.0f;
        spell->data.castingPerk = nullptr;
        spell->fullName = "Weapon affix";

        for (const auto* want : wanted) {
            auto* effect = new RE::Effect();
            effect->baseEffect = want->baseEffect;
            effect->effectItem = want->effectItem;
            // ★DURATION ZERO ON A CONSTANT EFFECT IS "FOREVER", not "instant" --
            // the enchantment row carries 0 for exactly this reason, and it is
            // copied through rather than corrected.
            effect->cost = 0.0f;
            spell->effects.push_back(effect);
        }

        g_ours.insert(spell);
        g_byEnch[id] = spell;
        logger::debug("wielder: built ability [{:08X}] for enchantment [{:08X}] with {} effect(s)",
            spell->GetFormID(), id, wanted.size());
        return spell;
    }

    // The abilities this actor should have right now: one per distinct
    // wielder-facing enchantment on a WORN weapon. Armour is the engine's own
    // business -- a constant-effect armour enchantment already reaches the
    // wearer -- so only weapons are read.
    std::vector<RE::SpellItem*> Desired(RE::Actor* a_actor)
    {
        std::vector<RE::SpellItem*> desired;

        auto* changes = a_actor->GetInventoryChanges();
        if (!changes || !changes->entryList) {
            return desired;
        }
        for (const auto* entry : *changes->entryList) {
            if (!entry || !entry->object || !entry->extraLists ||
                !entry->object->Is(RE::FormType::Weapon)) {
                continue;
            }
            for (auto* xList : *entry->extraLists) {
                if (!xList || !(xList->HasType<RE::ExtraWorn>() || xList->HasType<RE::ExtraWornLeft>())) {
                    continue;
                }
                const auto* xEnch = xList->GetByType<RE::ExtraEnchantment>();
                if (!xEnch || !xEnch->enchantment) {
                    continue;
                }
                if (auto* ability = AbilityFor(xEnch->enchantment);
                    ability && std::find(desired.begin(), desired.end(), ability) == desired.end()) {
                    desired.push_back(ability);
                }
            }
        }
        return desired;
    }

    struct EquipSink : RE::BSTEventSink<RE::TESEquipEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent*      a_event,
                                              RE::BSTEventSource<RE::TESEquipEvent>*) override
        {
            if (!a_event || !a_event->actor) {
                return RE::BSEventNotifyControl::kContinue;
            }
            // ★LOOKED UP, NOT TRUSTED. An unequip can name a form that is
            // already gone -- the PAPER crash was exactly that read -- and
            // only a live weapon is any of this module's business.
            const auto* base = RE::TESForm::LookupByID(a_event->baseObject);
            if (!base || !base->Is(RE::FormType::Weapon)) {
                return RE::BSEventNotifyControl::kContinue;
            }
            auto* actor = a_event->actor->As<RE::Actor>();
            if (!actor) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Deferred a frame: the event is dispatched from inside the equip,
            // before the worn flags have necessarily settled, and the
            // reconcile wants to read the finished state.
            const auto handle = actor->GetHandle();
            SKSE::GetTaskInterface()->AddTask([handle]() {
                if (auto actor = handle.get()) {
                    Wielder::Reconcile(actor.get());
                }
            });
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    EquipSink g_equipSink;
}

bool Wielder::WantsWielder(const RE::EnchantmentItem* a_ench)
{
    return !WielderEffects(a_ench).empty();
}

void Wielder::Reconcile(RE::Actor* a_actor)
{
    if (!a_actor || !Config::WielderBuffsEnabled()) {
        return;
    }

    const auto desired = Desired(a_actor);

    // Ours, present, and no longer wanted. Collected before removing: RemoveSpell
    // edits the array being walked.
    std::vector<RE::SpellItem*> stale;
    for (auto* spell : a_actor->GetActorRuntimeData().addedSpells) {
        if (spell && g_ours.contains(spell) &&
            std::find(desired.begin(), desired.end(), spell) == desired.end()) {
            stale.push_back(spell);
        }
    }
    for (auto* spell : stale) {
        a_actor->RemoveSpell(spell);
    }
    for (auto* spell : desired) {
        if (!a_actor->HasSpell(spell)) {
            a_actor->AddSpell(spell);
        }
    }
}

void Wielder::ReconcileLoaded()
{
    if (!Config::WielderBuffsEnabled()) {
        return;
    }
    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
        Reconcile(player);
    }
    auto* lists = RE::ProcessLists::GetSingleton();
    if (!lists) {
        return;
    }
    for (auto* list : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
        for (auto& handle : *list) {
            if (auto actor = handle.get()) {
                Reconcile(actor.get());
            }
        }
    }
}

void Wielder::Install()
{
    if (!Config::WielderBuffsEnabled()) {
        logger::info("wielder: buffs disabled by WielderBuffs=0; weapon skill affixes will "
                     "read on the card and do nothing");
        return;
    }
    if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
        holder->AddEventSink<RE::TESEquipEvent>(&g_equipSink);
        logger::info("wielder: equip sink installed");
    } else {
        logger::error("wielder: no ScriptEventSourceHolder; weapon skill affixes will not apply");
    }
}
