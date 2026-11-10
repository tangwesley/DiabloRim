// =============================================================================
//  Socketing -- reading the sockets another mod put on an item. See Sockets.h.
// =============================================================================

#include "PCH.h"

#include "Sockets.h"

#include "GridTint.h"
#include "api/SocketingAPI.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    static_assert(Sockets::kMaxSockets == SocketingAPI::kMaxSockets,
                  "a row must hold every socket Socketing can report");

    SocketingAPI::ISocketing001* g_soc = nullptr;
    // ★The same object again, as version 2 (Socketing 0.6.0+): the only way
    // to its tooltip lines. Null on 0.5.0, which still gets its wells.
    SocketingAPI::ISocketing002* g_soc2 = nullptr;
    bool                         g_connected = false;

    // Socketing's own advice for the line buffer: "16 lines are plenty".
    constexpr std::uint32_t kMaxLines = 16;

    // What the grid needs of one shard or gem, by Socketing's key ("Fus",
    // "Flawless Ruby"). form 0 records a key Socketing could not place.
    struct Stone
    {
        std::uint32_t form = 0;
        std::uint32_t rgb = 0;
    };
    std::unordered_map<std::string, Stone> g_stones;

    // Every enchanted unit asked about, by its enchantment's FormID. A row with
    // count 0 is remembered too: it is every vanilla-enchanted and every rolled
    // item on the board, and asking Socketing again for each of them per frame
    // is the cost this map exists to avoid.
    std::unordered_map<RE::FormID, Sockets::Row> g_rows;

    // Each socketed unit's tooltip lines, under the same key. ★Kept, because
    // the grid asks for them every frame its tooltip is up and Socketing
    // builds them from its whole item card each time it is asked.
    std::unordered_map<RE::FormID, std::vector<SocketingAPI::TooltipLine>> g_lines;

    // Everything remembered about ITEMS -- not the catalog. One place, so a
    // reason to forget the rows can never leave the lines behind.
    void ForgetItems()
    {
        g_rows.clear();
        g_lines.clear();
    }

    // The created enchantment that keys a unit, or 0 for a unit without one,
    // which is therefore not socketed: see Sockets.h.
    RE::FormID KeyOf(const RE::ExtraDataList* a_xl)
    {
        const auto* xe = a_xl ? a_xl->GetByType<RE::ExtraEnchantment>() : nullptr;
        return xe && xe->enchantment ? xe->enchantment->GetFormID() : 0;
    }

    // ★ONE BUFFER FOR THE CATALOG STRUCT, not one per call. A Socketable is
    // about two KB of fixed text (three stat lines of 512 bytes each), and it
    // is only ever filled on the main thread, which the interface requires.
    SocketingAPI::Socketable g_socketable{};

    const Stone* StoneOf(const char* a_key)
    {
        auto it = g_stones.find(a_key);
        if (it == g_stones.end()) {
            Stone s{};
            if (g_soc->FindSocketable(a_key, g_socketable)) {
                s.form = g_socketable.formId;
                s.rgb = g_socketable.color & 0xFFFFFF;
            } else {
                logger::warn("sockets: Socketing does not know the shard '{}' it reported", a_key);
            }
            it = g_stones.emplace(a_key, s).first;
        }
        return it->second.form ? &it->second : nullptr;
    }

    // Socketing's events. Filtered to sender "Socketing" at registration, so
    // the type numbers here are its own and cannot collide with SKSE's.
    void OnSocketing(SKSE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg) {
            return;
        }
        switch (a_msg->type) {
        case SocketingAPI::kDataLoaded:
            // The catalog was (re)built: keys may now name other forms.
            g_stones.clear();
            ForgetItems();
            break;

        case SocketingAPI::kItemChanged:
            // ★THE WHOLE MAP, not the one row. The enchantment that keys an
            // item may already be gone when its sockets are removed, so the
            // event's list cannot be trusted to name the row to drop -- and
            // re-asking costs one call per socketed item the grid can see.
            // A rebuild then redraws the player's grid; the partner window
            // and the doll collect every frame and need no telling.
            ForgetItems();
            GridTint::RequestRebuild();
            break;

        default:
            break;
        }
    }
}

bool Sockets::Connect()
{
    if (g_connected) {
        return g_soc != nullptr;
    }
    g_connected = true;

    g_soc = SocketingAPI::GetInterface();
    if (!g_soc) {
        logger::info("sockets: Socketing is not installed; no sockets to show");
        return false;
    }

    g_soc2 = SocketingAPI::GetInterface002();

    const bool listening = SKSE::GetMessagingInterface()->RegisterListener(
        SocketingAPI::kPluginName, OnSocketing);
    logger::info("sockets: found Socketing {} (interface {}), catalog {}; change events {}; "
        "tooltip lines {}",
        g_soc->GetPluginVersion(), g_soc->GetInterfaceVersion(),
        g_soc->IsReady() ? "loaded" : "not loaded yet",
        listening ? "subscribed" : "NOT subscribed -- the grid will not redraw on a change",
        g_soc2 ? "yes" : "no (Socketing is older than 0.6.0)");
    return true;
}

const Sockets::Row* Sockets::RowOf(std::uint32_t a_base, const RE::ExtraDataList* a_xl)
{
    if (!g_soc) {
        return nullptr;
    }
    const RE::FormID key = KeyOf(a_xl);
    if (key == 0) {
        return nullptr;
    }
    if (const auto it = g_rows.find(key); it != g_rows.end()) {
        return it->second.count ? &it->second : nullptr;
    }

    // Before Socketing's catalog is loaded its answer is not one to keep.
    if (!g_soc->IsReady()) {
        return nullptr;
    }

    Row row{};
    auto* form = RE::TESForm::LookupByID(a_base);
    auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
    SocketingAPI::ItemSockets is{};
    // The interface takes a mutable list and only reads it.
    if (obj && g_soc->GetItemSockets(obj, const_cast<RE::ExtraDataList*>(a_xl), is)) {
        row.count = static_cast<std::uint8_t>(
            (std::min)(static_cast<std::uint32_t>(is.sockets), kMaxSockets));
        for (std::uint32_t i = 0; i < row.count; ++i) {
            if (!is.shards[i][0]) {
                continue;   // an empty socket
            }
            if (const auto* s = StoneOf(is.shards[i])) {
                row.shard[i] = s->form;
                row.rgb[i] = s->rgb;
            }
        }
        logger::debug("sockets: {:08X} on {:08X} has {} socket(s), {} filled",
            key, a_base, row.count, is.filled);
    }
    const auto& kept = g_rows.emplace(key, row).first->second;
    return kept.count ? &kept : nullptr;
}

std::span<const SocketingAPI::TooltipLine> Sockets::LinesOf(std::uint32_t a_base,
    const RE::ExtraDataList* a_xl)
{
    // RowOf first: it is what proves the unit socketed (and Socketing ready),
    // so no unit Socketing does not own ever costs a line-building call.
    if (!g_soc2 || !RowOf(a_base, a_xl)) {
        return {};
    }
    const RE::FormID key = KeyOf(a_xl);
    auto it = g_lines.find(key);
    if (it == g_lines.end()) {
        std::vector<SocketingAPI::TooltipLine> lines(kMaxLines);
        auto* form = RE::TESForm::LookupByID(a_base);
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        // The interface takes a mutable list and only reads it.
        const std::uint32_t n = obj ? g_soc2->GetTooltipLines(obj,
            const_cast<RE::ExtraDataList*>(a_xl), lines.data(), kMaxLines) : 0;
        lines.resize((std::min)(n, kMaxLines));
        it = g_lines.emplace(key, std::move(lines)).first;
    }
    return it->second;
}

bool Sockets::ArtOf(std::uint32_t a_form, char* a_out, std::uint32_t a_capacity)
{
    if (!g_soc || !a_out || a_capacity == 0) {
        return false;
    }
    a_out[0] = '\0';
    auto* form = RE::TESForm::LookupByID(a_form);
    if (!form || !g_soc->SocketableOf(form, g_socketable)) {
        return false;
    }

    // ★THE PNG FIRST. Socketing ships both pictures, and the grid decodes
    // through Windows Imaging Component, which reads PNG on every Windows but
    // DDS only where its codec has the format. The DDS is the fallback for a
    // build that ever ships without the PNG.
    const char* path = g_socketable.iconPng[0] ? g_socketable.iconPng : g_socketable.iconDds;
    const std::size_t n = strnlen(path, SocketingAPI::kNameLength);
    if (n == 0 || n >= a_capacity) {
        return false;
    }
    std::memcpy(a_out, path, n);
    a_out[n] = '\0';
    return true;
}

void Sockets::Forget()
{
    ForgetItems();
}
