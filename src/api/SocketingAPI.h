// SPDX-License-Identifier: MIT
// SocketingAPI.h - Socketing's interface for other SKSE plugins (interface version 2, Socketing 0.6.0+; version 1 since 0.5.0).
//
// This header alone is MIT-licensed so that any plugin, whatever its licence, can copy it in; Socketing itself is
// GPL-3.0-or-later. The reference is api/README.md next to it (shipped as SKSE/Plugins/Socketing/API/).
//
// Getting the interface (any time from SKSE's kPostLoad on; nullptr when Socketing isn't loaded):
//
//     auto* soc = SocketingAPI::GetInterface();           // needs CommonLibSSE (SKSE/SKSE.h included before this)
//     auto* soc2 = SocketingAPI::GetInterface002();       // 0.6.0+: version 2 (tooltip lines); nullptr on 0.5.0
//
// or, without the helper, over SKSE messaging: Dispatch(kGetInterface, &msg, sizeof msg, "Socketing") with a
// GetInterfaceMessage, answered inline; or GetProcAddress(GetModuleHandleA("Socketing.dll"), "SocketingGetInterface"),
// a `void* (*)(std::uint32_t version)`. All three give the same object, which lives as long as the game.
//
// Rules:
//   - Call on the game's main thread, as for any game data (UI code, hooks and SKSE tasks are; your own threads
//     are not). The catalog (shards, gems, phrases) is filled at SKSE's kDataLoaded: IsReady() says so, and the
//     kDataLoaded message below is sent then.
//   - Strings are UTF-8, NUL-terminated, in the fixed buffers of plain structs. A "key" is the English name of a
//     shard, gem or phrase ("Fus", "Flawless Ruby", "Crescendo"): stable across versions and translations, and the
//     name of the icon file. A "name" is what the player reads (translated).
//   - An inventory item is its base object plus its ExtraDataList (RE::InventoryEntryData::extraLists): the
//     sockets live on that list, so a plain stack without one is never socketed. For a world reference pass
//     &ref->extraList. The same pair works for any actor's or container's items, not only the player's.
//   - Events: RegisterListener("Socketing", handler) (at or after kPostLoad) receives kDataLoaded and
//     kItemChanged (data: const ItemChanged*). Papyrus scripts get the mod event "Socketing_ItemChanged".
//   - DescribeItem() and CatalogJson() give everything the structs hold and more (the item card's text, the
//     phrase card's HTML) as JSON, for a consumer that would rather parse than bind: fields are only added.

#pragma once
#include <cstdint>

namespace RE
{
	class Actor;
	class ExtraDataList;
	class TESBoundObject;
	class TESForm;
}

namespace SocketingAPI
{
	inline constexpr const char*   kPluginName = "Socketing";   // the SKSE messaging name (sender and receiver)
	inline constexpr std::uint32_t kInterfaceVersion = 2;   // the newest; every older version is still served

	// SKSE messaging types (SKSE::MessagingInterface::Message::type)
	enum MessageType : std::uint32_t
	{
		kGetInterface = 0x534F4B01,   // you -> Socketing, data = GetInterfaceMessage*: filled in before Dispatch returns
		kDataLoaded = 0x534F4B02,     // Socketing -> everyone: the catalog is loaded (after SKSE's kDataLoaded); no data
		kItemChanged = 0x534F4B03,    // Socketing -> everyone: an item's sockets changed; data = const ItemChanged*
	};

	struct GetInterfaceMessage
	{
		void* (*GetInterface)(std::uint32_t a_version) = nullptr;   // set by Socketing; nullptr result: unknown version
	};

	// which kind of gear an item is for socketing: its stats and phrases differ by side
	enum class Side : char
	{
		kNone = 0,
		kWeapon = 'W',   // every weapon, staves included
		kBody = 'A',     // body armour and robes
		kOther = 'H',    // helmets, circlets, hats and hoods, shields, gloves, boots, cloaks
	};
	inline constexpr std::uint32_t kSideCount = 3;   // per-side arrays below are indexed W 0, A 1, H 2
	inline int SideIndex(char a_side) { return a_side == 'W' ? 0 : a_side == 'A' ? 1 : a_side == 'H' ? 2 : -1; }

	enum Tier : std::int32_t
	{
		kTierLow = 0,    // shards, level 1+
		kTierMid = 1,    // level 20+
		kTierHigh = 2,   // level 35+
		kTierGem = 9,    // a vanilla gem (no phrases, small bonuses)
	};

	// why an item can't take sockets (CanSocket)
	enum SocketableReason : std::uint32_t
	{
		kCanSocket = 0,
		kNotGear = 1,           // not a kind of item that takes sockets (no weapon or armour, or a kind with no sockets)
		kNotPlayable = 2,
		kUniqueItem = 3,        // artifacts and uniques (the game's MagicDisallowEnchanting keyword)
		kEnchanted = 4,         // enchanted by the game or the player: an item is either enchanted or socketed
		kAlreadySocketed = 5,   // it has sockets already (free or not)
	};

	inline constexpr std::uint32_t kMaxSockets = 8;    // the game data allows 6 today
	inline constexpr std::uint32_t kKeyLength = 32;
	inline constexpr std::uint32_t kNameLength = 128;
	inline constexpr std::uint32_t kTextLength = 512;

	// one socketed item
	struct ItemSockets
	{
		std::uint8_t  sockets = 0;                        // sockets on the item
		std::uint8_t  filled = 0;                         // of them holding a shard or gem
		char          side = 0;                           // Side
		char          shards[kMaxSockets][kKeyLength];    // socket by socket, in order: the socketable's key, "" for an empty socket
		char          phrase[kKeyLength];                 // the completed phrase's key, "" when the sockets spell none
		char          phraseName[kNameLength];            // ... and its translated name: the item is called this while the phrase holds
		char          baseName[kNameLength];              // the item's own name from before socketing (no temper suffix): what it is called again
		                                                  // once the phrase breaks, and what to call it if you show the phrase separately
		std::uint32_t enchantment = 0;                    // FormID of the created enchantment that carries the sockets: unique per socketed
		                                                  // item and constant for its life (a key to cache by)
		std::uint32_t origEnchantment = 0;                // the item's own enchantment before socketing, 0 for none (always 0 since 0.22)
		std::int32_t  valueBonus = 0;                     // gold the sockets add to the game's item value (the game's figure already has it)
		float         damageMult = 1.0f;                  // weapons: the "+% damage" of shards and phrase as a factor (1 = none); the game
		                                                  // counts it only while the weapon is worn, so an unworn weapon's shown damage x this
	};

	// a shard or gem
	struct Socketable
	{
		char          key[kKeyLength];                    // "Fus", "Flawless Ruby"
		char          name[kNameLength];                  // translated: "Fus", "Flawless Ruby"
		char          meaning[kNameLength];               // shards: what the dragon word means ("Force"); gems: ""
		std::int32_t  tier = 0;                           // Tier
		std::int32_t  rank = 0;                           // the order of the catalog: shards low to high (Ro 0 .. Joor 32), then gems
		std::uint32_t color = 0;                          // 0xRRGGBB: the tier's colour for shards, the stone's for gems (the menus' tint)
		std::uint32_t formId = 0;                         // the inventory item (a MISC item: Socketing.esp's shards, Skyrim.esm's gems)
		std::int32_t  value = 0;                          // its gold value
		char          iconDds[kNameLength];               // "Textures\Socketing\Icons\Shards\Fus.dds": 128 x 128 RGBA (DXT5), a path under Data
		char          iconPng[kNameLength];               // the same as PNG, for building into your own SWF or atlas
		char          stats[kSideCount][kTextLength];     // what it gives on each side, as the item card words it: "+10% damage" (W),
		                                                  // "+10% block" (A), "+10% warrior skill experience" (H); several joined by ", "
	};

	// a phrase: the right shards in the right order on the right kind of item
	struct Phrase
	{
		char          key[kKeyLength];                    // "Crescendo"
		char          name[kNameLength];                  // translated
		char          sides[kSideCount + 1];              // the sides it is for, as letters: "W", "WH", "AH"
		std::uint8_t  count = 0;                          // its shards (= the sockets the item must have, all filled)
		char          shards[kMaxSockets][kKeyLength];    // in order
		char          bases[kSideCount][kNameLength];     // per side, what it goes on: "melee weapon", "shield / melee weapon"; "" for a side it isn't for
		char          bonus[kSideCount][kTextLength];     // per side, its bonus in words (translated), on top of its shards' stats
		std::int32_t  value = 0;                          // gold a completed phrase adds to the item's value
	};

	// kItemChanged: an item's sockets were set, filled, emptied or removed; also sent when Socketing rebuilds an item
	// after a game load or data change. The pointers are valid during the callback.
	struct ItemChanged
	{
		RE::TESBoundObject* object = nullptr;
		RE::ExtraDataList*  extraList = nullptr;
		RE::Actor*          owner = nullptr;   // who carries it: the player for anything in the player's inventory (null: a container's)
		bool                removed = false;   // true: the sockets were taken off (GetItemSockets fails for it now)
	};

	// one line of an item's tooltip (ISocketing002::GetTooltipLines). The same layout as Grid Inventory's TooltipLine
	// (GridInventoryAPI.h), so it copies straight into that host's buffer.
	struct TooltipLine
	{
		char          text[128];             // UTF-8, NUL-terminated, at most 127 bytes
		std::uint32_t rgba = 0;              // ImGui's IM_COL32 packing (0xAABBGGRR, alpha FF); 0 = your default colour
		std::uint8_t  indent = 0;            // always 0 from Socketing
		std::uint8_t  separatorBefore = 0;   // non-zero on the first line: a rule between your lines and Socketing's
		std::uint8_t  pad[2]{};
	};
	static_assert(sizeof(TooltipLine) == 136, "TooltipLine is part of the API");

	class ISocketing001
	{
	public:
		virtual std::uint32_t GetInterfaceVersion() const = 0;   // kInterfaceVersion of this object
		virtual const char*   GetPluginVersion() const = 0;      // "0.5.0"
		virtual bool          IsReady() const = 0;               // the catalog is loaded (kDataLoaded was sent)
		virtual bool          IsActive() const = 0;              // the mod acts for the loaded character (Socketing.ini OnlyCharacter)

		// items
		virtual bool          GetItemSockets(RE::TESBoundObject* a_object, RE::ExtraDataList* a_extraList, ItemSockets& a_out) const = 0;   // false: not socketed
		virtual std::uint32_t CanSocket(RE::TESBoundObject* a_object, RE::ExtraDataList* a_extraList) const = 0;   // SocketableReason
		virtual std::int32_t  MaxSockets(RE::TESBoundObject* a_object) const = 0;   // most sockets this kind of item can get (0: none)
		// everything about one socketed item as JSON (api/README.md): writes up to a_capacity bytes (NUL-terminated),
		// returns the bytes the whole text needs including the NUL (call again with that), 0 when not socketed
		virtual std::uint32_t DescribeItem(RE::TESBoundObject* a_object, RE::ExtraDataList* a_extraList, char* a_buffer, std::uint32_t a_capacity) const = 0;

		// shards and gems, in rank order
		virtual std::uint32_t CountSocketables() const = 0;
		virtual bool          GetSocketable(std::uint32_t a_index, Socketable& a_out) const = 0;
		virtual bool          FindSocketable(const char* a_key, Socketable& a_out) const = 0;
		virtual bool          SocketableOf(RE::TESForm* a_item, Socketable& a_out) const = 0;   // the shard or gem an inventory item is

		// phrases
		virtual std::uint32_t CountPhrases() const = 0;
		virtual bool          GetPhrase(std::uint32_t a_index, Phrase& a_out) const = 0;
		virtual bool          FindPhrase(const char* a_key, Phrase& a_out) const = 0;

		// the whole catalog as JSON (api/README.md): socketables, phrases, tiers and colours, side names, the phrase
		// cards' HTML. Built once per data load; the pointer stays valid until the next load (copy it if you keep it)
		virtual const char*   CatalogJson() const = 0;
	};

	// version 2 (Socketing 0.6.0): everything of version 1, plus
	class ISocketing002 : public ISocketing001
	{
	public:
		// what a socketed item's sockets do, as tooltip lines (Socketing's effects are hidden from the game's own
		// enchantment text, so a UI that lists an item's effects shows none of them): the socket line first ("Fus ·
		// Ro · Empty socket", grey), then one line per effect sentence (the shards' stats in your default colour,
		// what isn't built yet in grey, the phrase's bonus and aura in gold), translated, as Socketing's item card
		// shows them. Writes at most a_capacity lines; returns how many it wrote, 0 when the item isn't socketed.
		virtual std::uint32_t GetTooltipLines(RE::TESBoundObject* a_object, RE::ExtraDataList* a_extraList, TooltipLine* a_out, std::uint32_t a_capacity) const = 0;
	};

#if __has_include(<SKSE/SKSE.h>)
	// the interface, over SKSE messaging (synchronous); nullptr when Socketing isn't loaded
	inline ISocketing001* GetInterface()
	{
		static ISocketing001* cached = nullptr;
		if (cached) return cached;
		auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging) return nullptr;
		GetInterfaceMessage msg;
		messaging->Dispatch(kGetInterface, &msg, sizeof(msg), kPluginName);
		if (!msg.GetInterface) return nullptr;
		cached = static_cast<ISocketing001*>(msg.GetInterface(1));
		return cached;
	}

	// version 2 (tooltip lines), the same way; nullptr when Socketing isn't loaded or is older than 0.6.0
	inline ISocketing002* GetInterface002()
	{
		static ISocketing002* cached = nullptr;
		if (cached) return cached;
		auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging) return nullptr;
		GetInterfaceMessage msg;
		messaging->Dispatch(kGetInterface, &msg, sizeof(msg), kPluginName);
		if (!msg.GetInterface) return nullptr;
		cached = static_cast<ISocketing002*>(msg.GetInterface(2));
		return cached;
	}
#endif
}
