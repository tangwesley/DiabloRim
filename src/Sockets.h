// =============================================================================
//  Socketing -- reading the sockets another mod put on an item.
// =============================================================================
//  Socketing (Nexus 195740) gives unenchanted weapons and armour sockets, and
//  shards and gems to put in them. This mod does not take part in any of that;
//  it only READS it, through the interface Socketing publishes for exactly this
//  (api/SocketingAPI.h, vendored verbatim, MIT), so that Grid Inventory can
//  draw the wells and the stones in them. GridTint.cpp is where the answer is
//  handed to the grid.
//
//  THE CREATED ENCHANTMENT IS THE KEY, as it is for our own tiers. Socketing
//  keeps its record on a created enchantment of the item's own, unique to the
//  item and constant for its life, so an item with no ExtraEnchantment is not
//  socketed -- which settles nearly every unit with one extra-data read -- and
//  one that has one is asked about once and remembered.
//
//  Absent Socketing this file does nothing at all: Connect() finds no
//  interface, no table is offered to the grid, and RowOf answers nothing.
// =============================================================================
#pragma once

#include "api/SocketingAPI.h"

#include <cstdint>
#include <span>

namespace Sockets
{
    // Socketing's own ceiling (SocketingAPI::kMaxSockets). The game data uses 6.
    inline constexpr std::uint32_t kMaxSockets = 8;

    // One unit's sockets, as Socketing reported them.
    struct Row
    {
        std::uint8_t  count = 0;              // sockets on the item; 0 = not socketed
        std::uint32_t shard[kMaxSockets]{};   // the shard or gem item's FormID, socket by socket; 0 = empty
        std::uint32_t rgb[kMaxSockets]{};     // its colour, 0xRRGGBB: the shard's tier, or the gem's stone
    };

    // Finds Socketing and subscribes to its change events. Safe to call more
    // than once (only the first call does anything) and must be called at or
    // after SKSE's kPostLoad: before that, Socketing may not be loaded yet.
    // False when Socketing is not installed, which is not an error.
    bool Connect();

    // The sockets on one unit, or nullptr when it has none.
    //
    // HOT PATH: the grid asks once per visible cell per frame on its partner
    // window and equipment doll. One extra-data read and one hash lookup once
    // an item has been seen; Socketing itself is asked only the first time.
    [[nodiscard]] const Row* RowOf(std::uint32_t a_base, const RE::ExtraDataList* a_xl);

    // Socketing's picture of one shard or gem item, as a path relative to the
    // Data folder, into a_out. False when a_form is not a socketable.
    bool ArtOf(std::uint32_t a_form, char* a_out, std::uint32_t a_capacity);

    // What one unit's sockets do, as Socketing words it for a tooltip: the
    // socket line ("Fus · Ro · Empty socket") then one line per effect, in
    // Socketing's colours. Empty for a unit that is not socketed, and always
    // empty on Socketing older than 0.6.0, which has no such call.
    //
    // Asked of Socketing once per unit and kept, like the row; valid until
    // the next change Socketing announces, so copy it out before then.
    [[nodiscard]] std::span<const SocketingAPI::TooltipLine> LinesOf(std::uint32_t a_base,
        const RE::ExtraDataList* a_xl);

    // Drops every remembered row. A save is about to load or a new game to
    // start, and created enchantments' FormIDs are issued per save: the same
    // number can name another item's enchantment in the next one.
    void Forget();
}
