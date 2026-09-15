// =============================================================================
//  Grid Inventory tint. See GridTint.h.
// =============================================================================

#include "PCH.h"

#include "GridTint.h"

#include "Config.h"
#include "Persist.h"
#include "api/GridInventoryAPI.h"
#include "roll/Roll.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <string>

namespace
{
    std::atomic<bool> g_registered{ false };
    std::atomic<bool> g_offered{ false };

    // ImU32 on the host's build is 0xAABBGGRR -- red in the LOW byte. Written
    // as a function so the constants below can be read in the order a colour is
    // normally quoted, rather than reversed by hand and mis-transcribed once.
    constexpr std::uint32_t Rgba(std::uint8_t a_r, std::uint8_t a_g, std::uint8_t a_b)
    {
        return (std::uint32_t{ 0xFF } << 24) | (std::uint32_t{ a_b } << 16) |
               (std::uint32_t{ a_g } << 8) | std::uint32_t{ a_r };
    }

    // The five bands, in the order roll::Band declares them. White is absent on
    // purpose: a band-0 item is an ordinary item and must look like one, so the
    // tier query answers 0 for it and the host draws what it always drew.
    //
    // These are the affix bands, not a copy of the host's own rarity colours.
    // Blue is deliberately close to the grid's "enchanted" blue -- a band-1 item
    // IS the least interesting enchanted thing on the board, and having it read
    // as a slightly brighter version of the colour it would have had anyway is
    // the honest presentation.
    //
    // Red sits a step past orange in hue and a step down in brightness, so the
    // two read as neighbours on one scale rather than as unrelated colours --
    // and so it is not the pure red the host uses for damage and warnings.
    constexpr std::uint32_t kPalette[]{
        Rgba(105, 155, 255),   // 1  blue     tiers 1-3
        Rgba(240, 215, 100),   // 2  yellow   tiers 4-6
        Rgba(185, 120, 240),   // 3  purple   tiers 7-9
        Rgba(255, 140, 60),    // 4  orange   tiers 10-12
        Rgba(230, 60, 70),     // 5  red      tiers 13-15
    };
    static_assert(std::size(kPalette) <= GridInvAPI::kMaxTintTier,
                  "the ABI cannot carry this many bands");
    static_assert(std::size(kPalette) == static_cast<std::size_t>(roll::kBandCount) - 1,
                  "one colour per non-white band, in roll::Band order");

    // ---- the tables the host calls ---------------------------------------

    // The band of one unit, 0 when this mod did not roll it.
    //
    // SHARED BY BOTH HOOKS, and that sharing is the point rather than a tidy-up:
    // the tint and the tooltip line must never disagree about an item, and the
    // surest way to guarantee that is for there to be one answer.
    std::uint8_t BandOf(const void* a_xl)
    {
        // No list of its own means a plain unit -- nothing this mod has touched,
        // because attaching an affix is exactly what creates one.
        if (!a_xl) {
            return 0;
        }
        const auto* list = static_cast<const RE::ExtraDataList*>(a_xl);
        const auto* xe = list->GetByType<RE::ExtraEnchantment>();
        if (!xe || !xe->enchantment) {
            return 0;
        }

        // THE ENCHANTMENT IS THE KEY, not the base form and not the list.
        //
        // The base form is shared by every iron sword in Skyrim, and the list
        // pointer is rewritten by the engine on every container move. The
        // created enchantment is the one thing that belongs to this instance
        // and survives being carried, looted, stored and reloaded -- which is
        // why the tier is recorded against it and travels in the co-save.
        //
        // A vanilla-enchanted item reaches here too (the host asks about every
        // tile) and answers 0: we never rolled it, so its FormID is not in the
        // map and it keeps the host's own enchanted blue.
        return Persist::EnchTier(xe->enchantment->GetFormID());
    }

    // HOT PATH. Once per visible tile per frame, again per equipment-doll slot,
    // and again while a tooltip is built. One extra-data walk and one hash
    // lookup, and it must stay that way: no form lookup, no allocation.
    std::uint8_t GetTier(void*, std::uint32_t, const void* a_xl)
    {
        return BandOf(a_xl);
    }

    // ★THE MARKER, ON HOVER -- the thing the item's NAME used to carry.
    //
    // NOT the hot path: once per tooltip. It may build a string, which GetTier
    // above may not.
    //
    // ONE LINE, and it is the glyphs alone. The band already has a voice here
    // -- the host paints the item's name in our palette colour on this very
    // tooltip -- so a line reading "Purple" under a purple name would be the
    // same fact twice in two alphabets. The glyphs say how MANY, which colour
    // alone cannot, and they are what the player configured.
    std::uint32_t GetLines(void*, std::uint32_t, const void* a_xl,
        GridInvAPI::TooltipLine* a_out, std::uint32_t a_capacity)
    {
        if (!a_out || a_capacity == 0) {
            return 0;
        }

        // ★SILENT WHEN THE MARKER IS IN THE NAME. The host draws the name on
        // this same tooltip, so with TierMarkerInName on the glyphs are already
        // up there -- and repeating them two lines apart looks like a bug in
        // someone else's mod. The INI calls these an either/or; this is where
        // that is actually enforced.
        if (roll::TierMarkInName()) {
            return 0;
        }

        const std::uint8_t band = BandOf(a_xl);
        if (band == 0) {
            return 0;
        }
        const std::string mark = roll::BandMark(static_cast<roll::Band>(band));
        if (mark.empty()) {
            return 0;   // marker turned off in the INI
        }

        // ★TRUNCATED ON A CODEPOINT BOUNDARY, NOT A BYTE ONE. The mark is
        // whatever the player typed into the INI -- a word, not necessarily a
        // glyph -- and the field is 128 bytes. Cutting a UTF-8 sequence in half
        // would hand the host bytes that are not text, so back off over any
        // continuation bytes (10xxxxxx) before terminating.
        std::size_t n = (std::min)(mark.size(),
            static_cast<std::size_t>(GridInvAPI::kTooltipTextLen - 1));
        while (n > 0 && (static_cast<unsigned char>(mark[n]) & 0xC0) == 0x80) {
            --n;
        }
        std::memcpy(a_out[0].text, mark.data(), n);
        a_out[0].text[n] = '\0';

        // The band's own colour, so the line and the name it sits under are
        // plainly the same statement. Falls back to the host's body colour if a
        // band ever outruns the palette.
        a_out[0].rgba = (band <= std::size(kPalette)) ? kPalette[band - 1] : 0u;
        a_out[0].indent = 0;
        a_out[0].separatorBefore = 0;
        return 1;
    }

    // ★WHAT A ROLLED ITEM IS WORTH AT THE HOST'S COUNTER -- the same rule
    // Pricing.cpp applies to the vanilla menus, answered over the ABI because
    // the host's shop never goes near the function that hook patches: it
    // calls the engine's value routine itself, from its own DLL, and the
    // vanilla BarterMenu the hook reads direction from is hidden while the
    // host's window is up. Without this table a purple sword sold in the grid
    // for its plain value with SellPricesScaled on OR off, since neither
    // setting was ever consulted.
    //
    // NOT the hot path: once per shelf cell at collect, once per tooltip, once
    // per sale. Still one extra-data walk and one hash lookup, and two reads
    // of settings that were loaded once at startup.
    float GetMultiplier(void*, std::uint32_t, const void* a_xl, std::uint32_t a_side)
    {
        const std::uint8_t band = BandOf(a_xl);
        if (band == 0) {
            return 1.0f;   // not ours, or white: the host prices it as it always did
        }
        // The selling side is scaled only when the INI says so -- the same
        // default Pricing.h explains: what a merchant asks goes up, what a
        // merchant pays does not, unless the player chose otherwise.
        if (a_side == GridInvAPI::kPriceSell && !Config::SellPricesScaled()) {
            return 1.0f;
        }
        const float mult = Config::PriceMult(static_cast<roll::Band>(band));
        return mult > 0.0f ? mult : 1.0f;
    }

    std::uint32_t GetPalette(void*, std::uint32_t* a_out, std::uint32_t a_capacity)
    {
        if (!a_out) {
            return 0;
        }
        const auto n = static_cast<std::uint32_t>(
            (std::min)(static_cast<std::size_t>(a_capacity), std::size(kPalette)));
        for (std::uint32_t i = 0; i < n; ++i) {
            a_out[i] = kPalette[i];
        }
        return n;
    }

    // ---- the handshake ----------------------------------------------------

    void Offer()
    {
        // Once. The host warns and ignores a second table, and a warning in
        // someone else's log is a bad way to say "we have a bug".
        if (g_offered.exchange(true)) {
            return;
        }

        // Static, not a local. The host copies the struct during the call, but
        // `name` is a borrowed pointer it keeps -- so the string has to outlive
        // this function, and making the whole table static is the cheapest way
        // to be sure of that.
        static GridInvAPI::Tinter tinter{
            sizeof(GridInvAPI::Tinter),
            GridInvAPI::kABIVersion,
            "DiabloInSkyrim",
            nullptr,   // self: we keep no per-instance state
            &GetTier,
            &GetPalette,
        };

        const bool sent = SKSE::GetMessagingInterface()->Dispatch(
            GridInvAPI::kMsgRegisterTinter, &tinter, sizeof(tinter),
            GridInvAPI::kHostPluginName);

        // ★A SECOND TABLE, A SECOND SLOT, AND IT IS NOT THE PROVIDER ONE.
        //
        // The tooltip line cannot ride Provider::GetTooltipLines: that hook is
        // keyed by ItemKey, whose uid is ExtraUniqueID -- 0 for every renamed or
        // enchanted unit, which is every item we have touched. Annotator is
        // handed the ExtraDataList instead, exactly as the tinter is, and it is
        // its own slot, so a socket mod holding the provider slot keeps it.
        static GridInvAPI::Annotator annot{
            sizeof(GridInvAPI::Annotator),
            GridInvAPI::kABIVersion,
            "DiabloInSkyrim",
            nullptr,   // self: we keep no per-instance state
            &GetLines,
        };

        const bool sentAnnot = SKSE::GetMessagingInterface()->Dispatch(
            GridInvAPI::kMsgRegisterAnnot, &annot, sizeof(annot),
            GridInvAPI::kHostPluginName);

        // ★A THIRD TABLE, FOR THE COUNTER. The host's shop window is the one
        // place a price is shown that the call-site hook in Pricing.cpp cannot
        // reach (see GetMultiplier above), so the host asks us instead. Same
        // slot discipline as the two before it: its own message, and a host
        // built before it existed simply never sends for it -- the prices
        // there stay plain, as they were, and nothing else is lost.
        static GridInvAPI::Pricer pricer{
            sizeof(GridInvAPI::Pricer),
            GridInvAPI::kABIVersion,
            "DiabloInSkyrim",
            nullptr,   // self: we keep no per-instance state
            &GetMultiplier,
        };

        const bool sentPrice = SKSE::GetMessagingInterface()->Dispatch(
            GridInvAPI::kMsgRegisterPricer, &pricer, sizeof(pricer),
            GridInvAPI::kHostPluginName);

        // Dispatch returning true means the message was DELIVERED, not that the
        // host accepted the table -- the version gate and the null-pointer check
        // both run inside its handler and refuse invisibly from here. The host
        // logs its own verdict; this line only proves we spoke.
        g_registered.store(sent);
        logger::info("grid tint: offered {} band(s) to {} -- tint {}, tooltip {}, price {}",
            std::size(kPalette), GridInvAPI::kHostPluginName,
            sent ? "delivered" : "not delivered (is Grid Inventory installed?)",
            sentAnnot ? "delivered" : "not delivered",
            sentPrice ? "delivered" : "not delivered");
    }

    // Takes EVERY sender. Only Grid Inventory's own 4CC types may be acted on
    // here -- see the note in GridTint.h.
    void OnApiMessage(SKSE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg || a_msg->type != GridInvAPI::kMsgHostReady) {
            return;
        }

        // MEASURE THE PAYLOAD BEFORE TRUSTING ANY OFFSET IN IT. This listener
        // hears every plugin in the load order, and another mod's message type
        // may collide with this 4CC by coincidence. structSize and abiVersion
        // both live inside the first eight bytes, so a short payload could
        // otherwise sail past the version check below.
        if (!a_msg->data || a_msg->dataLen < sizeof(GridInvAPI::HostReady)) {
            return;
        }
        const auto* ready = static_cast<const GridInvAPI::HostReady*>(a_msg->data);
        if (ready->abiVersion != GridInvAPI::kABIVersion ||
            ready->structSize != sizeof(GridInvAPI::HostReady)) {
            logger::warn("grid tint: host announced abi {} (we speak {}); not offering a table",
                ready->abiVersion, GridInvAPI::kABIVersion);
            return;
        }

        Offer();
    }
}

void GridTint::Install()
{
    // sender == nullptr: unfiltered, so the host's announcement actually lands.
    // Registered under a different sender key than the lifecycle listener in
    // main.cpp, which is what lets the two coexist.
    const bool ok = SKSE::GetMessagingInterface()->RegisterListener(nullptr, OnApiMessage);
    logger::info("grid tint: ABI listener registered: {}", ok ? "ok" : "FAILED");
}

bool GridTint::Registered()
{
    return g_registered.load();
}
