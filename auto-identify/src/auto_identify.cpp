// auto-identify - D2RLoader plugin
//
// Identifies an item when a player picks it up, never when it drops. The item
// qualities to identify come from the [auto-identify] table of the config.
// Optionally hides the affix names of magic items and the rare-name line of
// rare / crafted items, so only the base item name is shown.
//
// Hook: D2R's server pickup routine (maybe_D2GAME_ITEMS_PickupItem). Packet 0x16
// and the player-mode pickup paths all reach it. When it returns success the
// item is looked up by GUID on the player's cursor and in the inventory list,
// so an item the pickup destroyed (gold, auto-stack, tome refill) is never touched.
// Identification goes through the game's own D2GAME_ITEMS_Identify, which also
// sends the updated item to every client.
//
// Name hook: the client item-name builder (maybe_D2CLIENT_ITEMS_BuildName). Every
// game name display reaches it through D2RCore's GetItemName export. For a hidden
// quality the identified flag is cleared just for the call, so the builder takes
// its unidentified branch, which writes only the base name.
//
// All RVAs are relative to the D2RLoader.exe image (ctx->exeBase) and were read
// from /d2rloader_full.dmp in Ghidra; evidence is in notes/auto-identify.md of
// the workspace. Every hook and called function is guarded by its expected bytes.

#include <D2RLPlugin/api.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// RE-supplied constants (D2RLoader.exe RVAs).
// ---------------------------------------------------------------------------
namespace Rva {
// maybe_D2GAME_ITEMS_PickupItem(pPlayer, nItemGUID, bToCursor, nMaxDistance, bCheckCollision, bToCube)
// entry (inline hook).
inline constexpr uint64_t PickupItem = 0x471950;
// D2GAME_ITEMS_Identify(pGame, pPlayer, pItem, bFlag). The check covers the bytes
// after its 15-byte prologue, which gamble-tweaks may have hooked already.
inline constexpr uint64_t IdentifyItem      = 0x46e8c0;
inline constexpr uint64_t IdentifyItemCheck = 0x46e8cf;
// maybe_UNITS_GetInventory(pUnit, szFile, nLine).
inline constexpr uint64_t GetInventory = 0x34a360;
// INVENTORY_GetCursorItem(pInventory).
inline constexpr uint64_t GetCursorItem = 0x388a70;
// INVENTORY_GetFirstItem(pInventory).
inline constexpr uint64_t GetFirstItem = 0x388c10;
// INVENTORY_GetNextItem(pItem).
inline constexpr uint64_t GetNextItem = 0x38aba0;
// maybe_UNITS_GetUnitId(pUnit, szFile, nLine).
inline constexpr uint64_t GetUnitId = 0x34a330;
// SUNIT_GetGameFromUnit(pUnit): returns pUnit->pGame (+0xD8).
inline constexpr uint64_t GetGameFromUnit = 0x48ff00;
// maybe_D2CLIENT_ITEMS_BuildName(pItem, szName /* 0x400 */). The entry starts with
// TEST RCX,RCX / JZ rel32; the hook goes right after them so no branch is moved.
// The stack there is the same as at entry and RCX is known to be non-null.
inline constexpr uint64_t BuildItemName     = 0x1e1d80;
inline constexpr uint64_t BuildItemNameHook = 0x1e1d89;
} // namespace Rva

// Pickup prologue: MOV [RSP+0x18],RBX; PUSH RBP; PUSH RSI; PUSH RDI; PUSH R12;
// PUSH R13; PUSH R14; PUSH R15 (5,1,1,1,2,2,2,2 = 16 bytes), no RIP-relative operand.
static constexpr uint8_t kPickupExpected[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
	                                           0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
// D2GAME_ITEMS_Identify +0xF: MOV RBP,R8; MOVZX R12D,R9B; MOV R14,RDX (3,4,3).
static constexpr uint8_t kIdentifyCheckExpected[] = { 0x49, 0x8B, 0xE8, 0x45, 0x0F, 0xB6, 0xE1, 0x4C, 0x8B, 0xF2 };
static constexpr uint8_t kGetInventoryExpected[]  = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF1 };
static constexpr uint8_t kGetCursorItemExpected[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x75, 0x1B };
static constexpr uint8_t kGetFirstItemExpected[]  = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x74, 0x2E };
static constexpr uint8_t kGetNextItemExpected[]   = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x75, 0x10 };
static constexpr uint8_t kGetUnitIdExpected[]     = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D };
static constexpr uint8_t kGetGameExpected[]       = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x75, 0x20 };
// Name builder entry: TEST RCX,RCX; JZ +0x1509 (3,6). Checked, not patched.
static constexpr uint8_t kBuildNameEntryExpected[] = { 0x48, 0x85, 0xC9, 0x0F, 0x84, 0x09, 0x15, 0x00, 0x00 };
// Hook site: PUSH RBP; PUSH RSI; PUSH R12; LEA RBP,[RSP-0x1600]; MOV EAX,0x1700
// (1,1,2,8,5 = 17 bytes), no RIP-relative operand. The CALL __chkstk after it is not moved.
static constexpr uint8_t kBuildNameHookExpected[] = { 0x55, 0x56, 0x41, 0x54, 0x48, 0x8D, 0xAC, 0x24, 0x00,
	                                                  0xEA, 0xFF, 0xFF, 0xB8, 0x00, 0x17, 0x00, 0x00 };

// D2UnitStrc / D2ItemDataStrc offsets used by the hook.
namespace Off {
inline constexpr size_t UnitType        = 0x00; // dwUnitType (maybe_UNITS_GetUnitType)
inline constexpr size_t UnitData        = 0x10; // pItemData
inline constexpr size_t ItemDataQuality = 0x00; // dwQualityNo (ITEMS_GetTransactionCost switch 4..9)
inline constexpr size_t ItemDataFlags   = 0x18; // dwItemFlags (ITEMS_CheckItemFlag)
} // namespace Off

inline constexpr uint32_t kUnitTypePlayer  = 0;
inline constexpr uint32_t kUnitTypeItem    = 4;
inline constexpr uint32_t kItemFlagIdentified = 0x10; // IFLAG_IDENTIFIED
inline constexpr uint32_t kQualityMax      = 9;    // ITEMQUAL_TEMPERED
inline constexpr uint32_t kQualityMagic    = 4;
inline constexpr uint32_t kQualityRare     = 6;
inline constexpr uint32_t kQualityCrafted  = 8;
// Upper bound on the inventory walk; the list is never this long in practice.
inline constexpr int kMaxInventoryItems = 4096;

// ---------------------------------------------------------------------------
// Config.
// ---------------------------------------------------------------------------
struct Config {
	bool     enabled     = true;
	// Bit n set = identify items of quality n (ITEMQUAL_*, 1..9).
	uint32_t qualityMask = (1U << 4) | (1U << 5) | (1U << 6) | (1U << 7) | (1U << 8);
	// Bit n set = show only the base name for identified items of quality n.
	// Set by hide_magic_affixes (4), hide_rare_names (6) and hide_crafted_names (8).
	uint32_t hideNameMask = 0;
};

static Config g_config;

namespace {

auto trim(const std::string& s) -> std::string {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return "";
	}
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

auto lower(std::string s) -> std::string {
	for (char& c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

auto parseBool(const std::string& v, bool fallback) -> bool {
	std::string t = trim(v);
	if (t == "true" || t == "1") {
		return true;
	}
	if (t == "false" || t == "0") {
		return false;
	}
	return fallback;
}

// Quality name or number -> ITEMQUAL_* id, 0 if unknown.
auto parseQuality(const std::string& token) -> uint32_t {
	static constexpr std::array<const char*, kQualityMax + 1> kNames {
		"", "inferior", "normal", "superior", "magic", "set", "rare", "unique", "crafted", "tempered",
	};
	std::string t = lower(trim(token));
	if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'') && t.back() == t.front()) {
		t = trim(t.substr(1, t.size() - 2));
	}
	if (t.empty()) {
		return 0;
	}
	if (t == "craft") {
		return 8;
	}
	for (uint32_t id = 1; id <= kQualityMax; ++id) {
		if (t == kNames[id]) {
			return id;
		}
	}
	char* end        = nullptr;
	const auto value = std::strtoul(t.c_str(), &end, 10);
	if (end != nullptr && *end == '\0' && value >= 1 && value <= kQualityMax) {
		return static_cast<uint32_t>(value);
	}
	return 0;
}

auto stripComment(const std::string& line) -> std::string {
	// '#' outside a quoted string starts a comment.
	char quote = 0;
	for (size_t i = 0; i < line.size(); ++i) {
		const char c = line[i];
		if (quote != 0) {
			if (c == quote) {
				quote = 0;
			}
		} else if (c == '"' || c == '\'') {
			quote = c;
		} else if (c == '#') {
			return line.substr(0, i);
		}
	}
	return line;
}

// Minimal reader for the [auto-identify] table: scalar keys and the (possibly
// multi-line) qualities array. Unknown quality names are reported and skipped.
template <typename WarnFn>
void ParseConfig(const char* toml, Config& out, WarnFn warn) {
	if (toml == nullptr) {
		return;
	}
	std::string text(toml);
	std::string section;
	std::string pendingKey;  // key whose array value continues on the next lines
	std::string pendingValue;
	size_t pos = 0;

	auto applyQualities = [&](const std::string& raw) {
		std::string v = trim(raw);
		if (v.empty() || v.front() != '[' || v.back() != ']') {
			warn("auto-identify: 'qualities' must be an array; keeping the default.");
			return;
		}
		v             = v.substr(1, v.size() - 2);
		uint32_t mask = 0;
		size_t start  = 0;
		while (start <= v.size()) {
			size_t comma      = v.find(',', start);
			std::string token = trim(v.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
			if (!token.empty()) {
				const uint32_t id = parseQuality(token);
				if (id == 0) {
					char msg[160] {};
					std::snprintf(msg, sizeof(msg), "auto-identify: unknown quality '%s' ignored.", token.c_str());
					warn(msg);
				} else {
					mask |= 1U << id;
				}
			}
			if (comma == std::string::npos) {
				break;
			}
			start = comma + 1;
		}
		out.qualityMask = mask;
	};

	while (pos < text.size()) {
		size_t nl        = text.find('\n', pos);
		std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
		pos              = (nl == std::string::npos) ? text.size() : nl + 1;
		line             = trim(stripComment(line));

		if (!pendingKey.empty()) {
			pendingValue += ' ' + line;
			if (pendingValue.find(']') != std::string::npos) {
				applyQualities(pendingValue);
				pendingKey.clear();
				pendingValue.clear();
			}
			continue;
		}
		if (line.empty()) {
			continue;
		}
		if (line.front() == '[' && line.back() == ']' && line.find('=') == std::string::npos) {
			section = trim(line.substr(1, line.size() - 2));
			continue;
		}
		if (section != "auto-identify") {
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		std::string key = trim(line.substr(0, eq));
		std::string val = trim(line.substr(eq + 1));
		auto setHide = [&](uint32_t quality) {
			const uint32_t bit = 1U << quality;
			out.hideNameMask   = parseBool(val, (out.hideNameMask & bit) != 0) ? (out.hideNameMask | bit) : (out.hideNameMask & ~bit);
		};
		if (key == "enabled") {
			out.enabled = parseBool(val, out.enabled);
		} else if (key == "hide_magic_affixes") {
			setHide(kQualityMagic);
		} else if (key == "hide_rare_names") {
			setHide(kQualityRare);
		} else if (key == "hide_crafted_names") {
			setHide(kQualityCrafted);
		} else if (key == "qualities") {
			if (!val.empty() && val.front() == '[' && val.find(']') == std::string::npos) {
				pendingKey   = key;
				pendingValue = val;
			} else {
				applyQualities(val);
			}
		}
	}
	if (!pendingKey.empty()) {
		warn("auto-identify: unterminated 'qualities' array; keeping the default.");
	}
}

auto QualityListText(uint32_t mask) -> std::string {
	static constexpr std::array<const char*, kQualityMax + 1> kNames {
		"", "inferior", "normal", "superior", "magic", "set", "rare", "unique", "crafted", "tempered",
	};
	std::string s;
	for (uint32_t id = 1; id <= kQualityMax; ++id) {
		if ((mask & (1U << id)) != 0) {
			if (!s.empty()) {
				s += ',';
			}
			s += kNames[id];
		}
	}
	return s.empty() ? "none" : s;
}

} // namespace

// ---------------------------------------------------------------------------
// Native functions.
// ---------------------------------------------------------------------------
using PickupItemFn      = bool(__fastcall*)(void* pPlayer, uint32_t nItemGUID, uint8_t bToCursor, uint32_t nMaxDistance, uint8_t bCheckCollision, uint8_t bToCube);
using IdentifyItemFn    = void(__fastcall*)(void* pGame, void* pPlayer, void* pItem, char bFlag);
using GetInventoryFn    = void*(__fastcall*)(void* pUnit, const char* szFile, int nLine);
using GetCursorItemFn   = void*(__fastcall*)(void* pInventory);
using GetFirstItemFn    = void*(__fastcall*)(void* pInventory);
using GetNextItemFn     = void*(__fastcall*)(void* pItem);
using GetUnitIdFn       = uint32_t(__fastcall*)(void* pUnit, const char* szFile, int nLine);
using GetGameFromUnitFn = void*(__fastcall*)(void* pUnit);
// Only RCX and RDX are used (D2RCore's GetItemName passes just those two);
// R8/R9 are passed through unchanged anyway.
using BuildItemNameFn   = void(__fastcall*)(void* pItem, char* szName, void* a3, void* a4);

// Set right after commit; the hook can already run in between, so it waits for it.
static std::atomic<PickupItemFn> g_originalPickupItem { nullptr };
static IdentifyItemFn            g_identifyItem    = nullptr;
static GetInventoryFn            g_getInventory    = nullptr;
static GetCursorItemFn           g_getCursorItem   = nullptr;
static GetFirstItemFn            g_getFirstItem    = nullptr;
static GetNextItemFn             g_getNextItem     = nullptr;
static GetUnitIdFn               g_getUnitId       = nullptr;
static GetGameFromUnitFn         g_getGameFromUnit = nullptr;
static bool                      g_hookActive      = false;
static std::atomic<uint32_t>     g_identifiedCount { 0 };
static std::atomic<BuildItemNameFn> g_originalBuildItemName { nullptr };
static bool                         g_nameHookActive = false;

static auto ReadU32(const void* base, size_t offset) -> uint32_t {
	uint32_t value = 0;
	std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
	return value;
}

static auto ReadPtr(const void* base, size_t offset) -> void* {
	void* value = nullptr;
	std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
	return value;
}

static auto IsItemWithGuid(void* pUnit, uint32_t nItemGUID) -> bool {
	return pUnit != nullptr && ReadU32(pUnit, Off::UnitType) == kUnitTypeItem && g_getUnitId(pUnit, __FILE__, __LINE__) == nItemGUID;
}

// The picked-up item is on the cursor or in the player's inventory list
// (inventory, cube, stash, belt). Returns nullptr if the pickup consumed it.
static auto FindPlayerItem(void* pPlayer, uint32_t nItemGUID) -> void* {
	void* inventory = g_getInventory(pPlayer, __FILE__, __LINE__);
	if (inventory == nullptr) {
		return nullptr;
	}
	if (void* cursor = g_getCursorItem(inventory); IsItemWithGuid(cursor, nItemGUID)) {
		return cursor;
	}
	int guard = 0;
	for (void* item = g_getFirstItem(inventory); item != nullptr && guard < kMaxInventoryItems; item = g_getNextItem(item), ++guard) {
		if (IsItemWithGuid(item, nItemGUID)) {
			return item;
		}
	}
	return nullptr;
}

static void IdentifyPickedUpItem(void* pPlayer, uint32_t nItemGUID) {
	void* item = FindPlayerItem(pPlayer, nItemGUID);
	if (item == nullptr) {
		return;
	}
	const void* itemData = ReadPtr(item, Off::UnitData);
	if (itemData == nullptr || (ReadU32(itemData, Off::ItemDataFlags) & kItemFlagIdentified) != 0) {
		return;
	}
	const uint32_t quality = ReadU32(itemData, Off::ItemDataQuality);
	if (quality > kQualityMax || (g_config.qualityMask & (1U << quality)) == 0) {
		return;
	}
	void* game = g_getGameFromUnit(pPlayer);
	if (game == nullptr) {
		return;
	}
	g_identifyItem(game, pPlayer, item, 0);
	g_identifiedCount.fetch_add(1, std::memory_order_relaxed);
}

static auto __fastcall HookPickupItem(void* pPlayer, uint32_t nItemGUID, uint8_t bToCursor, uint32_t nMaxDistance, uint8_t bCheckCollision, uint8_t bToCube) -> bool {
	PickupItemFn callOriginal = g_originalPickupItem.load(std::memory_order_acquire);
	for (int spin = 0; callOriginal == nullptr && spin < 1000; ++spin) {
		std::this_thread::yield();
		callOriginal = g_originalPickupItem.load(std::memory_order_acquire);
	}
	if (callOriginal == nullptr) {
		return false; // the game's own "not picked up" result
	}
	const bool pickedUp = callOriginal(pPlayer, nItemGUID, bToCursor, nMaxDistance, bCheckCollision, bToCube);
	if (pickedUp && g_config.enabled && g_config.qualityMask != 0 && pPlayer != nullptr && ReadU32(pPlayer, Off::UnitType) == kUnitTypePlayer) {
		IdentifyPickedUpItem(pPlayer, nItemGUID);
	}
	return pickedUp;
}

// The item's flags word when its name should be cut to the base name, else nullptr.
static auto HiddenNameFlags(void* pItem) -> uint32_t* {
	if (ReadU32(pItem, Off::UnitType) != kUnitTypeItem) {
		return nullptr;
	}
	auto* itemData = static_cast<uint8_t*>(ReadPtr(pItem, Off::UnitData));
	if (itemData == nullptr) {
		return nullptr;
	}
	const uint32_t quality = ReadU32(itemData, Off::ItemDataQuality);
	if (quality > kQualityMax || (g_config.hideNameMask & (1U << quality)) == 0) {
		return nullptr;
	}
	auto* flags = reinterpret_cast<uint32_t*>(itemData + Off::ItemDataFlags);
	return (*flags & kItemFlagIdentified) != 0 ? flags : nullptr;
}

static void __fastcall HookBuildItemName(void* pItem, char* szName, void* a3, void* a4) {
	BuildItemNameFn callOriginal = g_originalBuildItemName.load(std::memory_order_acquire);
	for (int spin = 0; callOriginal == nullptr && spin < 1000; ++spin) {
		std::this_thread::yield();
		callOriginal = g_originalBuildItemName.load(std::memory_order_acquire);
	}
	if (callOriginal == nullptr) {
		if (szName != nullptr) {
			szName[0] = '\0';
		}
		return;
	}
	uint32_t* flags = HiddenNameFlags(pItem);
	if (flags == nullptr) {
		callOriginal(pItem, szName, a3, a4);
		return;
	}
	// The builder's unidentified branch writes only the base name. This is the
	// client's copy of the item; the flag is set again before the hook returns.
	*flags &= ~kItemFlagIdentified;
	callOriginal(pItem, szName, a3, a4);
	*flags |= kItemFlagIdentified;
}

// ---------------------------------------------------------------------------
// Hook installation.
// ---------------------------------------------------------------------------
static auto Expect(const D2RL::PluginContext* ctx, uint64_t rva, const uint8_t* bytes, uint32_t size, const char* what) -> bool {
	if (ctx->CheckExpectedBytes(rva, bytes, size)) {
		return true;
	}
	char msg[160] {};
	std::snprintf(msg, sizeof(msg), "auto-identify: unexpected bytes at RVA 0x%llx (%s); hook not installed.", static_cast<unsigned long long>(rva), what);
	ctx->LogError(msg);
	return false;
}

static auto InstallPickupHook(const D2RL::PluginContext* ctx) -> bool {
	const bool bytesOk = Expect(ctx, Rva::PickupItem, kPickupExpected, sizeof(kPickupExpected), "maybe_D2GAME_ITEMS_PickupItem")
	                  && Expect(ctx, Rva::IdentifyItemCheck, kIdentifyCheckExpected, sizeof(kIdentifyCheckExpected), "D2GAME_ITEMS_Identify")
	                  && Expect(ctx, Rva::GetInventory, kGetInventoryExpected, sizeof(kGetInventoryExpected), "maybe_UNITS_GetInventory")
	                  && Expect(ctx, Rva::GetCursorItem, kGetCursorItemExpected, sizeof(kGetCursorItemExpected), "INVENTORY_GetCursorItem")
	                  && Expect(ctx, Rva::GetFirstItem, kGetFirstItemExpected, sizeof(kGetFirstItemExpected), "INVENTORY_GetFirstItem")
	                  && Expect(ctx, Rva::GetNextItem, kGetNextItemExpected, sizeof(kGetNextItemExpected), "INVENTORY_GetNextItem")
	                  && Expect(ctx, Rva::GetUnitId, kGetUnitIdExpected, sizeof(kGetUnitIdExpected), "maybe_UNITS_GetUnitId")
	                  && Expect(ctx, Rva::GetGameFromUnit, kGetGameExpected, sizeof(kGetGameExpected), "SUNIT_GetGameFromUnit");
	if (!bytesOk) {
		return false;
	}

	const D2RL::MutationService* mut = nullptr;
	if (ctx->QueryService(&mut) != D2RL::ServiceQueryResult::Success || !D2RL::HasMutationServiceField(mut, D2RL::MutationServiceRequiredSize)) {
		ctx->LogError("auto-identify: MutationService unavailable; cannot install the hook.");
		return false;
	}

	D2RL::Mutations::TransactionHandle tx = D2RL::Mutations::InvalidTransactionHandle;
	if (mut->beginTransaction(ctx, &tx) != D2RL::Mutations::Result::Success) {
		ctx->LogError("auto-identify: beginTransaction failed.");
		return false;
	}

	const D2RL::Mutations::InlineHookRequest hook {
		.structSize   = D2RL::Mutations::InlineHookRequestSize,
		.rva          = Rva::PickupItem,
		.expected     = kPickupExpected,
		.expectedSize = static_cast<uint32_t>(sizeof(kPickupExpected)),
		.target       = reinterpret_cast<void*>(&HookPickupItem),
	};
	D2RL::Mutations::OperationHandle op = D2RL::Mutations::InvalidOperationHandle;
	if (mut->stageInlineHook(ctx, tx, &hook, &op) != D2RL::Mutations::Result::Success) {
		mut->cancelTransaction(ctx, tx);
		ctx->LogError("auto-identify: staging the pickup hook failed.");
		return false;
	}

	// The hook reads these as soon as the commit lands.
	g_identifyItem    = reinterpret_cast<IdentifyItemFn>(ctx->exeBase + Rva::IdentifyItem);
	g_getInventory    = reinterpret_cast<GetInventoryFn>(ctx->exeBase + Rva::GetInventory);
	g_getCursorItem   = reinterpret_cast<GetCursorItemFn>(ctx->exeBase + Rva::GetCursorItem);
	g_getFirstItem    = reinterpret_cast<GetFirstItemFn>(ctx->exeBase + Rva::GetFirstItem);
	g_getNextItem     = reinterpret_cast<GetNextItemFn>(ctx->exeBase + Rva::GetNextItem);
	g_getUnitId       = reinterpret_cast<GetUnitIdFn>(ctx->exeBase + Rva::GetUnitId);
	g_getGameFromUnit = reinterpret_cast<GetGameFromUnitFn>(ctx->exeBase + Rva::GetGameFromUnit);

	D2RL::Mutations::CommitResult result { .structSize = D2RL::Mutations::CommitResultSize };
	if (mut->commit(ctx, tx, &result) != D2RL::Mutations::Result::Success || result.result != D2RL::Mutations::Result::Success) {
		char msg[160] {};
		std::snprintf(msg, sizeof(msg), "auto-identify: commit failed (result %u, reason %u); original bytes restored.", static_cast<unsigned>(result.result),
		              static_cast<unsigned>(result.failureReason));
		ctx->LogError(msg);
		return false;
	}

	void* original = nullptr;
	if (mut->getInlineHookOriginal(ctx, tx, op, &original) != D2RL::Mutations::Result::Success || original == nullptr) {
		// Cannot happen after a successful commit; without the original the hook
		// must not run, and it cannot be removed here, so fail the plugin load.
		ctx->LogError("auto-identify: no original pointer for the pickup routine.");
		return false;
	}
	g_originalPickupItem.store(reinterpret_cast<PickupItemFn>(original), std::memory_order_release);
	g_hookActive = true;
	ctx->LogInfo("auto-identify: pickup hook committed.");
	return true;
}

static auto InstallNameHook(const D2RL::PluginContext* ctx) -> bool {
	const bool bytesOk = Expect(ctx, Rva::BuildItemName, kBuildNameEntryExpected, sizeof(kBuildNameEntryExpected), "maybe_D2CLIENT_ITEMS_BuildName entry")
	                  && Expect(ctx, Rva::BuildItemNameHook, kBuildNameHookExpected, sizeof(kBuildNameHookExpected), "maybe_D2CLIENT_ITEMS_BuildName");
	if (!bytesOk) {
		return false;
	}

	const D2RL::MutationService* mut = nullptr;
	if (ctx->QueryService(&mut) != D2RL::ServiceQueryResult::Success || !D2RL::HasMutationServiceField(mut, D2RL::MutationServiceRequiredSize)) {
		ctx->LogError("auto-identify: MutationService unavailable; cannot install the item-name hook.");
		return false;
	}

	D2RL::Mutations::TransactionHandle tx = D2RL::Mutations::InvalidTransactionHandle;
	if (mut->beginTransaction(ctx, &tx) != D2RL::Mutations::Result::Success) {
		ctx->LogError("auto-identify: beginTransaction failed (item-name hook).");
		return false;
	}

	const D2RL::Mutations::InlineHookRequest hook {
		.structSize   = D2RL::Mutations::InlineHookRequestSize,
		.rva          = Rva::BuildItemNameHook,
		.expected     = kBuildNameHookExpected,
		.expectedSize = static_cast<uint32_t>(sizeof(kBuildNameHookExpected)),
		.target       = reinterpret_cast<void*>(&HookBuildItemName),
	};
	D2RL::Mutations::OperationHandle op = D2RL::Mutations::InvalidOperationHandle;
	if (mut->stageInlineHook(ctx, tx, &hook, &op) != D2RL::Mutations::Result::Success) {
		mut->cancelTransaction(ctx, tx);
		ctx->LogError("auto-identify: staging the item-name hook failed.");
		return false;
	}

	D2RL::Mutations::CommitResult result { .structSize = D2RL::Mutations::CommitResultSize };
	if (mut->commit(ctx, tx, &result) != D2RL::Mutations::Result::Success || result.result != D2RL::Mutations::Result::Success) {
		char msg[160] {};
		std::snprintf(msg, sizeof(msg), "auto-identify: item-name hook commit failed (result %u, reason %u); original bytes restored.",
		              static_cast<unsigned>(result.result), static_cast<unsigned>(result.failureReason));
		ctx->LogError(msg);
		return false;
	}

	void* original = nullptr;
	if (mut->getInlineHookOriginal(ctx, tx, op, &original) != D2RL::Mutations::Result::Success || original == nullptr) {
		ctx->LogError("auto-identify: no original pointer for the item-name builder.");
		return false;
	}
	g_originalBuildItemName.store(reinterpret_cast<BuildItemNameFn>(original), std::memory_order_release);
	g_nameHookActive = true;
	ctx->LogInfo("auto-identify: item-name hook committed.");
	return true;
}

// ---------------------------------------------------------------------------
static auto StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* cmd, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (cmd == nullptr || cmd->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	char msg[320] {};
	std::snprintf(msg, sizeof(msg), "auto-identify: enabled=%d hook=%d qualities=%s identified=%u name_hook=%d hide_names=%s", g_config.enabled,
	              g_hookActive, QualityListText(g_config.qualityMask).c_str(), g_identifiedCount.load(std::memory_order_relaxed), g_nameHookActive,
	              QualityListText(g_config.hideNameMask).c_str());
	cmd->plugin->WriteConsoleMessage(msg);
	return D2RL::ConsoleCommandResult::Handled;
}

// ---------------------------------------------------------------------------
static constexpr D2RL::PluginInfo kInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "auto-identify",
	.name        = "Auto Identify",
	.version     = "0.2.0",
	.author      = "Kain",
	.description = "Identifies items of the configured qualities when a player picks them up; can hide magic and rare item names.",
	// Shared: the pickup rule runs on the host, the name display on each client.
	.flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &kInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* ctx) noexcept -> bool {
	if (ctx == nullptr) {
		return false;
	}
	ctx->LogInfo("auto-identify loading.");

	(void)ctx->EnsureConfig();
	std::array<char, 8192> buf {};
	uint32_t required = 0;
	if (ctx->ReadConfig(buf.data(), static_cast<uint32_t>(buf.size()), &required)) {
		ParseConfig(buf.data(), g_config, [ctx](const char* msg) { ctx->LogWarn(msg); });
	} else {
		ctx->LogWarn("auto-identify: could not read config; using defaults.");
	}
	{
		char msg[192] {};
		std::snprintf(msg, sizeof(msg), "auto-identify: enabled=%d qualities=%s hide_names=%s", g_config.enabled, QualityListText(g_config.qualityMask).c_str(),
		              QualityListText(g_config.hideNameMask).c_str());
		ctx->LogInfo(msg);
	}

	(void)ctx->RegisterConsoleCommand("auto-identify", StatusCommand, "Show auto-identify status.");

	if (!g_config.enabled) {
		ctx->LogInfo("auto-identify: disabled in config; hooks not installed.");
		return true;
	}
	// Each feature hooks only when it is used. A failed hook fails the load, so
	// D2RLoader removes anything already installed.
	if (g_config.qualityMask != 0 && !InstallPickupHook(ctx)) {
		return false;
	}
	if (g_config.hideNameMask != 0 && !InstallNameHook(ctx)) {
		return false;
	}

	ctx->LogInfo("auto-identify loaded.");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
}
