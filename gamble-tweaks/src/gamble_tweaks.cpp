// gamble-tweaks - D2RLoader plugin
//
// Tier-1: plugin-owned "gamble" table + embedded config, rebuilt on every
// data-tables load.
//
// Feature A: stop forcing a ring and an amulet into the gamble window; the window
//            then shows only what gamble.txt selects.
//            (The post-purchase refill is left as the game has it: a bought
//            ring/amulet is replaced by a new ring/amulet, i.e. its own family.
//            Removing that special case is unsafe - rings/amulets have no
//            norm/uber/ultra codes, so the family search would come back empty.)
// Feature B: per-base constant gamble price from the plugin table (normal-tier code),
//            or the base's own "gamble cost" column, as the game prices rings/amulets.
// Feature C: gamble cost reduced by a configurable ItemStatCost stat (percent,
//            capped), optionally without the vendor "reduced prices" stat.
// Feature D: every bought gamble item gets a "reduced value" stat (percent by
//            quality) in its saved magic stat list; its sell price drops by that
//            percent after the game's calculation. Read from the item only.
//
// All RVAs are relative to the D2RLoader.exe image (ctx->exeBase) and were read
// from /d2rloader_full.dmp in Ghidra; evidence is in notes/gamble.md of the
// workspace. Every write is guarded by the expected original bytes.

#include <D2RLPlugin/api.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <intrin.h>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// RE-supplied constants (D2RLoader.exe RVAs).
// ---------------------------------------------------------------------------
namespace Rva {
// D2GAME_STORES_FillGamble: CMP EBP,2 - number of forced ring/amulet slots.
inline constexpr uint64_t FillGambleForcedSlots = 0x541a25;
// ITEMS_GetTransactionCost gamble formula path: JZ that skips the reduced-prices
// percentage when it is zero (only reached from the gamble branch).
inline constexpr uint64_t GambleVendorReduction = 0x36f268;
// ITEMS_GetTransactionCost entry (inline hook).
inline constexpr uint64_t GetTransactionCost = 0x36f0c0;
// DATATBLS_GetItemsTxtRecord(uchar nMode, int nItemId).
inline constexpr uint64_t GetItemsTxtRecord = 0x314110;
// DATATBLS_GetItemIdFromItemCode(uchar nMode, uint dwCode).
inline constexpr uint64_t GetItemIdFromItemCode = 0x313ef0;
// ReadWideUnitStat(unit, statId, layer) - thunk into D2RCore.
inline constexpr uint64_t ReadUnitStat = 0x2f5020;
// D2GAME_ITEMS_Identify(pGame, pPlayer, pItem, bFlag) entry (inline hook, feature D).
inline constexpr uint64_t IdentifyItem = 0x46e8c0;
// maybe_D2GAME_NPC_BuyItem: CALL D2GAME_ITEMS_Identify, reached only when bGamble
// (CMP byte [RBP+0xE0],0 / JZ at 0x53e696). The hook acts when it returns to 0x53e6b2.
inline constexpr uint64_t GambleIdentifyCall   = 0x53e6ad;
inline constexpr uint64_t GambleIdentifyReturn = 0x53e6b2;
// Item magic stat list, as the item deserializer (FUN_1403780d0) builds it:
// STATLIST_GetStatListFromUnitStateOrFlag(pUnit, nState, nFlag).
inline constexpr uint64_t GetStatListFromUnitStateOrFlag = 0x2f5b80;
// STATLIST_AllocStatList(nFlags, pMemPool, nUnitType, nUnitId).
inline constexpr uint64_t AllocStatList = 0x2f7300;
// AttachWideStatList(pUnit, pStatList, 1) (D2COMMON_10475_PostStatToStatList) - thunk into D2RCore.
inline constexpr uint64_t AttachStatList = 0x2f2cb0;
// SetWideListStat(nMode, pStatList, nStatId, nValue, nLayer) - thunk into D2RCore.
inline constexpr uint64_t SetListStat = 0x2f7c00;
// maybe_UNITS_GetUnitId(pUnit, szFile, nLine).
inline constexpr uint64_t GetUnitId = 0x34a330;
} // namespace Rva

// CMP EBP,2 -> CMP EBP,0 (no forced slots).
static constexpr uint8_t kForcedSlotsExpected[] = { 0x83, 0xFD, 0x02 };
static constexpr uint8_t kForcedSlotsPatched[]  = { 0x83, 0xFD, 0x00 };
// JZ 0x36f320 -> JMP 0x36f320; NOP (always skip the reduced-prices percentage).
static constexpr uint8_t kVendorReductionExpected[] = { 0x0F, 0x84, 0xB2, 0x00, 0x00, 0x00 };
static constexpr uint8_t kVendorReductionPatched[]  = { 0xE9, 0xB3, 0x00, 0x00, 0x00, 0x90 };
// ITEMS_GetTransactionCost prologue: three whole MOVs (5,5,5), no RIP-relative operand.
static constexpr uint8_t kTransactionCostExpected[] = { 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x44, 0x89, 0x44,
	                                                    0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10 };
// Functions the hook calls; checked before use.
static constexpr uint8_t kItemsTxtRecordExpected[] = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x8B, 0xFA };
static constexpr uint8_t kItemIdFromCodeExpected[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xDA };
static constexpr uint8_t kReadUnitStatExpected[]   = { 0xFF, 0x25, 0xF2, 0x51, 0xB3, 0x03 };
// D2GAME_ITEMS_Identify prologue: MOV [RSP+0x20],RBP; PUSH R12; PUSH R14; PUSH R15;
// SUB RSP,0x50 (5,2,2,2,4), no RIP-relative operand.
static constexpr uint8_t kIdentifyExpected[] = { 0x48, 0x89, 0x6C, 0x24, 0x20, 0x41, 0x54, 0x41,
	                                             0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50 };
// CALL D2GAME_ITEMS_Identify at the gamble-buy site (checked so the return address is right).
static constexpr uint8_t kGambleIdentifyCallExpected[] = { 0xE8, 0x0E, 0x02, 0xF3, 0xFF };
static constexpr uint8_t kGetStatListExpected[]   = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };
static constexpr uint8_t kAllocStatListExpected[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
	                                                  0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20 };
static constexpr uint8_t kAttachStatListExpected[] = { 0xFF, 0x25, 0xBA, 0x75, 0xB3, 0x03 };
static constexpr uint8_t kSetListStatExpected[]    = { 0xFF, 0x25, 0x2A, 0x26, 0xB3, 0x03 };
static constexpr uint8_t kGetUnitIdExpected[]      = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D };

// D2UnitStrc / D2ItemsTxt offsets used by the hook.
namespace Off {
inline constexpr size_t UnitClassId      = 0x04;  // maybe_UNITS_GetClassId
inline constexpr size_t UnitData         = 0x10;  // maybe_UNITS_GetUnitData (pItemData)
inline constexpr size_t ItemDataQuality  = 0x00;  // dwQualityNo (GetTransactionCost switch 4..9)
inline constexpr size_t UnitDataTableSet = 0x1BD; // maybe_UNITS_GetDataTableSet (byte)
inline constexpr size_t ItemsTxtCode     = 0x80;  // dwCode
inline constexpr size_t ItemsTxtNormCode = 0x84;  // dwNormCode
inline constexpr size_t ItemsTxtGambleCost = 0xDC; // dwGambleCost ("gamble cost" column)
} // namespace Off

inline constexpr int      kTransactionSell       = 1;  // TRANSACTIONTYPE_SELL
inline constexpr int      kTransactionGamble     = 2;  // TRANSACTIONTYPE_GAMBLE
inline constexpr uint32_t kItemStatListFlag      = 0x40; // item magic stat list (serialized)
inline constexpr uint32_t kUnitTypeItem          = 4;
inline constexpr uint32_t kQualityMagic          = 4;
inline constexpr uint32_t kQualitySet            = 5;
inline constexpr uint32_t kQualityRare           = 6;
inline constexpr uint32_t kQualityUnique         = 7;
inline constexpr uint32_t kStatReducedPrices      = 87; // item_reducedprices
inline constexpr int32_t  kVendorReductionMaxPct  = 99; // same clamp as the game

// ---------------------------------------------------------------------------
// Config.
// ---------------------------------------------------------------------------
struct Config {
	uint32_t gambleCostReductionStatId  = 0;    // ItemStatCost.txt row id; 0 = disabled
	bool     gambleCostReductionPercent = true; // percent (else flat gold)
	uint32_t gambleCostReductionCap     = 50;   // max percent removed by the stat
	bool     ignoreVendorReduction      = true; // vendor reduced-prices stat not applied to gambling
	bool     windowStrictGambleTxt      = true; // feature A
	bool     useConstantGamblePrice     = true; // feature B
	bool     useItemGambleCost          = true; // feature B: "gamble cost" column for every base
	// Feature D: "reduced value" stat put on gambled items; percent off their sell price.
	uint32_t reducedValueStatId         = 0;    // ItemStatCost.txt row id; 0 = disabled
	uint32_t reducedValueMagic          = 25;
	uint32_t reducedValueRare           = 33;
	uint32_t reducedValueSet            = 33;
	uint32_t reducedValueUnique         = 50;
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

// Minimal reader for the scalar keys of the [gamble-tweaks] table.
void ParseConfig(const char* toml, Config& out) {
	if (toml == nullptr) {
		return;
	}
	std::string text(toml);
	std::string section;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t nl = text.find('\n', pos);
		std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
		pos = (nl == std::string::npos) ? text.size() : nl + 1;

		size_t hash = line.find('#');
		if (hash != std::string::npos) {
			line = line.substr(0, hash);
		}
		line = trim(line);
		if (line.empty()) {
			continue;
		}
		if (line.front() == '[' && line.back() == ']') {
			section = trim(line.substr(1, line.size() - 2));
			continue;
		}
		if (section != "gamble-tweaks") {
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		std::string key = trim(line.substr(0, eq));
		std::string val = trim(line.substr(eq + 1));
		if (key == "gamble_cost_reduction_stat_id") {
			out.gambleCostReductionStatId = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
		} else if (key == "gamble_cost_reduction_is_percent") {
			out.gambleCostReductionPercent = parseBool(val, out.gambleCostReductionPercent);
		} else if (key == "gamble_cost_reduction_cap_percent") {
			out.gambleCostReductionCap = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
		} else if (key == "ignore_vendor_reduction_for_gamble") {
			out.ignoreVendorReduction = parseBool(val, out.ignoreVendorReduction);
		} else if (key == "gamble_window_strict_gambletxt") {
			out.windowStrictGambleTxt = parseBool(val, out.windowStrictGambleTxt);
		} else if (key == "use_constant_gamble_price") {
			out.useConstantGamblePrice = parseBool(val, out.useConstantGamblePrice);
		} else if (key == "use_item_gamble_cost") {
			out.useItemGambleCost = parseBool(val, out.useItemGambleCost);
		} else if (key == "reduced_value_stat_id") {
			out.reducedValueStatId = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
		} else if (key == "reduced_value_magic") {
			out.reducedValueMagic = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
		} else if (key == "reduced_value_rare") {
			out.reducedValueRare = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
		} else if (key == "reduced_value_set") {
			out.reducedValueSet = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
		} else if (key == "reduced_value_unique") {
			out.reducedValueUnique = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
		}
	}
	if (out.gambleCostReductionCap > 100) {
		out.gambleCostReductionCap = 100;
	}
	for (uint32_t* pct : { &out.reducedValueMagic, &out.reducedValueRare, &out.reducedValueSet, &out.reducedValueUnique }) {
		if (*pct > 100) {
			*pct = 100;
		}
	}
}

} // namespace

// ---------------------------------------------------------------------------
// Plugin-owned "gamble" table.
//   code    Ascii(8)  NORMAL-tier base code (e.g. "cap", not "xap"), matched like
//                     the game's own gamble price lookup
//   gamble  Dword     reserved (1 = listed); gamble.txt still decides the pool
//   price   Dword     constant gamble price; 0 = use the game formula (feature B)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct GambleRow {
	char     code[8];
	uint32_t gamble;
	uint32_t price;
};
#pragma pack(pop)
static_assert(sizeof(GambleRow) == 16, "GambleRow must stay tab-table-compatible");

static constexpr std::array<D2RL::CustomTables::ColumnDefinition, 3> kGambleColumns {
	D2RL::CustomTables::ColumnDefinition {
		.structSize = D2RL::CustomTables::ColumnDefinitionSize,
		.name       = "code",
		.type       = D2RL::CustomTables::ColumnType::Ascii,
		.offset     = static_cast<uint32_t>(offsetof(GambleRow, code)),
		.length     = sizeof(GambleRow::code),
	},
	D2RL::CustomTables::ColumnDefinition {
		.structSize = D2RL::CustomTables::ColumnDefinitionSize,
		.name       = "gamble",
		.type       = D2RL::CustomTables::ColumnType::Dword,
		.offset     = static_cast<uint32_t>(offsetof(GambleRow, gamble)),
	},
	D2RL::CustomTables::ColumnDefinition {
		.structSize = D2RL::CustomTables::ColumnDefinitionSize,
		.name       = "price",
		.type       = D2RL::CustomTables::ColumnType::Dword,
		.offset     = static_cast<uint32_t>(offsetof(GambleRow, price)),
	},
};

// Default table shipped from memory. A mod can override it at the same virtual path.
static constexpr char kGambleTablePath[] = "data/global/excel/d2rloader/gamble-tweaks/gamble.txt";
static constexpr char kGambleTableText[] = "code\tgamble\tprice\nrin\t1\t0\namu\t1\t0\n";

struct GambleEntry {
	uint32_t code; // space-padded FourCC, same byte order as ItemsTxt codes
	uint32_t gamble;
	uint32_t price;
};

// Rebuilt on DataTablesLoaded, read by the price hook (game and UI threads).
static std::vector<GambleEntry> g_entries;
static std::shared_mutex        g_entriesLock;

static const D2RL::CustomTableService* g_customTables = nullptr;
static D2RL::CustomTables::TableHandle g_gambleTable  = D2RL::CustomTables::InvalidHandle;

static auto CodeFourCC(const char* s) -> uint32_t {
	char c[4] = { ' ', ' ', ' ', ' ' };
	for (int i = 0; i < 4 && s[i] != '\0'; ++i) {
		c[i] = s[i];
	}
	return D2RL::Items::MakeItemCode(c[0], c[1], c[2], c[3]);
}

// Returns the constant price for a normal-tier code, 0 if none.
static auto FindConstantPrice(uint32_t code) -> uint32_t {
	std::shared_lock lock(g_entriesLock);
	for (const auto& e : g_entries) {
		if (e.code == code) {
			return e.price;
		}
	}
	return 0;
}

static void RebuildSnapshot(const D2RL::PluginContext* ctx) {
	std::vector<GambleEntry> entries;
	if (g_customTables == nullptr || g_gambleTable == D2RL::CustomTables::InvalidHandle) {
		return;
	}
	D2RL::CustomTables::TableInfo info { .structSize = D2RL::CustomTables::TableInfoSize };
	if (g_customTables->getTableInfo(ctx, g_gambleTable, D2RL::CustomTables::TableBank::Rotw, &info) != D2RL::CustomTables::Result::Success) {
		return;
	}
	if (info.state != D2RL::CustomTables::TableState::Ready || info.rowSize != sizeof(GambleRow)) {
		ctx->LogWarn("gamble-tweaks: gamble table not ready or row size mismatch.");
		return;
	}
	std::vector<GambleRow> rows(info.rowCount);
	if (info.rowCount != 0) {
		if (g_customTables->copyRows(ctx, g_gambleTable, D2RL::CustomTables::TableBank::Rotw, info.revision, rows.data(), info.byteCount) != D2RL::CustomTables::Result::Success) {
			ctx->LogWarn("gamble-tweaks: copyRows failed (stale revision?).");
			return;
		}
	}
	entries.reserve(rows.size());
	for (const auto& r : rows) {
		char code[9] = {};
		std::memcpy(code, r.code, sizeof(r.code));
		entries.push_back(GambleEntry { CodeFourCC(code), r.gamble, r.price });
	}
	{
		std::unique_lock lock(g_entriesLock);
		g_entries.swap(entries);
	}
	char msg[96] {};
	std::snprintf(msg, sizeof(msg), "gamble-tweaks: %u gamble rows loaded.", info.rowCount);
	ctx->LogInfo(msg);
}

static void __cdecl OnDataTablesLoaded(const D2RL::PluginContext* ctx, const D2RL::Lifecycle::DataTablesLoadedEvent* ev, void*) noexcept {
	if (ctx == nullptr || !D2RL::Lifecycle::HasDataTablesLoadedEventField(ev, D2RL::Lifecycle::DataTablesLoadedEventRequiredSize)) {
		return;
	}
	RebuildSnapshot(ctx);
}

// ---------------------------------------------------------------------------
// Feature B/C: hook on ITEMS_GetTransactionCost.
// int ITEMS_GetTransactionCost(D2UnitStrc* pPlayer, D2UnitStrc* pItem, int nDifficulty,
//                              void* pQuestFlags, int nVendorId, int nTransactionType)
// Both the client (tooltip, buy packet cost) and the server (cost check) call it,
// so the result has to be deterministic.
// ---------------------------------------------------------------------------
using GetTransactionCostFn = int(__fastcall*)(void* pPlayer, void* pItem, int nDifficulty, void* pQuestFlags, int nVendorId, int nTransactionType);
using GetItemsTxtRecordFn  = uint8_t*(__fastcall*)(uint8_t nMode, int nItemId);
using GetItemIdFromCodeFn  = int(__fastcall*)(uint8_t nMode, uint32_t dwCode);
using ReadUnitStatFn       = int(__fastcall*)(void* pUnit, uint32_t nStatId, uint32_t nLayer);

// Set right after commit; the hook can already run in between, so it waits for it.
static std::atomic<GetTransactionCostFn> g_originalGetTransactionCost { nullptr };
static GetItemsTxtRecordFn  g_getItemsTxtRecord           = nullptr;
static GetItemIdFromCodeFn  g_getItemIdFromCode           = nullptr;
static ReadUnitStatFn       g_readUnitStat                = nullptr;
static bool                 g_vendorReductionPatched      = false;
static bool                 g_priceHookActive             = false;
static bool                 g_windowPatched               = false;

// Normal-tier code of an item, the same record the game prices gambles from.
static auto ItemNormalCode(void* pItem) -> uint32_t {
	if (pItem == nullptr || g_getItemsTxtRecord == nullptr) {
		return 0;
	}
	const auto* unit    = static_cast<const uint8_t*>(pItem);
	const uint8_t nMode = unit[Off::UnitDataTableSet];
	int32_t classId     = 0;
	std::memcpy(&classId, unit + Off::UnitClassId, sizeof(classId));
	const uint8_t* record = g_getItemsTxtRecord(nMode, classId);
	if (record == nullptr) {
		return 0;
	}
	uint32_t normCode = 0;
	uint32_t code     = 0;
	std::memcpy(&normCode, record + Off::ItemsTxtNormCode, sizeof(normCode));
	std::memcpy(&code, record + Off::ItemsTxtCode, sizeof(code));
	return normCode != 0 ? normCode : code;
}

// "gamble cost" of the normal-tier record - what the game charges for rings and
// amulets (code -> DATATBLS_GetItemIdFromItemCode -> DATATBLS_GetItemsTxtRecord,
// +0xDC). 0 if the base has none.
static auto ItemNormalGambleCost(void* pItem, uint32_t normCode) -> uint32_t {
	if (normCode == 0 || g_getItemIdFromCode == nullptr || g_getItemsTxtRecord == nullptr) {
		return 0;
	}
	const uint8_t nMode = static_cast<const uint8_t*>(pItem)[Off::UnitDataTableSet];
	const int itemId    = g_getItemIdFromCode(nMode, normCode);
	if (itemId < 0) {
		return 0;
	}
	const uint8_t* record = g_getItemsTxtRecord(nMode, itemId);
	if (record == nullptr) {
		return 0;
	}
	uint32_t cost = 0;
	std::memcpy(&cost, record + Off::ItemsTxtGambleCost, sizeof(cost));
	return cost;
}

static auto ApplyPercent(int64_t price, int64_t percent) -> int64_t {
	return price - (price * percent) / 100;
}

static auto __fastcall HookGetTransactionCost(void* pPlayer, void* pItem, int nDifficulty, void* pQuestFlags, int nVendorId, int nTransactionType) -> int {
	GetTransactionCostFn callOriginal = g_originalGetTransactionCost.load(std::memory_order_acquire);
	for (int spin = 0; callOriginal == nullptr && spin < 1000; ++spin) {
		std::this_thread::yield();
		callOriginal = g_originalGetTransactionCost.load(std::memory_order_acquire);
	}
	if (callOriginal == nullptr) {
		return 0x7FFFFFFF; // the game's own "cannot price" value
	}
	const int original = callOriginal(pPlayer, pItem, nDifficulty, pQuestFlags, nVendorId, nTransactionType);

	// Feature D - sell price minus the item's own "reduced value" percent, after the
	// game's whole calculation. Read from the item only, never the player.
	if (nTransactionType == kTransactionSell) {
		if (g_config.reducedValueStatId == 0 || g_readUnitStat == nullptr || pItem == nullptr || original <= 1 || original == 0x7FFFFFFF) {
			return original;
		}
		int32_t pct = g_readUnitStat(pItem, g_config.reducedValueStatId, 0);
		if (pct <= 0) {
			return original;
		}
		pct                 = pct > 100 ? 100 : pct;
		const int64_t price = ApplyPercent(original, pct);
		return static_cast<int>(price < 1 ? 1 : price); // the game's own floor for a sale
	}

	if (nTransactionType != kTransactionGamble || pPlayer == nullptr || pItem == nullptr || original <= 0 || original == 0x7FFFFFFF) {
		return original;
	}

	int64_t price = original;

	// Feature B - constant per-base price keyed by the normal-tier code: the plugin
	// table's price wins, else the base's "gamble cost" column (priced like rings and
	// amulets), else the game's result stands.
	if (g_config.useConstantGamblePrice || g_config.useItemGambleCost) {
		const uint32_t normCode = ItemNormalCode(pItem);
		uint32_t constant       = g_config.useConstantGamblePrice ? FindConstantPrice(normCode) : 0;
		if (constant == 0 && g_config.useItemGambleCost) {
			constant = ItemNormalGambleCost(pItem, normCode);
		}
		if (constant != 0) {
			price = constant;
			// Rings and amulets get the reduced-prices percentage after the gamble cost;
			// keep that unless it is switched off for gambling.
			if (!g_config.ignoreVendorReduction && g_readUnitStat != nullptr) {
				int32_t pct = g_readUnitStat(pPlayer, kStatReducedPrices, 0);
				pct         = pct < 0 ? 0 : (pct > kVendorReductionMaxPct ? kVendorReductionMaxPct : pct);
				price       = ApplyPercent(price, pct);
			}
		}
	}

	// Feature C - reduction from the configured stat, capped.
	if (g_config.gambleCostReductionStatId != 0 && g_readUnitStat != nullptr) {
		const int32_t value = g_readUnitStat(pPlayer, g_config.gambleCostReductionStatId, 0);
		if (value > 0) {
			const int64_t cap = g_config.gambleCostReductionCap;
			if (g_config.gambleCostReductionPercent) {
				price = ApplyPercent(price, value > cap ? cap : value);
			} else {
				const int64_t maxOff = (price * cap) / 100;
				price -= value > maxOff ? maxOff : value;
			}
		}
	}

	if (price < 1) {
		price = 1;
	}
	if (price > 0x7FFFFFFE) {
		price = 0x7FFFFFFE;
	}
	return static_cast<int>(price);
}

// ---------------------------------------------------------------------------
// Feature D: hook on D2GAME_ITEMS_Identify. maybe_D2GAME_NPC_BuyItem calls it for a
// bought gamble item only; it then serializes the item to every client, so a stat
// written first reaches the client (which prices the sale itself) and the save.
// void D2GAME_ITEMS_Identify(void* pGame, D2UnitStrc* pPlayer, D2UnitStrc* pItem, char bFlag)
// ---------------------------------------------------------------------------
using IdentifyItemFn   = void(__fastcall*)(void* pGame, void* pPlayer, void* pItem, char bFlag);
using GetStatListFn    = void*(__fastcall*)(void* pUnit, uint32_t nState, uint32_t nFlag);
using AllocStatListFn  = void*(__fastcall*)(uint32_t nFlags, void* pMemPool, uint32_t nUnitType, uint32_t nUnitId);
using AttachStatListFn = void(__fastcall*)(void* pUnit, void* pStatList, int bAttach);
using SetListStatFn    = uint64_t(__fastcall*)(uint8_t nMode, void* pStatList, uint32_t nStatId, int32_t nValue, uint32_t nLayer);
using GetUnitIdFn      = uint32_t(__fastcall*)(void* pUnit, const char* szFile, int nLine);

static std::atomic<IdentifyItemFn> g_originalIdentifyItem { nullptr };
static uintptr_t                   g_gambleIdentifyReturn = 0;
static GetStatListFn               g_getStatList          = nullptr;
static AllocStatListFn             g_allocStatList        = nullptr;
static AttachStatListFn            g_attachStatList       = nullptr;
static SetListStatFn               g_setListStat          = nullptr;
static GetUnitIdFn                 g_getUnitId            = nullptr;
static bool                        g_reducedValueActive   = false;

static auto ReducedValueForQuality(uint32_t quality) -> uint32_t {
	switch (quality) {
	case kQualityMagic:
		return g_config.reducedValueMagic;
	case kQualityRare:
		return g_config.reducedValueRare;
	case kQualitySet:
		return g_config.reducedValueSet;
	case kQualityUnique:
		return g_config.reducedValueUnique;
	default:
		return 0;
	}
}

// Same steps as the item deserializer uses for its own saved stat: the item's
// state-0 magic list (flag 0x40), created and attached if missing, then set.
static void TagGambledItem(void* pItem) {
	auto* unit = static_cast<uint8_t*>(pItem);
	void* data = nullptr;
	std::memcpy(&data, unit + Off::UnitData, sizeof(data));
	if (data == nullptr) {
		return;
	}
	uint32_t quality = 0;
	std::memcpy(&quality, static_cast<const uint8_t*>(data) + Off::ItemDataQuality, sizeof(quality));
	const uint32_t value = ReducedValueForQuality(quality);
	if (value == 0) {
		return;
	}
	void* list = g_getStatList(pItem, 0, kItemStatListFlag);
	if (list == nullptr) {
		list = g_allocStatList(kItemStatListFlag, nullptr, kUnitTypeItem, g_getUnitId(pItem, __FILE__, __LINE__));
		if (list == nullptr) {
			return;
		}
		g_attachStatList(pItem, list, 1);
	}
	g_setListStat(unit[Off::UnitDataTableSet], list, g_config.reducedValueStatId, static_cast<int32_t>(value), 0);
}

static auto __fastcall HookIdentifyItem(void* pGame, void* pPlayer, void* pItem, char bFlag) -> void {
	IdentifyItemFn callOriginal = g_originalIdentifyItem.load(std::memory_order_acquire);
	for (int spin = 0; callOriginal == nullptr && spin < 1000; ++spin) {
		std::this_thread::yield();
		callOriginal = g_originalIdentifyItem.load(std::memory_order_acquire);
	}
	// The inline hook jumps here, so the return address is still BuyItem's call site.
	if (pItem != nullptr && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_gambleIdentifyReturn) {
		TagGambledItem(pItem);
	}
	if (callOriginal != nullptr) {
		callOriginal(pGame, pPlayer, pItem, bFlag);
	}
}

// ---------------------------------------------------------------------------
// Hook/patch installation. Each feature is checked against its expected bytes
// first; a mismatch disables that feature only.
// ---------------------------------------------------------------------------
static auto Expect(const D2RL::PluginContext* ctx, uint64_t rva, const uint8_t* bytes, uint32_t size, const char* what) -> bool {
	if (ctx->CheckExpectedBytes(rva, bytes, size)) {
		return true;
	}
	char msg[160] {};
	std::snprintf(msg, sizeof(msg), "gamble-tweaks: unexpected bytes at RVA 0x%llx (%s); feature skipped.", static_cast<unsigned long long>(rva), what);
	ctx->LogError(msg);
	return false;
}

static auto StagePatch(const D2RL::PluginContext* ctx, const D2RL::MutationService* mut, D2RL::Mutations::TransactionHandle tx, uint64_t rva, const uint8_t* expected,
                       const uint8_t* bytes, uint32_t size) -> bool {
	const D2RL::Mutations::BytePatchRequest patch {
		.structSize   = D2RL::Mutations::BytePatchRequestSize,
		.rva          = rva,
		.expected     = expected,
		.expectedSize = size,
		.bytes        = bytes,
		.size         = size,
	};
	D2RL::Mutations::OperationHandle op = D2RL::Mutations::InvalidOperationHandle;
	return mut->stageBytePatch(ctx, tx, &patch, &op) == D2RL::Mutations::Result::Success;
}

static auto InstallHooksAndPatches(const D2RL::PluginContext* ctx) -> bool {
	const bool wantWindow  = g_config.windowStrictGambleTxt;
	const bool wantVendor  = g_config.ignoreVendorReduction;
	const bool wantReduced = g_config.reducedValueStatId != 0;
	const bool wantPrice   = g_config.useConstantGamblePrice || g_config.useItemGambleCost || g_config.gambleCostReductionStatId != 0 || wantReduced;
	if (!wantWindow && !wantVendor && !wantPrice) {
		ctx->LogInfo("gamble-tweaks: all code features disabled in config.");
		return true;
	}

	const bool okWindow = wantWindow && Expect(ctx, Rva::FillGambleForcedSlots, kForcedSlotsExpected, sizeof(kForcedSlotsExpected), "FillGamble forced slots");
	const bool okVendor = wantVendor && Expect(ctx, Rva::GambleVendorReduction, kVendorReductionExpected, sizeof(kVendorReductionExpected), "gamble vendor reduction");
	bool okPrice        = wantPrice && Expect(ctx, Rva::GetTransactionCost, kTransactionCostExpected, sizeof(kTransactionCostExpected), "ITEMS_GetTransactionCost")
	              && Expect(ctx, Rva::GetItemsTxtRecord, kItemsTxtRecordExpected, sizeof(kItemsTxtRecordExpected), "DATATBLS_GetItemsTxtRecord");
	const bool needStatRead = g_config.gambleCostReductionStatId != 0 || !g_config.ignoreVendorReduction || wantReduced;
	if (okPrice && needStatRead) {
		okPrice = Expect(ctx, Rva::ReadUnitStat, kReadUnitStatExpected, sizeof(kReadUnitStatExpected), "ReadWideUnitStat");
	}
	// Without the code lookup the hook still runs; the "gamble cost" option just falls back to the game price.
	const bool okItemCost = okPrice && g_config.useItemGambleCost
	                     && Expect(ctx, Rva::GetItemIdFromItemCode, kItemIdFromCodeExpected, sizeof(kItemIdFromCodeExpected), "DATATBLS_GetItemIdFromItemCode");
	// Feature D needs the sell price (price hook) and every function the tagging calls.
	const bool okReduced = okPrice && wantReduced
	                    && Expect(ctx, Rva::IdentifyItem, kIdentifyExpected, sizeof(kIdentifyExpected), "D2GAME_ITEMS_Identify")
	                    && Expect(ctx, Rva::GambleIdentifyCall, kGambleIdentifyCallExpected, sizeof(kGambleIdentifyCallExpected), "BuyItem gamble Identify call")
	                    && Expect(ctx, Rva::GetStatListFromUnitStateOrFlag, kGetStatListExpected, sizeof(kGetStatListExpected), "STATLIST_GetStatListFromUnitStateOrFlag")
	                    && Expect(ctx, Rva::AllocStatList, kAllocStatListExpected, sizeof(kAllocStatListExpected), "STATLIST_AllocStatList")
	                    && Expect(ctx, Rva::AttachStatList, kAttachStatListExpected, sizeof(kAttachStatListExpected), "AttachWideStatList")
	                    && Expect(ctx, Rva::SetListStat, kSetListStatExpected, sizeof(kSetListStatExpected), "SetWideListStat")
	                    && Expect(ctx, Rva::GetUnitId, kGetUnitIdExpected, sizeof(kGetUnitIdExpected), "maybe_UNITS_GetUnitId");
	if (!okWindow && !okVendor && !okPrice) {
		ctx->LogError("gamble-tweaks: no feature passed its byte check; nothing installed.");
		return true; // keep the data features running
	}

	const D2RL::MutationService* mut = nullptr;
	if (ctx->QueryService(&mut) != D2RL::ServiceQueryResult::Success || !D2RL::HasMutationServiceField(mut, D2RL::MutationServiceRequiredSize)) {
		ctx->LogError("gamble-tweaks: MutationService unavailable; cannot install hooks.");
		return false;
	}

	D2RL::Mutations::TransactionHandle tx = D2RL::Mutations::InvalidTransactionHandle;
	if (mut->beginTransaction(ctx, &tx) != D2RL::Mutations::Result::Success) {
		ctx->LogError("gamble-tweaks: beginTransaction failed.");
		return false;
	}

	bool staged = true;
	if (okWindow) {
		staged = staged && StagePatch(ctx, mut, tx, Rva::FillGambleForcedSlots, kForcedSlotsExpected, kForcedSlotsPatched, sizeof(kForcedSlotsPatched));
	}
	if (okVendor) {
		staged = staged && StagePatch(ctx, mut, tx, Rva::GambleVendorReduction, kVendorReductionExpected, kVendorReductionPatched, sizeof(kVendorReductionPatched));
	}

	D2RL::Mutations::OperationHandle priceOp = D2RL::Mutations::InvalidOperationHandle;
	if (staged && okPrice) {
		const D2RL::Mutations::InlineHookRequest hook {
			.structSize   = D2RL::Mutations::InlineHookRequestSize,
			.rva          = Rva::GetTransactionCost,
			.expected     = kTransactionCostExpected,
			.expectedSize = static_cast<uint32_t>(sizeof(kTransactionCostExpected)),
			.target       = reinterpret_cast<void*>(&HookGetTransactionCost),
		};
		staged = mut->stageInlineHook(ctx, tx, &hook, &priceOp) == D2RL::Mutations::Result::Success;
	}

	D2RL::Mutations::OperationHandle identifyOp = D2RL::Mutations::InvalidOperationHandle;
	if (staged && okReduced) {
		const D2RL::Mutations::InlineHookRequest hook {
			.structSize   = D2RL::Mutations::InlineHookRequestSize,
			.rva          = Rva::IdentifyItem,
			.expected     = kIdentifyExpected,
			.expectedSize = static_cast<uint32_t>(sizeof(kIdentifyExpected)),
			.target       = reinterpret_cast<void*>(&HookIdentifyItem),
		};
		staged = mut->stageInlineHook(ctx, tx, &hook, &identifyOp) == D2RL::Mutations::Result::Success;
	}

	if (!staged) {
		mut->cancelTransaction(ctx, tx);
		ctx->LogError("gamble-tweaks: staging failed; nothing installed.");
		return false;
	}

	// The hook reads these as soon as the commit lands.
	g_getItemsTxtRecord = reinterpret_cast<GetItemsTxtRecordFn>(ctx->exeBase + Rva::GetItemsTxtRecord);
	g_readUnitStat      = reinterpret_cast<ReadUnitStatFn>(ctx->exeBase + Rva::ReadUnitStat);
	if (okItemCost) {
		g_getItemIdFromCode = reinterpret_cast<GetItemIdFromCodeFn>(ctx->exeBase + Rva::GetItemIdFromItemCode);
	}
	if (okReduced) {
		g_gambleIdentifyReturn = static_cast<uintptr_t>(ctx->exeBase + Rva::GambleIdentifyReturn);
		g_getStatList          = reinterpret_cast<GetStatListFn>(ctx->exeBase + Rva::GetStatListFromUnitStateOrFlag);
		g_allocStatList        = reinterpret_cast<AllocStatListFn>(ctx->exeBase + Rva::AllocStatList);
		g_attachStatList       = reinterpret_cast<AttachStatListFn>(ctx->exeBase + Rva::AttachStatList);
		g_setListStat          = reinterpret_cast<SetListStatFn>(ctx->exeBase + Rva::SetListStat);
		g_getUnitId            = reinterpret_cast<GetUnitIdFn>(ctx->exeBase + Rva::GetUnitId);
	}

	D2RL::Mutations::CommitResult result { .structSize = D2RL::Mutations::CommitResultSize };
	if (mut->commit(ctx, tx, &result) != D2RL::Mutations::Result::Success || result.result != D2RL::Mutations::Result::Success) {
		char msg[160] {};
		std::snprintf(msg, sizeof(msg), "gamble-tweaks: commit failed (result %u, reason %u); original bytes restored.", static_cast<unsigned>(result.result),
		              static_cast<unsigned>(result.failureReason));
		ctx->LogError(msg);
		return false;
	}

	if (okPrice) {
		void* original = nullptr;
		if (mut->getInlineHookOriginal(ctx, tx, priceOp, &original) != D2RL::Mutations::Result::Success || original == nullptr) {
			// Cannot happen after a successful commit; without the original the hook
			// must not run, and it cannot be removed here, so fail the plugin load.
			ctx->LogError("gamble-tweaks: no original pointer for ITEMS_GetTransactionCost.");
			return false;
		}
		g_originalGetTransactionCost.store(reinterpret_cast<GetTransactionCostFn>(original), std::memory_order_release);
		g_priceHookActive            = true;
	}
	if (okReduced) {
		void* original = nullptr;
		if (mut->getInlineHookOriginal(ctx, tx, identifyOp, &original) != D2RL::Mutations::Result::Success || original == nullptr) {
			ctx->LogError("gamble-tweaks: no original pointer for D2GAME_ITEMS_Identify.");
			return false;
		}
		g_originalIdentifyItem.store(reinterpret_cast<IdentifyItemFn>(original), std::memory_order_release);
		g_reducedValueActive = true;
	}
	g_windowPatched          = okWindow;
	g_vendorReductionPatched = okVendor;
	ctx->LogInfo("gamble-tweaks: hooks/patches committed.");
	return true;
}

// ---------------------------------------------------------------------------
static auto StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* cmd, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (cmd == nullptr || cmd->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	size_t rows = 0;
	{
		std::shared_lock lock(g_entriesLock);
		rows = g_entries.size();
	}
	char msg[384] {};
	std::snprintf(msg, sizeof(msg),
		"gamble-tweaks: rows=%zu | A window=%d | B constPrice=%d itemCost=%d | C stat=%u pct=%d cap=%u noVendor=%d | D stat=%u %u/%u/%u/%u active=%d | priceHook=%d",
		rows, g_windowPatched, g_config.useConstantGamblePrice, g_getItemIdFromCode != nullptr, g_config.gambleCostReductionStatId, g_config.gambleCostReductionPercent,
		g_config.gambleCostReductionCap, g_vendorReductionPatched, g_config.reducedValueStatId, g_config.reducedValueMagic, g_config.reducedValueRare,
		g_config.reducedValueSet, g_config.reducedValueUnique, g_reducedValueActive, g_priceHookActive);
	cmd->plugin->WriteConsoleMessage(msg);
	return D2RL::ConsoleCommandResult::Handled;
}

// ---------------------------------------------------------------------------
static constexpr D2RL::PluginInfo kInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "gamble-tweaks",
	.name        = "Gamble Tweaks",
	.version     = "0.4.0",
	.author      = "Kain",
	.description = "Gamble window, gamble price, and gamble-cost stat tweaks.",
	// NativeHooks for the price hook. Shared so host/client match on TCP/IP.
	.flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &kInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* ctx) noexcept -> bool {
	if (ctx == nullptr) {
		return false;
	}
	ctx->LogInfo("gamble-tweaks loading.");

	(void)ctx->EnsureConfig();
	std::array<char, 8192> buf {};
	uint32_t required = 0;
	if (ctx->ReadConfig(buf.data(), static_cast<uint32_t>(buf.size()), &required)) {
		ParseConfig(buf.data(), g_config);
	} else {
		ctx->LogWarn("gamble-tweaks: could not read config; using defaults.");
	}

	const D2RL::ResourceService* res = nullptr;
	if (ctx->QueryService(&res) != D2RL::ServiceQueryResult::Success || !D2RL::HasResourceServiceField(res, D2RL::ResourceServiceRequiredSize)) {
		ctx->LogError("gamble-tweaks: ResourceService missing.");
		return false;
	}
	const D2RL::Resources::ResourceRegistration resource {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.path       = kGambleTablePath,
		.bytes      = kGambleTableText,
		.byteCount  = sizeof(kGambleTableText) - 1,
	};
	D2RL::Resources::RegistrationHandle resHandle = D2RL::Resources::InvalidHandle;
	if (res->registerResource(ctx, &resource, &resHandle) != D2RL::Resources::Result::Success) {
		ctx->LogError("gamble-tweaks: failed to register default gamble table.");
		return false;
	}

	if (ctx->QueryService(&g_customTables) != D2RL::ServiceQueryResult::Success || !D2RL::HasCustomTableServiceField(g_customTables, D2RL::CustomTableServiceRequiredSize)) {
		ctx->LogError("gamble-tweaks: CustomTableService missing.");
		return false;
	}
	const D2RL::CustomTables::TableRegistration table {
		.structSize   = D2RL::CustomTables::TableRegistrationSize,
		.name         = "gamble",
		.banks        = D2RL::CustomTables::TableBank::Rotw,
		.rowSize      = sizeof(GambleRow),
		.columns      = kGambleColumns.data(),
		.columnCount  = static_cast<uint32_t>(kGambleColumns.size()),
		.columnStride = D2RL::CustomTables::ColumnDefinitionSize,
	};
	if (g_customTables->registerTable(ctx, &table, &g_gambleTable) != D2RL::CustomTables::Result::Success) {
		ctx->LogError("gamble-tweaks: failed to register gamble table.");
		return false;
	}

	const D2RL::LifecycleService* life = nullptr;
	if (ctx->QueryService(&life) != D2RL::ServiceQueryResult::Success || !D2RL::HasLifecycleServiceField(life, D2RL::LifecycleServiceRequiredSize)) {
		ctx->LogError("gamble-tweaks: LifecycleService missing.");
		return false;
	}
	const D2RL::Lifecycle::DataTablesLoadedListener listener {
		.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize,
		.callback   = OnDataTablesLoaded,
	};
	D2RL::Lifecycle::ListenerHandle lh = D2RL::Lifecycle::InvalidHandle;
	if (life->registerDataTablesLoadedListener(ctx, &listener, &lh) != D2RL::Lifecycle::Result::Success) {
		ctx->LogError("gamble-tweaks: failed to register tables-loaded listener.");
		return false;
	}

	(void)ctx->RegisterConsoleCommand("gamble-tweaks", StatusCommand, "Show gamble-tweaks status.");

	if (!InstallHooksAndPatches(ctx)) {
		return false;
	}

	ctx->LogInfo("gamble-tweaks loaded.");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	std::unique_lock lock(g_entriesLock);
	g_entries.clear();
}
