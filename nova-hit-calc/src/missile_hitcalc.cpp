// missile-hitcalc — D2RLoader plugin
//
// Makes the srvhitfunc 5 / clthitfunc 4 "nova ring" take its missile count from the
// missiles.txt row's SHitCalc1 (server) / CHitCalc1 (client) instead of the fixed step
// sHitPar1 / cHitPar1, so the count scales with skill level and synergies. A calc that
// evaluates to <= 0 (or an empty column) falls through to the original function.
//
// Both hooks spawn the calc's count of sub-missiles, spread evenly over the game's
// 64-direction table (i = k*64/n), using the same params and calls as the vanilla function
// on that side (server: SrvHit05; client: CltHit04).
//
// Hook method: SrvHit05 / CltHit04 are reached only through slot 5 of the server missile
// hit table / slot 4 of the client one (no other reference in the exe image), so each
// slot's pointer is swapped with an 8-byte byte patch. The original is called directly;
// no trampoline.
//
// Target: D2RLoader.exe; all RVAs are relative to ctx->exeBase. Code is analysed in
// /d2rloader_full.dmp. Mechanism reference is D2MOO (behaviour only, never offsets).
// Design: hit_nova_func/HANDOFF-missile-hitcalc.md; RE findings: notes/missile-hitcalc.md.

#include <D2RLPlugin/api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// RE-supplied (notes/missile-hitcalc.md). Zero → that feature is skipped.
// ---------------------------------------------------------------------------
namespace Rva {
// Server hit table slot 5 (table 0x2391030 + 5*8), holds &MISSMODE_SrvHit05_Unused.
inline constexpr uint64_t SrvHitSlot5 = 0x2391058;
// MISSMODE_SrvHit05_Unused (D2MOO MissMode.cpp).
inline constexpr uint64_t SrvHit05 = 0x45FD20;

// Game helpers called by the hooks.
inline constexpr uint64_t GetMissilesTxtRecord      = 0x166040; // DATATBLS_GetMissilesTxtRecord
inline constexpr uint64_t SUnitGetOwner             = 0x490300; // SUNIT_GetOwner
inline constexpr uint64_t MissileGetSkill           = 0x3BC450; // MISSILE_GetSkill
inline constexpr uint64_t MissileGetLevel           = 0x3BB470; // MISSILE_GetLevel
inline constexpr uint64_t UnitGetCastId             = 0x34B6C0; // maybe_UNITS_GetCastId
inline constexpr uint64_t MissileSetTargetX         = 0x3BD8A0; // MISSILE_SetTargetX
inline constexpr uint64_t MissileSetTargetY         = 0x3BDA20; // MISSILE_SetTargetY
inline constexpr uint64_t EvaluateMissileFormula    = 0x3B4A90; // MISSILE_EvaluateMissileFormula
inline constexpr uint64_t CreateMissileFromParams   = 0x5371A0; // MISSILES_CreateMissileFromParams

// 64-direction ring tables used by SrvHit05 (radius 30), int32 each.
inline constexpr uint64_t SrvRingX = 0x1D1B650;
inline constexpr uint64_t SrvRingY = 0x1D1B750;

// Client hit table slot 4 (table 0x2358710 + 4*8), holds &CltHit04. Count dword 0x2358A64 = 81.
inline constexpr uint64_t CltHitSlot4 = 0x2358730;
// Client hit func 4: client mirror of SrvHit05 (not in D2MOO). int32_t(missile), returns 1.
inline constexpr uint64_t CltHit04 = 0x1AE5D0;

// Client helpers called by HookCltHit04.
inline constexpr uint64_t UnitGetDataTableSet = 0x34A0E0; // maybe_UNITS_GetDataTableSet
inline constexpr uint64_t CUnitGetOwner       = 0x09A7B0; // client owner lookup (flag 0x400, +0xE8/+0xEC)
inline constexpr uint64_t CreateClientMissile = 0x1B7760; // client create-from-params (params, 0)

// 64-direction ring tables used by CltHit04 (same values as the server tables).
inline constexpr uint64_t CltRingX = 0x1CD9690;
inline constexpr uint64_t CltRingY = 0x1CD9790;
} // namespace Rva

// Entry bytes read in Ghidra, whole instructions, stopping before the first rel32 or
// RIP-relative operand. Checked before anything is patched or called.
static constexpr uint8_t kSrvHit05Entry[] = {
	0x40, 0x53,                               // push rbx
	0x55,                                     // push rbp
	0x57,                                     // push rdi
	0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00, // sub rsp, 0xD0
};
static constexpr uint8_t kGetMissilesTxtRecordEntry[] = {
	0x40, 0x53,             // push rbx
	0x48, 0x83, 0xEC, 0x30, // sub rsp, 0x30
	0x48, 0x63, 0xDA,       // movsxd rbx, edx
};
static constexpr uint8_t kSUnitGetOwnerEntry[] = {
	0x48, 0x89, 0x74, 0x24, 0x18, // mov [rsp+18h], rsi
	0x57,                         // push rdi
	0x48, 0x83, 0xEC, 0x20,       // sub rsp, 0x20
	0x48, 0x8B, 0xFA,             // mov rdi, rdx
	0x48, 0x8B, 0xF1,             // mov rsi, rcx
};
// MISSILE_GetSkill and MISSILE_GetLevel share this prologue.
static constexpr uint8_t kMissileGetterEntry[] = {
	0x40, 0x53,             // push rbx
	0x48, 0x83, 0xEC, 0x20, // sub rsp, 0x20
	0x48, 0x8B, 0xD9,       // mov rbx, rcx
	0x48, 0x85, 0xC9,       // test rcx, rcx
	0x74, 0x0A,             // jz +0xA
};
static constexpr uint8_t kUnitGetCastIdEntry[] = {
	0xB8, 0xFF, 0xFF, 0xFF, 0xFF,       // mov eax, -1
	0x48, 0x85, 0xC9,                   // test rcx, rcx
	0x74, 0x06,                         // jz +6
	0x8B, 0x81, 0x2C, 0x01, 0x00, 0x00, // mov eax, [rcx+0x12C]
	0xC3,                               // ret
};
// MISSILE_SetTargetX and MISSILE_SetTargetY share this prologue.
static constexpr uint8_t kMissileSetterEntry[] = {
	0x48, 0x89, 0x5C, 0x24, 0x10, // mov [rsp+10h], rbx
	0x57,                         // push rdi
	0x48, 0x83, 0xEC, 0x20,       // sub rsp, 0x20
	0x8B, 0xFA,                   // mov edi, edx
	0x48, 0x8B, 0xD9,             // mov rbx, rcx
	0x48, 0x85, 0xC9,             // test rcx, rcx
	0x74, 0x0A,                   // jz +0xA
};
static constexpr uint8_t kEvaluateMissileFormulaEntry[] = {
	0x48, 0x89, 0x5C, 0x24, 0x08, // mov [rsp+8], rbx
	0x48, 0x89, 0x6C, 0x24, 0x10, // mov [rsp+10h], rbp
	0x48, 0x89, 0x74, 0x24, 0x18, // mov [rsp+18h], rsi
	0x57,                         // push rdi
};
static constexpr uint8_t kCreateMissileFromParamsEntry[] = {
	0x40, 0x55,                               // push rbp
	0x57,                                     // push rdi
	0x41, 0x56,                               // push r14
	0x48, 0x8D, 0x6C, 0x24, 0xC0,             // lea rbp, [rsp-0x40]
	0x48, 0x81, 0xEC, 0x40, 0x01, 0x00, 0x00, // sub rsp, 0x140
};

static constexpr uint8_t kCltHit04Entry[] = {
	0x40, 0x53,                               // push rbx
	0x56,                                     // push rsi
	0x57,                                     // push rdi
	0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00, // sub rsp, 0xD0
};
static constexpr uint8_t kUnitGetDataTableSetEntry[] = {
	0x48, 0x83, 0xEC, 0x28, // sub rsp, 0x28
	0x48, 0x85, 0xC9,       // test rcx, rcx
	0x75, 0x1A,             // jnz +0x1A
	0x88, 0x4C, 0x24, 0x30, // mov [rsp+30h], cl
	0x48, 0x8D, 0x4C, 0x24, 0x30, // lea rcx, [rsp+30h]
};
static constexpr uint8_t kCUnitGetOwnerEntry[] = {
	0x40, 0x53,                   // push rbx
	0x48, 0x83, 0xEC, 0x20,       // sub rsp, 0x20
	0xBA, 0x00, 0x04, 0x00, 0x00, // mov edx, 0x400
	0x48, 0x8B, 0xD9,             // mov rbx, rcx
};
static constexpr uint8_t kCreateClientMissileEntry[] = {
	0x89, 0x54, 0x24, 0x10,                   // mov [rsp+10h], edx
	0x55,                                     // push rbp
	0x57,                                     // push rdi
	0x41, 0x55,                               // push r13
	0x41, 0x57,                               // push r15
	0x48, 0x8D, 0x6C, 0x24, 0xC1,             // lea rbp, [rsp-0x3F]
	0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, // sub rsp, 0x88
	0x48, 0x8B, 0xF9,                         // mov rdi, rcx
	0x48, 0x8B, 0x49, 0x08,                   // mov rcx, [rcx+8]
};

// The game's 64-direction ring (radius 30), read from the server tables in Ghidra. Used
// only to check the game's tables before the hook is enabled; the hook reads the game's.
static constexpr int32_t kRingX[64] = {
	30, 29, 29, 28, 27, 26, 24, 23, 21, 19, 16, 14, 11, 8, 5, 2,
	0, -2, -5, -8, -11, -14, -16, -19, -21, -23, -24, -26, -27, -28, -29, -29,
	-30, -29, -29, -28, -27, -26, -24, -23, -21, -19, -16, -14, -11, -8, -5, -2,
	0, 2, 5, 8, 11, 14, 16, 19, 21, 23, 24, 26, 27, 28, 29, 29,
};
static constexpr int32_t kRingY[64] = {
	0, 2, 5, 8, 11, 14, 16, 19, 21, 23, 24, 26, 27, 28, 29, 29,
	30, 29, 29, 28, 27, 26, 24, 23, 21, 19, 16, 14, 11, 8, 5, 2,
	0, -2, -5, -8, -11, -14, -16, -19, -21, -23, -24, -26, -27, -28, -29, -29,
	-30, -29, -29, -28, -27, -26, -24, -23, -21, -19, -16, -14, -11, -8, -5, -2,
};

// Missiles row (stride 0x1CC), from D2R's missiles.txt field table.
namespace MissilesRow {
inline constexpr size_t HitSubMissile1 = 0x46; // int16
inline constexpr size_t SHitPar1       = 0x70; // int32
inline constexpr size_t SHitCalc1      = 0xAC; // uint32, 0xFFFFFFFF when empty
inline constexpr size_t CltHitSubMissile1 = 0x4E; // int16
inline constexpr size_t CHitPar1          = 0x90; // int32
inline constexpr size_t CHitCalc1         = 0xB0; // uint32, 0xFFFFFFFF when empty
// RotW calc column. The game also evaluates it for every missile it creates and stores the
// result on the missile (pMissileData+6); only a few RotW missile funcs read that back.
inline constexpr size_t Radius            = 0x1C8; // uint32, 0xFFFFFFFF when empty
} // namespace MissilesRow

// D2GameStrc+0x106: data-table bank byte passed to every DATATBLS getter.
inline constexpr size_t kGameBankOffset = 0x106;
// D2UnitStrc+0x04: dwClassId (what maybe_UNITS_GetClassId returns).
inline constexpr size_t kUnitClassIdOffset = 0x04;

// MISSILES_CreateMissileFromParams params (D2MOO D2MissileStrc), D2R layout. Only the
// fields SrvHit05 writes are named; the rest stays zero as in vanilla.
struct MissileParams {
	uint32_t dwFlags;         // 0x00  2 = target is an offset from the origin
	uint32_t unk04;
	void*    pOwner;          // 0x08
	void*    pOrigin;         // 0x10
	void*    pTarget;         // 0x18
	int32_t  nMissile;        // 0x20
	int32_t  nX;              // 0x24
	int32_t  nY;              // 0x28
	int32_t  nTargetX;        // 0x2C
	int32_t  nTargetY;        // 0x30
	uint8_t  unk34[0x08];
	int32_t  nSkill;          // 0x3C
	int32_t  nSkillLevel;     // 0x40
	uint8_t  unk44[0x24];
	int32_t  nCastId;         // 0x68
	uint8_t  unk6C[0x2C];
};
static_assert(offsetof(MissileParams, pOwner) == 0x08);
static_assert(offsetof(MissileParams, pOrigin) == 0x10);
static_assert(offsetof(MissileParams, nMissile) == 0x20);
static_assert(offsetof(MissileParams, nTargetX) == 0x2C);
static_assert(offsetof(MissileParams, nTargetY) == 0x30);
static_assert(offsetof(MissileParams, nSkill) == 0x3C);
static_assert(offsetof(MissileParams, nSkillLevel) == 0x40);
static_assert(offsetof(MissileParams, nCastId) == 0x68);
static_assert(sizeof(MissileParams) == 0x98); // vanilla zeroes [rsp+0x20, rsp+0xB8)

inline constexpr uint32_t kMissileParamsTargetIsOffset = 2;
// Radius of the game's direction tables. MISSILES_CreateMissileFromParams rejects a moving
// missile whose target is more than 99 from its origin on either axis.
inline constexpr int32_t kVanillaRingRadius = 30;
inline constexpr int32_t kMaxRingRadius     = 99;
// SrvHit05's return value after spawning the ring; CltHit04 always returns 1.
inline constexpr int32_t kSrvHit05Spawned = 3;
inline constexpr int32_t kCltHit04Done    = 1;

// ---------------------------------------------------------------------------
// Config (embedded config.toml, read with ReadConfig).
// ---------------------------------------------------------------------------
struct Config {
	bool    srvhit05 = true;
	bool    clthit04 = true;
	int32_t maxRingCount = 64;
	int32_t ringRadius = kVanillaRingRadius;
	bool    logCalls = false;
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

auto parseInt(const std::string& v, int32_t fallback) -> int32_t {
	std::string t = trim(v);
	if (t.empty() || t.size() > 9 || t.find_first_not_of("0123456789") != std::string::npos) {
		return fallback;
	}
	return static_cast<int32_t>(std::stol(t));
}

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
		if (section != "missile-hitcalc") {
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		std::string key = trim(line.substr(0, eq));
		std::string val = trim(line.substr(eq + 1));
		if (key == "srvhit05_count_from_calc") {
			out.srvhit05 = parseBool(val, out.srvhit05);
		} else if (key == "clthit04_count_from_calc") {
			out.clthit04 = parseBool(val, out.clthit04);
		} else if (key == "max_ring_count") {
			out.maxRingCount = parseInt(val, out.maxRingCount);
		} else if (key == "ring_radius") {
			out.ringRadius = parseInt(val, out.ringRadius);
		} else if (key == "log_calls") {
			out.logCalls = parseBool(val, out.logCalls);
		}
	}
	out.maxRingCount = std::clamp(out.maxRingCount, 1, 64);
	out.ringRadius   = std::clamp(out.ringRadius, 1, kMaxRingRadius);
}

template <typename T>
auto ReadAt(const void* base, size_t offset) noexcept -> T {
	T value {};
	std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(T));
	return value;
}

} // namespace

// ---------------------------------------------------------------------------
// Game functions (x64, rcx/rdx/r8/r9 then stack).
// ---------------------------------------------------------------------------
using SrvHitFn                  = int32_t(__fastcall*)(void* game, void* missile, void* hitUnit) noexcept;
using GetMissilesTxtRecordFn    = const uint8_t*(__fastcall*)(uint8_t bank, int32_t missileId) noexcept;
using SUnitGetOwnerFn           = void*(__fastcall*)(void* game, void* unit) noexcept;
using UnitGetIntFn              = int32_t(__fastcall*)(void* unit) noexcept;
using MissileSetIntFn           = void(__fastcall*)(void* missile, int32_t value) noexcept;
using EvaluateMissileFormulaFn  = int32_t(__fastcall*)(uint8_t bank, void* missile, void* owner, uint32_t calc, int32_t missileId, int32_t level) noexcept;
using CreateMissileFromParamsFn = void*(__fastcall*)(void* game, MissileParams* params) noexcept;
// CltHit04 reads only rcx; the client dispatcher also passes a second unit in rdx.
using CltHitFn                  = int32_t(__fastcall*)(void* missile, void* hitUnit) noexcept;
using UnitGetBankFn             = uint8_t(__fastcall*)(void* unit) noexcept;
using CUnitGetOwnerFn           = void*(__fastcall*)(void* unit) noexcept;
using CreateClientMissileFn     = void*(__fastcall*)(MissileParams* params, int32_t arg2) noexcept;

static const D2RL::PluginContext* g_ctx = nullptr;

static SrvHitFn                  g_originalSrvHit05 = nullptr;
static CltHitFn                  g_originalCltHit04 = nullptr;
static UnitGetBankFn             g_getDataTableSet = nullptr;
static CUnitGetOwnerFn           g_getClientOwner = nullptr;
static CreateClientMissileFn     g_createClientMissile = nullptr;
static const int32_t*            g_cltRingX = nullptr;
static const int32_t*            g_cltRingY = nullptr;
static GetMissilesTxtRecordFn    g_getMissilesTxtRecord = nullptr;
static SUnitGetOwnerFn           g_getOwner = nullptr;
static UnitGetIntFn              g_getSkill = nullptr;
static UnitGetIntFn              g_getLevel = nullptr;
static UnitGetIntFn              g_getCastId = nullptr;
static MissileSetIntFn           g_setTargetX = nullptr;
static MissileSetIntFn           g_setTargetY = nullptr;
static EvaluateMissileFormulaFn  g_evalMissileFormula = nullptr;
static CreateMissileFromParamsFn g_createMissile = nullptr;
static const int32_t*            g_srvRingX = nullptr;
static const int32_t*            g_srvRingY = nullptr;

// Logs hook calls up to this many per side, so a long session can't flood the log.
static constexpr int32_t kMaxHitLogs = 256;
static std::atomic<int32_t> g_srvHitLogs {0};
static std::atomic<int32_t> g_cltHitLogs {0};

static void LogHit(std::atomic<int32_t>& counter, const char* side, const char* column, int32_t classId, int32_t level, uint32_t calc, int32_t value, int32_t count, int32_t radius = 0) noexcept {
	const int32_t n = counter.fetch_add(1);
	if (!g_config.logCalls || g_ctx == nullptr || n > kMaxHitLogs) {
		return;
	}
	char msg[192] {};
	if (n == kMaxHitLogs) {
		std::snprintf(msg, sizeof(msg), "missile-hitcalc: %s log limit reached; further calls not logged.", side);
	} else if (count > 0) {
		std::snprintf(msg, sizeof(msg), "missile-hitcalc: %s missile=%d lvl=%d %s=0x%X -> %d, ring of %d at radius %d",
			side, classId, level, column, calc, value, count, radius);
	} else {
		std::snprintf(msg, sizeof(msg), "missile-hitcalc: %s missile=%d lvl=%d %s=0x%X -> %d, vanilla",
			side, classId, level, column, calc, value);
	}
	g_ctx->LogInfo(msg);
}

// Ring radius for this hit: the ring missile's own Radius calc if it gives > 0, else
// ring_radius from the config. Capped at kMaxRingRadius.
static auto RingRadius(uint8_t bank, void* missile, void* owner, const uint8_t* row) noexcept -> int32_t {
	const int32_t radius = g_evalMissileFormula(bank, missile, owner, ReadAt<uint32_t>(row, MissilesRow::Radius), -1, -1);
	return radius > 0 ? std::min(radius, kMaxRingRadius) : g_config.ringRadius;
}

// Direction-table offset (radius 30) scaled to `radius`.
static auto ScaleRing(int32_t offset, int32_t radius) noexcept -> int32_t {
	return offset * radius / kVanillaRingRadius;
}

// Server nova ring: SHitCalc1 > 0 → spawn that many HitSubMissile1, evenly spaced over the
// 64 directions, with the same params SrvHit05 builds. Otherwise run the original.
static auto __fastcall HookSrvHit05(void* game, void* missile, void* hitUnit) noexcept -> int32_t {
	if (!g_config.srvhit05 || game == nullptr || missile == nullptr) {
		return g_originalSrvHit05(game, missile, hitUnit);
	}
	const uint8_t  bank    = ReadAt<uint8_t>(game, kGameBankOffset);
	const int32_t  classId = ReadAt<int32_t>(missile, kUnitClassIdOffset);
	const uint8_t* row     = g_getMissilesTxtRecord(bank, classId);
	if (row == nullptr || ReadAt<int16_t>(row, MissilesRow::HitSubMissile1) < 0) {
		return g_originalSrvHit05(game, missile, hitUnit);
	}
	void* owner = g_getOwner(game, missile);
	if (owner == nullptr) {
		return g_originalSrvHit05(game, missile, hitUnit);
	}

	const uint32_t calc  = ReadAt<uint32_t>(row, MissilesRow::SHitCalc1);
	const int32_t  value = g_evalMissileFormula(bank, missile, owner, calc, -1, -1);
	const int32_t  level = g_getLevel(missile);
	if (value <= 0) {
		LogHit(g_srvHitLogs, "srvhit05", "shitcalc1", classId, level, calc, value, 0);
		return g_originalSrvHit05(game, missile, hitUnit);
	}
	const int32_t count  = std::min(value, g_config.maxRingCount);
	const int32_t radius = RingRadius(bank, missile, owner, row);
	LogHit(g_srvHitLogs, "srvhit05", "shitcalc1", classId, level, calc, value, count, radius);

	MissileParams params {};
	params.dwFlags     = kMissileParamsTargetIsOffset;
	params.pOwner      = owner;
	params.pOrigin     = missile;
	params.nSkill      = g_getSkill(missile);
	params.nSkillLevel = level;
	params.nCastId     = g_getCastId(missile);
	params.nMissile    = ReadAt<int16_t>(row, MissilesRow::HitSubMissile1);
	for (int32_t k = 0; k < count; ++k) {
		const int32_t i = (k * 64) / count;
		params.nTargetX = ScaleRing(g_srvRingX[i], radius);
		params.nTargetY = ScaleRing(g_srvRingY[i], radius);
		if (void* sub = g_createMissile(game, &params); sub != nullptr) {
			g_setTargetX(sub, params.nTargetX);
			g_setTargetY(sub, params.nTargetY);
		}
	}
	return kSrvHit05Spawned;
}

// Client nova ring: CHitCalc1 > 0 → spawn that many CltHitSubMissile1 the same way, with
// the params CltHit04 builds. Otherwise run the original.
static auto __fastcall HookCltHit04(void* missile, void* hitUnit) noexcept -> int32_t {
	if (!g_config.clthit04 || missile == nullptr) {
		return g_originalCltHit04(missile, hitUnit);
	}
	const uint8_t  bank    = g_getDataTableSet(missile);
	const int32_t  classId = ReadAt<int32_t>(missile, kUnitClassIdOffset);
	const uint8_t* row     = g_getMissilesTxtRecord(bank, classId);
	if (row == nullptr || ReadAt<int16_t>(row, MissilesRow::CltHitSubMissile1) < 0) {
		return g_originalCltHit04(missile, hitUnit);
	}
	void* owner = g_getClientOwner(missile);
	if (owner == nullptr) {
		return g_originalCltHit04(missile, hitUnit);
	}

	const uint32_t calc  = ReadAt<uint32_t>(row, MissilesRow::CHitCalc1);
	const int32_t  value = g_evalMissileFormula(bank, missile, owner, calc, -1, -1);
	const int32_t  level = g_getLevel(missile);
	if (value <= 0) {
		LogHit(g_cltHitLogs, "clthit04", "chitcalc1", classId, level, calc, value, 0);
		return g_originalCltHit04(missile, hitUnit);
	}
	const int32_t count  = std::min(value, g_config.maxRingCount);
	const int32_t radius = RingRadius(bank, missile, owner, row);
	LogHit(g_cltHitLogs, "clthit04", "chitcalc1", classId, level, calc, value, count, radius);

	MissileParams params {};
	params.dwFlags     = kMissileParamsTargetIsOffset;
	params.pOwner      = owner;
	params.pOrigin     = missile;
	params.nSkill      = g_getSkill(missile);
	params.nSkillLevel = level;
	params.nCastId     = g_getCastId(missile);
	params.nMissile    = ReadAt<int16_t>(row, MissilesRow::CltHitSubMissile1);
	for (int32_t k = 0; k < count; ++k) {
		const int32_t i = (k * 64) / count;
		params.nTargetX = ScaleRing(g_cltRingX[i], radius);
		params.nTargetY = ScaleRing(g_cltRingY[i], radius);
		if (void* sub = g_createClientMissile(&params, 0); sub != nullptr) {
			g_setTargetX(sub, params.nTargetX);
			g_setTargetY(sub, params.nTargetY);
		}
	}
	return kCltHit04Done;
}

// ---------------------------------------------------------------------------
static auto BytesMatch(const D2RL::PluginContext* ctx, uint64_t rva, const void* expected, size_t size, const char* what) -> bool {
	if (std::memcmp(reinterpret_cast<const void*>(ctx->exeBase + rva), expected, size) == 0) {
		return true;
	}
	char msg[160] {};
	std::snprintf(msg, sizeof(msg), "missile-hitcalc: %s bytes at RVA 0x%llX do not match; wrong game build?",
		what, static_cast<unsigned long long>(rva));
	ctx->LogError(msg);
	return false;
}

template <typename Fn>
auto GameFn(const D2RL::PluginContext* ctx, uint64_t rva) -> Fn {
	return reinterpret_cast<Fn>(ctx->exeBase + rva);
}

// Writes `replacement` over the 8-byte table slot at `slotRva` if it still holds
// exeBase + `originalRva`.
static auto SwapTableSlot(const D2RL::PluginContext* ctx, const D2RL::MutationService* mut, uint64_t slotRva, uint64_t originalRva, void* replacement, const char* what) -> bool {
	const uint64_t expected = static_cast<uint64_t>(ctx->exeBase + originalRva);
	const uint64_t bytes    = reinterpret_cast<uint64_t>(replacement);

	D2RL::Mutations::TransactionHandle tx = D2RL::Mutations::InvalidTransactionHandle;
	if (mut->beginTransaction(ctx, &tx) != D2RL::Mutations::Result::Success) {
		ctx->LogError("missile-hitcalc: beginTransaction failed.");
		return false;
	}
	const D2RL::Mutations::BytePatchRequest patch {
		.structSize   = D2RL::Mutations::BytePatchRequestSize,
		.rva          = slotRva,
		.expected     = &expected,
		.expectedSize = static_cast<uint32_t>(sizeof(expected)),
		.bytes        = &bytes,
		.size         = static_cast<uint32_t>(sizeof(bytes)),
	};
	D2RL::Mutations::OperationHandle op = D2RL::Mutations::InvalidOperationHandle;
	const D2RL::Mutations::Result staged = mut->stageBytePatch(ctx, tx, &patch, &op);
	if (staged != D2RL::Mutations::Result::Success) {
		mut->cancelTransaction(ctx, tx);
		char msg[128] {};
		std::snprintf(msg, sizeof(msg), "missile-hitcalc: staging the %s slot patch failed (result %u).", what, static_cast<uint32_t>(staged));
		ctx->LogError(msg);
		return false;
	}
	D2RL::Mutations::CommitResult result { .structSize = D2RL::Mutations::CommitResultSize };
	const D2RL::Mutations::Result committed = mut->commit(ctx, tx, &result);
	if (committed != D2RL::Mutations::Result::Success || result.result != D2RL::Mutations::Result::Success) {
		mut->cancelTransaction(ctx, tx);
		char msg[160] {};
		std::snprintf(msg, sizeof(msg), "missile-hitcalc: %s slot patch commit failed (result %u, reason %u); original restored.",
			what, static_cast<uint32_t>(result.result), static_cast<uint32_t>(result.failureReason));
		ctx->LogError(msg);
		return false;
	}
	return true;
}

// Checks and resolves the D2Common helpers both hooks use.
static auto ResolveSharedHelpers(const D2RL::PluginContext* ctx) -> bool {
	if (!BytesMatch(ctx, Rva::GetMissilesTxtRecord, kGetMissilesTxtRecordEntry, sizeof(kGetMissilesTxtRecordEntry), "DATATBLS_GetMissilesTxtRecord")
		|| !BytesMatch(ctx, Rva::MissileGetSkill, kMissileGetterEntry, sizeof(kMissileGetterEntry), "MISSILE_GetSkill")
		|| !BytesMatch(ctx, Rva::MissileGetLevel, kMissileGetterEntry, sizeof(kMissileGetterEntry), "MISSILE_GetLevel")
		|| !BytesMatch(ctx, Rva::UnitGetCastId, kUnitGetCastIdEntry, sizeof(kUnitGetCastIdEntry), "maybe_UNITS_GetCastId")
		|| !BytesMatch(ctx, Rva::MissileSetTargetX, kMissileSetterEntry, sizeof(kMissileSetterEntry), "MISSILE_SetTargetX")
		|| !BytesMatch(ctx, Rva::MissileSetTargetY, kMissileSetterEntry, sizeof(kMissileSetterEntry), "MISSILE_SetTargetY")
		|| !BytesMatch(ctx, Rva::EvaluateMissileFormula, kEvaluateMissileFormulaEntry, sizeof(kEvaluateMissileFormulaEntry), "MISSILE_EvaluateMissileFormula")) {
		return false;
	}
	g_getMissilesTxtRecord = GameFn<GetMissilesTxtRecordFn>(ctx, Rva::GetMissilesTxtRecord);
	g_getSkill             = GameFn<UnitGetIntFn>(ctx, Rva::MissileGetSkill);
	g_getLevel             = GameFn<UnitGetIntFn>(ctx, Rva::MissileGetLevel);
	g_getCastId            = GameFn<UnitGetIntFn>(ctx, Rva::UnitGetCastId);
	g_setTargetX           = GameFn<MissileSetIntFn>(ctx, Rva::MissileSetTargetX);
	g_setTargetY           = GameFn<MissileSetIntFn>(ctx, Rva::MissileSetTargetY);
	g_evalMissileFormula   = GameFn<EvaluateMissileFormulaFn>(ctx, Rva::EvaluateMissileFormula);
	return true;
}

// Swaps server hit-table slot 5 to HookSrvHit05. Returns false only on an unexpected
// service failure; a build mismatch just leaves the hook off.
static auto InstallSrvHit05(const D2RL::PluginContext* ctx, const D2RL::MutationService* mut) -> bool {
	if (!BytesMatch(ctx, Rva::SrvHit05, kSrvHit05Entry, sizeof(kSrvHit05Entry), "SrvHit05")
		|| !BytesMatch(ctx, Rva::SUnitGetOwner, kSUnitGetOwnerEntry, sizeof(kSUnitGetOwnerEntry), "SUNIT_GetOwner")
		|| !BytesMatch(ctx, Rva::CreateMissileFromParams, kCreateMissileFromParamsEntry, sizeof(kCreateMissileFromParamsEntry), "MISSILES_CreateMissileFromParams")
		|| !BytesMatch(ctx, Rva::SrvRingX, kRingX, sizeof(kRingX), "server ring X table")
		|| !BytesMatch(ctx, Rva::SrvRingY, kRingY, sizeof(kRingY), "server ring Y table")) {
		ctx->LogWarn("missile-hitcalc: srvhit05 hook left off.");
		return true;
	}
	g_getOwner      = GameFn<SUnitGetOwnerFn>(ctx, Rva::SUnitGetOwner);
	g_createMissile = GameFn<CreateMissileFromParamsFn>(ctx, Rva::CreateMissileFromParams);
	g_srvRingX      = reinterpret_cast<const int32_t*>(ctx->exeBase + Rva::SrvRingX);
	g_srvRingY      = reinterpret_cast<const int32_t*>(ctx->exeBase + Rva::SrvRingY);
	// Set before commit: the game may call the hook as soon as the slot is written.
	g_originalSrvHit05 = GameFn<SrvHitFn>(ctx, Rva::SrvHit05);

	if (!SwapTableSlot(ctx, mut, Rva::SrvHitSlot5, Rva::SrvHit05, reinterpret_cast<void*>(&HookSrvHit05), "srvhit05")) {
		g_originalSrvHit05 = nullptr;
		return false;
	}
	ctx->LogInfo("missile-hitcalc: srvhit05 hook on (server hit table slot 5).");
	return true;
}

// Swaps client hit-table slot 4 to HookCltHit04. Same failure rules as the server hook.
static auto InstallCltHit04(const D2RL::PluginContext* ctx, const D2RL::MutationService* mut) -> bool {
	if (!BytesMatch(ctx, Rva::CltHit04, kCltHit04Entry, sizeof(kCltHit04Entry), "CltHit04")
		|| !BytesMatch(ctx, Rva::UnitGetDataTableSet, kUnitGetDataTableSetEntry, sizeof(kUnitGetDataTableSetEntry), "maybe_UNITS_GetDataTableSet")
		|| !BytesMatch(ctx, Rva::CUnitGetOwner, kCUnitGetOwnerEntry, sizeof(kCUnitGetOwnerEntry), "client owner getter")
		|| !BytesMatch(ctx, Rva::CreateClientMissile, kCreateClientMissileEntry, sizeof(kCreateClientMissileEntry), "client create-missile")
		|| !BytesMatch(ctx, Rva::CltRingX, kRingX, sizeof(kRingX), "client ring X table")
		|| !BytesMatch(ctx, Rva::CltRingY, kRingY, sizeof(kRingY), "client ring Y table")) {
		ctx->LogWarn("missile-hitcalc: clthit04 hook left off.");
		return true;
	}
	g_getDataTableSet     = GameFn<UnitGetBankFn>(ctx, Rva::UnitGetDataTableSet);
	g_getClientOwner      = GameFn<CUnitGetOwnerFn>(ctx, Rva::CUnitGetOwner);
	g_createClientMissile = GameFn<CreateClientMissileFn>(ctx, Rva::CreateClientMissile);
	g_cltRingX            = reinterpret_cast<const int32_t*>(ctx->exeBase + Rva::CltRingX);
	g_cltRingY            = reinterpret_cast<const int32_t*>(ctx->exeBase + Rva::CltRingY);
	g_originalCltHit04    = GameFn<CltHitFn>(ctx, Rva::CltHit04);

	if (!SwapTableSlot(ctx, mut, Rva::CltHitSlot4, Rva::CltHit04, reinterpret_cast<void*>(&HookCltHit04), "clthit04")) {
		g_originalCltHit04 = nullptr;
		return false;
	}
	ctx->LogInfo("missile-hitcalc: clthit04 hook on (client hit table slot 4).");
	return true;
}

static auto InstallHooks(const D2RL::PluginContext* ctx) -> bool {
	const bool wantSrv = g_config.srvhit05 && Rva::SrvHitSlot5 != 0 && Rva::SrvHit05 != 0;
	const bool wantClt = g_config.clthit04 && Rva::CltHitSlot4 != 0 && Rva::CltHit04 != 0;
	if (!wantSrv && !wantClt) {
		ctx->LogInfo("missile-hitcalc: no hooks enabled.");
		return true;
	}
	const D2RL::MutationService* mut = nullptr;
	if (ctx->QueryService(&mut) != D2RL::ServiceQueryResult::Success || !D2RL::HasMutationServiceField(mut, D2RL::MutationServiceRequiredSize)
		|| mut->beginTransaction == nullptr || mut->stageBytePatch == nullptr || mut->commit == nullptr || mut->cancelTransaction == nullptr) {
		ctx->LogError("missile-hitcalc: MutationService unavailable.");
		return false;
	}
	if (!ResolveSharedHelpers(ctx)) {
		ctx->LogWarn("missile-hitcalc: shared helpers do not match; all hooks left off.");
		return true;
	}
	if (wantSrv && !InstallSrvHit05(ctx, mut)) {
		return false;
	}
	if (wantClt && !InstallCltHit04(ctx, mut)) {
		return false;
	}
	return true;
}

static auto StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* cmd, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (cmd == nullptr || cmd->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	char msg[192] {};
	std::snprintf(msg, sizeof(msg), "missile-hitcalc: srvhit05=%d (hook %s, %d calls) clthit04=%d (hook %s, %d calls) max_ring_count=%d ring_radius=%d log_calls=%d",
		g_config.srvhit05, g_originalSrvHit05 != nullptr ? "on" : "off", g_srvHitLogs.load(),
		g_config.clthit04, g_originalCltHit04 != nullptr ? "on" : "off", g_cltHitLogs.load(),
		g_config.maxRingCount, g_config.ringRadius, g_config.logCalls);
	cmd->plugin->WriteConsoleMessage(msg);
	return D2RL::ConsoleCommandResult::Handled;
}

// ---------------------------------------------------------------------------
static constexpr D2RL::PluginInfo kInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "missile-hitcalc",
	.name        = "Missile HitCalc",
	.version     = "1.1.0",
	.author      = "Kain",
	.description = "Nova-ring missile count from SHitCalc1 / CHitCalc1.",
	.flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &kInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* ctx) noexcept -> bool {
	if (ctx == nullptr) {
		return false;
	}
	g_ctx = ctx;
	ctx->LogInfo("missile-hitcalc loading.");

	(void)ctx->EnsureConfig();
	std::array<char, 4096> buf {};
	uint32_t required = 0;
	if (ctx->ReadConfig(buf.data(), static_cast<uint32_t>(buf.size()), &required)) {
		ParseConfig(buf.data(), g_config);
	} else {
		ctx->LogWarn("missile-hitcalc: could not read config; using defaults.");
	}

	(void)ctx->RegisterConsoleCommand("missile-hitcalc", StatusCommand, "Show missile-hitcalc config and active hooks.");

	if (!InstallHooks(ctx)) {
		return false;
	}

	ctx->LogInfo("missile-hitcalc loaded.");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	g_ctx = nullptr;
}
