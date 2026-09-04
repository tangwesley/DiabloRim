#include "PCH.h"

#include "Pricing.h"

#include "Config.h"
#include "Persist.h"
#include "roll/Roll.h"

#include <cmath>
#include <vector>

namespace
{
    // The colour band of the instance this entry describes, or 0 for an item
    // that is not one of ours. An entry can carry several extra lists when it
    // stands for a stack; a rolled item is never stacked -- its enchantment
    // makes it unique -- so the first list with an enchantment is the one.
    std::uint8_t BandOf(const RE::InventoryEntryData* a_entry)
    {
        if (!a_entry || !a_entry->extraLists) {
            return 0;
        }
        for (const auto* xList : *a_entry->extraLists) {
            if (!xList) {
                continue;
            }
            const auto* xEnch = xList->GetByType<RE::ExtraEnchantment>();
            if (xEnch && xEnch->enchantment) {
                return Persist::EnchTier(xEnch->enchantment->GetFormID());
            }
        }
        return 0;
    }

    // Whether the value being asked for is a BUYING price: the barter menu is
    // up and showing the vendor's goods. Asked only for our own items, so the
    // Scaleform round trip inside IsViewingVendorItems is paid rarely.
    bool Buying()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::BarterMenu::MENU_NAME)) {
            return false;
        }
        const auto menu = ui->GetMenu<RE::BarterMenu>();
        return menu && menu->IsViewingVendorItems();
    }

    std::int32_t Hook(const RE::InventoryEntryData* a_entry)
    {
        const auto value = a_entry ? a_entry->GetValue() : 0;
        if (value <= 0) {
            return value;
        }
        const auto band = BandOf(a_entry);
        if (band == 0) {
            return value;
        }
        if (!Buying() && !Config::SellPricesScaled()) {
            return value;
        }
        const float mult = Config::PriceMult(static_cast<roll::Band>(band));
        if (mult <= 0.0f || mult == 1.0f) {
            return value;
        }
        const auto scaled = std::lround(static_cast<double>(value) * mult);
        return static_cast<std::int32_t>(std::min<long>(scaled, INT32_MAX));
    }

    // Every five-byte CALL or JMP in the game's code whose target is a_target.
    std::vector<std::pair<std::uintptr_t, std::uint8_t>> CallSitesOf(std::uintptr_t a_target)
    {
        std::vector<std::pair<std::uintptr_t, std::uint8_t>> sites;

        const auto  text = REL::Module::get().segment(REL::Segment::textx);
        const auto* base = reinterpret_cast<const std::uint8_t*>(text.address());
        const auto  size = text.size();
        if (!base || size < 5) {
            return sites;
        }

        for (std::size_t i = 0; i + 5 <= size; ++i) {
            const auto op = base[i];
            if (op != 0xE8 && op != 0xE9) {
                continue;
            }
            std::int32_t disp = 0;
            std::memcpy(&disp, base + i + 1, sizeof(disp));
            const auto site = text.address() + i;
            if (site + 5 + static_cast<std::intptr_t>(disp) == static_cast<std::intptr_t>(a_target)) {
                sites.emplace_back(site, op);
            }
        }
        return sites;
    }
}

void Pricing::Install()
{
    // The function itself, by the same id CommonLib calls it through. The
    // sites found below are exactly the callers of this address.
    const REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(15757, 15995) };
    const auto sites = CallSitesOf(target.address());
    if (sites.empty()) {
        logger::error("pricing: found no call to InventoryEntryData::GetValue [{:X}]; rolled "
                      "items will be priced as plain ones",
            target.address());
        return;
    }

    auto&       trampoline = SKSE::GetTrampoline();
    std::size_t calls = 0;
    std::size_t jumps = 0;
    for (const auto& [site, op] : sites) {
        if (op == 0xE8) {
            trampoline.write_call<5>(site, Hook);
            ++calls;
        } else {
            // A tail call: the caller jumped rather than called, and returns to
            // whoever called it. A jump to the hook keeps that shape.
            trampoline.write_branch<5>(site, Hook);
            ++jumps;
        }
    }
    logger::info("pricing: redirected {} call(s) and {} jump(s) to InventoryEntryData::GetValue",
        calls, jumps);
}
