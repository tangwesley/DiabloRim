#pragma once
// =============================================================================
//  Grid Inventory -- extension ABI v1
// =============================================================================
//  This header is the ONLY thing shared between Grid Inventory (the host, which
//  owns the grid, the tiles and every pixel drawn) and an extension plugin (a
//  "provider", which owns gameplay data the host knows nothing about).
//
//  ABI DISCIPLINE -- the rules below are not style, they are the contract:
//
//   1. <cstdint> is the sole dependency. No CommonLibSSE-NG type, no Dear ImGui
//      type, no STL type ever crosses the boundary. A provider built against a
//      different CommonLib revision, a different ImGui version or a different
//      MSVC runtime must still link and run.
//   2. Every struct is POD with explicit padding and a static_assert on its
//      size. Adding a field to a released struct is a breaking change: bump
//      kABIVersion instead.
//   3. Strings are fixed char[] buffers, UTF-8, NUL-terminated. Never a pointer
//      the other side has to free.
//   4. Pointer arguments are borrowed for the duration of the call only.
//   5. The provider NEVER calls ImGui. It returns data; the host draws it with
//      its own draw list, its own atlas and its own theme. Sharing an ImGui
//      context across a DLL boundary would require sharing the context pointer
//      AND the allocator, and any version drift between the two plugins would
//      be an undiagnosable CTD.
//
//  THREADING -- GetOverlay / GetTooltipLines / OfferDrop / GetTier / GetLines are
//  all called from the host's render thread inside an active ImGui frame. Do not
//  block, allocate, or call a game API that can open or close a menu.
//
//  HANDSHAKE (SKSE messaging, both plugins register a listener in
//  SKSEPluginLoad so ordering does not matter):
//
//      kPostLoad :  host     --[ kMsgHostReady, const HostReady* ]--> everyone
//      kPostLoad :  provider --[ kMsgRegisterProvider, const Provider* ]--> "GridInventory"
//
//  ★AND ONE SIGNAL THAT RUNS THE OTHER WAY (1.4.1):
//
//      any time  :  host     --[ kMsgCostumeState, const CostumeState* ]--> everyone
//
//  Everything above is the outside EXTENDING our UI: we call a Provider and it
//  answers. The costume signal is the opposite direction -- we announce, nobody
//  answers -- so it needs no Provider and no registration. A plugin that does
//  not know this message simply never sees it, which is why adding it did NOT
//  require an ABI bump: message types are separate namespaces, and existing
//  extensions are untouched.
//
//  ★REGISTER TWO LISTENERS, SPLIT BY SENDER (learned the hard way, 2026-07-30):
//
//      RegisterListener(OnLifecycle);            // == RegisterListener("SKSE", ..)
//      RegisterListener(nullptr, OnApiMessage);  // no filter
//
//   Why both:
//    - The one-argument shorthand is RegisterListener("SKSE", cb), which filters
//      out everything a *plugin* sends. With only that, the handshake fails
//      silently in BOTH directions: the host's broadcast never arrives, and the
//      provider's Dispatch finds no matching listener and returns false.
//    - A single unfiltered listener cannot replace the pair, because message
//      type numbers live in the SENDER's namespace. Another plugin's "type 1"
//      is indistinguishable from kPostLoad and its "type 4" from kPostLoadGame,
//      so lifecycle handling would fire at random. Do NOT try to sort it out by
//      inspecting msg->sender -- what that string holds for SKSE's own messages
//      is not something to guess at, and guessing wrong silently disables every
//      lifecycle branch you own.
//    - The unfiltered listener must therefore touch ONLY the 4CC types below.
//      Registering both works because the two use different sender keys.
//
//  The host refuses any Provider whose abiVersion != kABIVersion or whose
//  structSize != sizeof(Provider). Refusal is a log line plus an in-game
//  notification -- never a CTD, never a silent no-op.
//
//  Vendor this file verbatim into a consumer project. Do not edit the copy.
// =============================================================================

#include <cstdint>

namespace GridInvAPI
{
    // ---- versioning -------------------------------------------------------

    inline constexpr std::uint32_t kABIVersion = 1;

    // SKSE plugin name the provider dispatches to, and the host's own name.
    inline constexpr const char* kHostPluginName = "GridInventory";

    inline constexpr std::uint32_t kMsgHostReady        = 0x47494852;  // 'GIHR'
    inline constexpr std::uint32_t kMsgRegisterProvider = 0x47495250;  // 'GIRP'
    inline constexpr std::uint32_t kMsgCostumeState     = 0x47494353;  // 'GICS'
    inline constexpr std::uint32_t kMsgRegisterTinter   = 0x47495443;  // 'GITC'
    inline constexpr std::uint32_t kMsgRegisterAnnot    = 0x4749414E;  // 'GIAN'

    // ---- limits -----------------------------------------------------------

    inline constexpr std::uint32_t kMaxBadges       = 8;    // D2 caps at 6; 8 is headroom
    inline constexpr std::uint32_t kTooltipTextLen  = 128;  // bytes incl. NUL
    inline constexpr std::uint32_t kMaxTooltipLines = 16;

    // ★A tint tier travels in THREE SPARE BITS of the host's existing glow
    // byte (bits 5..7), which is what keeps this feature free of signature
    // churn -- see Grid.h. Three bits is the whole budget: 0 means "no
    // opinion" and 1..7 are the tiers a tinter may claim.
    inline constexpr std::uint32_t kMaxTintTier = 7;

    // ---- item identity ----------------------------------------------------

    // The only identifier the two sides share. `uid` is ExtraUniqueID, which the
    // engine rewrites on every container move -- so this key is valid for the
    // current frame and must never be persisted by either side.
    //
    // uid == 0 means "a unit of the plain stack": the entry has no ExtraDataList
    // of its own. Such a unit can never carry provider data, and the host still
    // draws a tile for it (see the collection arithmetic in PLAN_INSTANCE 2-B).
    struct ItemKey
    {
        std::uint32_t owner;   // container REFR FormID; the player is 0x00000014
        std::uint32_t base;    // TESBoundObject FormID
        std::uint16_t uid;     // ExtraUniqueID, 0 = plain stack unit
        std::uint16_t _pad0;
        std::uint32_t _pad1;
    };
    static_assert(sizeof(ItemKey) == 16, "ItemKey is part of the ABI");

    // ---- badge overlay ----------------------------------------------------

    enum BadgeState : std::uint8_t
    {
        kBadgeEmpty  = 0,   // draw the empty well only
        kBadgeFilled = 1,   // draw iconId inside the well
        kBadgeLocked = 2    // well is present but cannot be used right now
    };

    // `iconId` is a TESBoundObject FormID, NOT an atlas index. The host already
    // renders an icon for any bound object through its own icon cache (model-path
    // hashed, both art styles, capture on demand), so a provider hands over the
    // form of the thing sitting in the well and gets the host's art pipeline for
    // free -- no shared texture handle, no shared atlas, nothing to version.
    //
    //   iconId == 0                      -> host draws its default empty well
    //   iconId != 0, state == kBadgeEmpty -> provider-supplied well art
    //   iconId != 0, state == kBadgeFilled -> that form's icon inside the well
    struct Badge
    {
        std::uint32_t iconId;    // TESBoundObject FormID; 0 = host default well
        std::uint32_t tintRGBA;  // 0 = host default tint
        std::uint8_t  state;     // BadgeState
        std::uint8_t  _pad[3];
    };
    static_assert(sizeof(Badge) == 12, "Badge is part of the ABI");

    // One instance's overlay. `count` is the total number of wells, which is what
    // the host feeds into its layout table together with the tile's width and
    // height -- the geometry is the host's business, the contents are not.
    struct Overlay
    {
        std::uint8_t count;      // 0 = this instance has no overlay
        std::uint8_t _pad[3];
        Badge        badges[kMaxBadges];
    };
    static_assert(sizeof(Overlay) == 100, "Overlay is part of the ABI");

    // ---- tooltip ----------------------------------------------------------

    struct TooltipLine
    {
        char          text[kTooltipTextLen];  // UTF-8, NUL-terminated
        std::uint32_t rgba;                   // 0 = host default colour
        std::uint8_t  indent;                 // 0..3
        std::uint8_t  separatorBefore;        // non-zero = rule above this line
        std::uint8_t  _pad[2];
    };
    static_assert(sizeof(TooltipLine) == 136, "TooltipLine is part of the ABI");

    // ---- drop routing -----------------------------------------------------

    enum DropVerdict : std::uint32_t
    {
        kDropReject  = 0,   // not ours -- host continues its own routing table
        kDropAccept  = 1,   // ours, and legal (commit != 0 means it happened)
        kDropBlocked = 2    // ours, but refused now -- host stops routing, shows refusal
    };

    struct DropQuery
    {
        ItemKey      target;        // instance the cursor is over
        ItemKey      dragged;       // instance being carried
        std::int32_t draggedCount;  // carried quantity (a split fragment may be < stack)
        std::int32_t badgeIndex;    // nearest badge, or -1 if the cursor is outside every well
        std::uint8_t commit;        // 0 = hover query (highlight only), 1 = perform
        std::uint8_t _pad[3];
    };
    static_assert(sizeof(DropQuery) == 44, "DropQuery is part of the ABI");

    // ---- host -> provider services ---------------------------------------

    // What the host's partner window currently is. A provider that must treat a
    // PURCHASE differently from LOOT cannot work this out for itself: the host
    // replaces the vanilla container and barter menus, so RE::BarterMenu is
    // never open during a merchant transaction and the transfer arrives looking
    // exactly like ordinary container loot.
    enum PartnerKind : std::uint32_t
    {
        kPartnerNone       = 0,   // plain inventory -- no partner window
        kPartnerContainer  = 1,   // chest / corpse / companion (incl. stealing)
        kPartnerMerchant   = 2,   // barter: money changes hands
        kPartnerPickpocket = 3    // living target
    };

    struct HostServices
    {
        std::uint32_t structSize;   // = sizeof(HostServices)
        std::uint32_t abiVersion;   // = kABIVersion

        // Ask the host to re-collect the grid on its next frame. Safe to call from
        // any thread; the host only sets a flag.
        void (*RequestRebuild)();

        // True while the grid menu is open. A provider that mutates inventory
        // should check this before doing anything the user could be looking at.
        bool (*IsMenuOpen)();

        // Grant-time tile snapshot: how many grid cells `base` occupies RIGHT NOW
        // under the host's resolved definition (per-form override > model-shared >
        // category preset), counting only the true cells of a polyomino mask --
        // an L-shaped item in a 2x3 bounding box is 5, not 6. The value follows
        // the user's live EDIT-mode tile changes, so a provider that rolls
        // per-instance state from it must call it AT THE MOMENT of the roll and
        // freeze the result; never re-derive persisted state from a later call.
        // Returns 0 when the form is unknown to the host. Main/game thread only
        // (reads the host's def maps without a lock).
        std::uint32_t (*CellSpanOf)(std::uint32_t base);

        // One of PartnerKind, valid for as long as the grid menu is open.
        // Read it while handling an inventory-transfer event to learn HOW the
        // item was acquired -- see the PartnerKind comment above for why the
        // engine's own event cannot answer that.
        std::uint32_t (*PartnerKind)();

        // FormID of the container / merchant / pickpocket target whose window is
        // open, or 0 in kPartnerNone. A provider that wants to act on what is
        // LYING IN a container (rather than on what reaches the player) needs
        // the reference itself, and only the host knows which one it opened.
        std::uint32_t (*PartnerRef)();
    };
    static_assert(sizeof(HostServices) == 48, "HostServices is part of the ABI");

    struct HostReady
    {
        std::uint32_t       structSize;  // = sizeof(HostReady)
        std::uint32_t       abiVersion;  // = kABIVersion
        const HostServices* services;    // owned by the host, valid for the process lifetime
    };
    static_assert(sizeof(HostReady) == 16, "HostReady is part of the ABI");

    // ---- costume state ----------------------------------------------------
    //
    //  A COSTUME is an appearance-only outfit: the body shows the set held by
    //  one loadout tab while the stats keep coming from what is really worn.
    //  Sent whenever that changes -- put on, switched to another tab, or taken
    //  off -- so a companion mod can follow what the player LOOKS like without
    //  reading equipment, which would tell it the wrong thing.
    //
    //  ★SENT ON EVERY TRANSITION, not only the ones a player causes. Loading a
    //  save that had a costume, reverting to a new game, and deleting the tab a
    //  costume pointed at all move this state, and all announce here. A listener
    //  that only handled the button presses would drift out of sync silently.
    //
    //  ★`pieces` IS BORROWED. It points into the host's own buffer and is valid
    //  for the duration of the callback and no longer -- copy what you need
    //  before returning. Length is `pieceCount`, which is 0 when `tab` is -1.
    //
    //  ★A PIECE IS A FORM, not a stack unit: `base` is the armour's FormID,
    //  `owner` is the player, and `uid` is 0. A costume names what to LOOK
    //  like; it does not point at one particular copy in the inventory.
    //
    //  ★ARMOUR ONLY. The loadout tab behind a costume may also hold weapons, a
    //  shield and a quiver -- a costume leaves all of those alone, because they
    //  are held rather than worn. Only the pieces that actually reach the body
    //  are listed here, so every entry is something the player is now seen in.
    struct CostumeState
    {
        std::uint32_t  structSize;   // = sizeof(CostumeState)
        std::uint32_t  abiVersion;   // = kABIVersion
        std::int32_t   tab;          // loadout tab supplying the look; -1 = none
        std::uint32_t  pieceCount;   // 0 when tab is -1
        const ItemKey* pieces;       // BORROWED: valid only during the callback
    };
    static_assert(sizeof(CostumeState) == 24, "CostumeState is part of the ABI");

    // ---- provider -> host ------------------------------------------------

    struct Provider
    {
        std::uint32_t structSize;   // = sizeof(Provider)
        std::uint32_t abiVersion;   // = kABIVersion
        const char*   name;         // static string, diagnostics only
        void*         self;         // opaque; handed back as the first argument

        // HOT PATH -- called once per visible tile per frame, and again per
        // equipment-doll slot. Must be a hash lookup and nothing more.
        // Return false to say "nothing to draw"; *out is then untouched.
        bool (*GetOverlay)(void* self, const ItemKey* key, Overlay* out);

        // Fill up to `capacity` lines into the host-owned buffer, return how many
        // were written. Called only while the tooltip for `key` is being built.
        std::uint32_t (*GetTooltipLines)(void* self, const ItemKey* key,
                                         TooltipLine* out, std::uint32_t capacity);

        // Consulted before the host's own drop routing tables, for both whole-tile
        // and split-fragment drops. Returns a DropVerdict.
        std::uint32_t (*OfferDrop)(void* self, const DropQuery* query);
    };
    static_assert(sizeof(Provider) == 48, "Provider is part of the ABI");

    // ---- provider -> host: RARITY TINT -------------------------------------

    // ★★A SECOND, SEPARATE TABLE -- deliberately NOT a fourth hook on Provider.
    //
    // Provider is `static_assert(sizeof(Provider) == 48)` and the host refuses
    // any table whose structSize disagrees, so growing it would silently unload
    // every extension already built against ABI 1. Worse, the host keeps ONE
    // provider slot: an extension that only wants to colour items would have to
    // out-compete whatever registered first and lose its badges to do it.
    //
    // A tinter is its own slot, its own message, and its own handshake. The two
    // are orthogonal -- a plugin may register either, both, or neither -- and
    // kABIVersion does not move, because nothing that exists today can tell the
    // difference.
    //
    // ★★★WHY THE HOST ASKS, RATHER THAN BEING TOLD. Rarity is per-INSTANCE, and
    // an instance has no name the two sides can share: ItemKey.uid is
    // ExtraUniqueID, which is 0 for exactly the items this exists to colour --
    // the engine never assigns one to a merely renamed or enchanted list, so
    // every such unit collapses onto the same key. The host therefore hands
    // over the pointer it is ALREADY HOLDING while it draws, and the tinter
    // reads whatever it put there itself. That pointer is borrowed FOR THE
    // DURATION OF THE CALL and must never be stored: the engine rewrites these
    // lists on every container move.
    struct Tinter
    {
        std::uint32_t structSize;   // = sizeof(Tinter)
        std::uint32_t abiVersion;   // = kABIVersion
        const char*   name;         // static string, diagnostics only
        void*         self;         // opaque; handed back as the first argument

        // HOT PATH -- once per visible tile per frame, again per doll slot, and
        // again while a tooltip is built. Must be a hash lookup and nothing
        // more: no allocation, no form lookup, no lock.
        //
        //   base  the TESBoundObject FormID
        //   xl    the RE::ExtraDataList* of THIS sub-stack, or nullptr when the
        //         unit has none of its own. Read-only, borrowed for the call.
        //
        // Return 0 for "no opinion" -- the host then draws exactly what it drew
        // before this table existed. Otherwise 1..kMaxTintTier; anything above
        // is clamped by the host rather than refused, so a tinter built against
        // a later ABI degrades instead of vanishing.
        std::uint8_t (*GetTier)(void* self, std::uint32_t base, const void* xl);

        // The colour for each tier, ONCE, at registration -- not per frame.
        // Fill out[0] for tier 1, out[1] for tier 2, and so on; return how many
        // were written. Packed 0xAABBGGRR, matching ImU32 on this build, which
        // is what lets the host hand it straight to ImGui with no conversion.
        //
        // A tier the palette does not cover is drawn as if the tinter had
        // returned 0 for it, so a short palette is a narrower feature and never
        // a wrong colour.
        std::uint32_t (*GetPalette)(void* self, std::uint32_t* out, std::uint32_t capacity);
    };
    static_assert(sizeof(Tinter) == 40, "Tinter is part of the ABI");

    // ---- provider -> host: TOOLTIP ANNOTATION ------------------------------

    // ★★A THIRD TABLE, FOR THE SAME REASON THERE WAS A SECOND.
    //
    // Provider already has a GetTooltipLines and it CANNOT DO THIS JOB. It is
    // handed an ItemKey and nothing else, and ItemKey.uid is ExtraUniqueID --
    // 0 for every merely renamed or enchanted unit, which is precisely the
    // population an extension has something to say about. Every affixed iron
    // sword in a chest arrives at that hook under the same key as every plain
    // one, so a line built from it would be right by luck or not at all. The
    // reasoning that gave Tinter its `xl` argument gives this its own table.
    //
    // ★★WHY NOT SIMPLY GROW Tinter. Rule 2 at the top of this file: adding a
    // field to a released struct is a breaking change, and the host refuses any
    // table whose structSize disagrees. Growing Tinter would stop every already
    // built extension from colouring anything, and bumping kABIVersion to cover
    // that would ALSO refuse every existing Provider -- a socket mod losing its
    // badges because a loot mod wanted a tooltip line. Additive costs nobody
    // anything: kABIVersion does not move, no released struct changes size, and
    // a plugin that has never heard of this message simply never sends one.
    //
    // Provider, Tinter and Annotator are three independent slots. Register any
    // combination; holding one has never implied holding another.
    struct Annotator
    {
        std::uint32_t structSize;   // = sizeof(Annotator)
        std::uint32_t abiVersion;   // = kABIVersion
        const char*   name;         // static string, diagnostics only
        void*         self;         // opaque; handed back as the first argument

        // Lines to add to ONE unit's tooltip. Return how many were written into
        // `out`. Zero means "nothing to say about this one", which is the
        // answer for most of an inventory and must stay cheap.
        //
        //   base      the TESBoundObject FormID
        //   xl        the RE::ExtraDataList* of THIS sub-stack, or nullptr when
        //             the unit has none of its own. Read-only, and BORROWED FOR
        //             THE DURATION OF THE CALL -- the engine rewrites these
        //             lists on every container move, so a stored copy is a
        //             dangling pointer by the next transfer.
        //   out       host-owned buffer of `capacity` lines, never null.
        //
        // NOT the hot path: once per tooltip, not once per tile per frame. It
        // still runs inside the host's ImGui frame on the render thread, so do
        // not block, do not open or close a menu, and never call ImGui.
        //
        // ★NEVER WRITE MORE THAN `capacity` LINES. The host clamps the return
        // value, which protects its own reads -- it cannot undo a write past
        // the end of its buffer.
        std::uint32_t (*GetLines)(void* self, std::uint32_t base, const void* xl,
                                  TooltipLine* out, std::uint32_t capacity);
    };
    static_assert(sizeof(Annotator) == 32, "Annotator is part of the ABI");
}
