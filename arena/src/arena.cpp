// arena - D2RLoader plugin
//
// Restores two pieces of the cut Diablo II 1.10 Arena (mods\arena, levels "Arena Tomb 1/2"):
//  - Teleport gates: the red arches (act 2 TeleportPad1/3/4, objects.txt OperateFn 27). In an arena level a gate
//    sends the player to a random OTHER gate of the same arena. Stock OperateFn 27 only finds a pad of the same class
//    in the same or an adjacent room (D2MOO ObjMode.cpp OBJECTS_OperateFunction27_TeleportPad), so the far-apart
//    arena gates would do nothing.
//  - Paired trapdoors (act 2 Trapdoor, OperateFn 16): the first click opens one (stock behaviour, mode 0 -> 2). An
//    open trapdoor leads to its partner only when the partner has been opened too; otherwise the player gets a chat
//    line (packet 0x26, that player only). Stock OperateFn 16 warps through a warp tile in the room, which the arenas
//    don't have.
// Everywhere else (Arcane Sanctuary pads, the Lut Gholein sewer trapdoor) the original function runs unchanged.
//
// How: gpObjOperateFnTable[16] / [27] (8-byte code pointers) are replaced by wrappers that call the saved original
// outside the arena levels. D2RLevels' objects part copies game slots 1-74 into its own table, so this also works
// next to it (notes/objects.md).
//
// Gate / trapdoor spots come from the config (DS1 subtile coordinates, written by arena/tools/build_data.py from the
// mod's DS1s). World subtile = level position (D2DrlgLevelStrc +0x24/+0x28, tiles) * 5 + DS1 subtile.
//
// All RVAs are relative to the D2RLoader.exe image (ctx->exeBase), read in /d2rloader_full.dmp in Ghidra on
// 2026-10-31 (notes/arena.md §5). Every call target is checked against its expected bytes before install.
//
// Status: NOT TESTED (arena/TESTS.md G*).

#include <D2RLPlugin/api.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// D2RLoader.exe RVAs (read 2026-10-31).
// ---------------------------------------------------------------------------
namespace Rva {
inline constexpr uint64_t OperateTable      = 0x23a51c0; // gpObjOperateFnTable, 75 x 8-byte pointers
inline constexpr uint64_t TrapDoor          = 0x58e230;  // OBJECTS_OperateFunction16_TrapDoor (table slot 16)
inline constexpr uint64_t TeleportPad       = 0x58ead0;  // OBJECTS_OperateFunction27_TeleportPad (table slot 27)
inline constexpr uint64_t GetRoom           = 0x34b440;  // D2ActiveRoomStrc* UNITS_GetRoom(pUnit)
inline constexpr uint64_t GetCoords         = 0x34a040;  // void UNITS_GetCoords(pUnit, int32 xy[2]) - subtiles
inline constexpr uint64_t GetAnimMode       = 0x34ab60;  // int UNITS_GetAnimMode(pUnit)
inline constexpr uint64_t ChangeAnimMode    = 0x34e5f0;  // void UNITS_ChangeAnimMode(pUnit, nMode)
inline constexpr uint64_t FreeCoords        = 0x362f90;  // room COLLISION_GetFreeCoordinates(pRoom, int32 xy[2], size, mask, 0)
inline constexpr uint64_t MovePlayer        = 0x491ca0;  // int sub_6FCBDFE0(pGame, pUnit, pRoom, x, y, 0, 0)
inline constexpr uint64_t RoomCoords        = 0x2efb40;  // void DUNGEON_GetRoomCoordinates(pRoom, 0x20 bytes)
inline constexpr uint64_t LevelIdFromRoom   = 0x2efc10;  // int DUNGEON_GetLevelIdFromRoom(pRoom)
inline constexpr uint64_t ClientFromPlayer  = 0x48fde0;  // client SUNIT_GetClientFromPlayer(pPlayer)
inline constexpr uint64_t RoomInSight       = 0x47d2d0;  // packet 0x07 (client, nLevel, tileX, tileY)
inline constexpr uint64_t SetTeleported     = 0x34f490;  // SUNIT_ToggleTeleportedFlagEx(pUnit, 1)
inline constexpr uint64_t FindRoomAt        = 0x2ef720;  // room DUNGEON_FindRoomBySubtileCoordinates(pAct, x, y)
inline constexpr uint64_t StreamRoom        = 0x2f0810;  // DUNGEON_StreamRoomAtCoords(nTblSet, pAct, tileX, tileY)
inline constexpr uint64_t GetAct            = 0x3e2b5c8; // uint8 GetActFromLevelId(nTblSet, nLevel) - FF 25 thunk
inline constexpr uint64_t ServerMessage     = 0x477a30;  // D2GAME_PACKETS_SendPacket0x26_ServerMessage(client, packet)
inline constexpr uint64_t AttachSound       = 0x491960;  // SUNIT_AttachSound(pUnit, nSoundEvent, pUpdateUnit) (pspell-ext)
inline constexpr uint64_t AllocUnit         = 0x490410;  // SUNIT_AllocUnitData(type, class, x, y, game, room, 1, mode, 0, 0) (ACT5Q3_SpawnFrozenDrehya, book-of-lore)
inline constexpr uint64_t WarpUnit          = 0x488180;  // LEVEL_WarpUnit(game, player, level, tileCalc): same act = spawn spot + move
} // namespace Rva

namespace Expected {
// Prologues (16 bytes read in the dump unless shorter).
inline constexpr uint8_t kGetRoom[]          = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9 };
inline constexpr uint8_t kGetCoords[]        = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xD9 };
inline constexpr uint8_t kGetAnimMode[]      = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1A };
inline constexpr uint8_t kChangeAnimMode[]   = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xFA, 0x48, 0x8B, 0xD9 };
inline constexpr uint8_t kFreeCoords[]       = { 0x48, 0x83, 0xEC, 0x58, 0x8B, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00 };
inline constexpr uint8_t kMovePlayer[]       = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
inline constexpr uint8_t kRoomCoords[]       = { 0x48, 0x85, 0xC9, 0x75, 0x0B, 0x0F, 0x57, 0xC0, 0x0F, 0x11, 0x02 };
inline constexpr uint8_t kLevelIdFromRoom[]  = { 0x48, 0x85, 0xC9, 0x75, 0x03, 0x33, 0xC0, 0xC3, 0x48, 0x8B, 0x49, 0x18 };
inline constexpr uint8_t kClientFromPlayer[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x74, 0x1E };
inline constexpr uint8_t kRoomInSight[]      = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0xC6, 0x44, 0x24, 0x20, 0x07 };
inline constexpr uint8_t kSetTeleported[]    = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xFA, 0x48, 0x8B, 0xD9 };
inline constexpr uint8_t kFindRoomAt[]       = { 0x44, 0x8B, 0xD2, 0x48, 0x85, 0xC9, 0x74, 0x42 };
inline constexpr uint8_t kStreamRoom[]       = { 0x48, 0x8B, 0x52, 0x70, 0xE9 };
inline constexpr uint8_t kGetActThunk[]      = { 0xFF, 0x25, 0xEA, 0xEE, 0xFF, 0xFF };
inline constexpr uint8_t kServerMessage[]    = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x90 };
inline constexpr uint8_t kAttachSound[]      = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20 };
inline constexpr uint8_t kAllocUnit[]        = { 0x40, 0x55, 0x53, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x70, 0xFC, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x90, 0x04, 0x00, 0x00 };
inline constexpr uint8_t kWarpUnit[]         = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57 };
} // namespace Expected

// Struct offsets (read 2026-10-31).
constexpr size_t   kOpGame        = 0x00;  // D2ObjOperateFnStrc pGame
constexpr size_t   kOpObject      = 0x08;  // pObject
constexpr size_t   kOpPlayer      = 0x10;  // pPlayer
constexpr size_t   kGameTableSet  = 0x106; // D2GameStrc table set byte (OperateFunction43)
constexpr size_t   kGameActs      = 0x190; // D2GameStrc act pointers: begin, +8 count (OperateFunction43)
constexpr size_t   kGameClients   = 0x138; // D2GameStrc first client (D2RCore HandleClientChatMessage)
constexpr size_t   kClientNext    = 0x628; // D2ClientStrc next client in the game (same)
constexpr size_t   kRoomDrlgRoom  = 0x18;  // D2ActiveRoomStrc pDrlgRoom (DUNGEON_GetLevelIdFromRoom)
constexpr size_t   kRoomAdjacent  = 0x00;  // ppRoomList (DUNGEON_GetAdjacentRoomsListFromRoom)
constexpr size_t   kRoomAdjCount  = 0x40;  // nNumRooms
constexpr size_t   kRoomSubtiles  = 0x80;  // x, y, w, h in subtiles (DUNGEON_GetRoomCoordinates)
constexpr size_t   kRoomFirstUnit = 0xA8;  // UNITROOM_GetFirstUnit
constexpr size_t   kUnitRoomNext  = 0x160; // UNITROOM_GetNextUnit
constexpr size_t   kDrlgRoomLevel = 0x90;  // D2DrlgRoomStrc pLevel (DRLGROOM_GetLevelId)
constexpr size_t   kLevelPos      = 0x24;  // D2DrlgLevelStrc nPosX, nPosY in tiles (DRLG_SetLevelPositionAndSize)
constexpr uint32_t kUnitPlayer    = 0;
constexpr uint32_t kUnitObject    = 2;
constexpr int32_t  kModeNeutral   = 0;
constexpr int32_t  kModeOpened    = 2;
constexpr uint32_t kFreeMask      = 0x1C09; // as OperateFunction27
constexpr uint32_t kArrivalSize   = 3;      // as OperateFunction27

using OperateFn          = int32_t(__fastcall*)(uint8_t* op, int32_t nOperateFn);
using GetRoomFn          = void*(__fastcall*)(void* pUnit);
using GetCoordsFn        = void(__fastcall*)(void* pUnit, int32_t* xy);
using GetAnimModeFn      = int32_t(__fastcall*)(void* pUnit);
using ChangeAnimModeFn   = void(__fastcall*)(void* pUnit, int32_t nMode);
using FreeCoordsFn       = void*(__fastcall*)(void* pRoom, int32_t* xy, uint32_t size, uint32_t mask, int32_t a5);
using MovePlayerFn       = int32_t(__fastcall*)(void* pGame, void* pUnit, void* pRoom, int32_t x, int32_t y, int32_t a6, int32_t a7);
using RoomCoordsFn       = void(__fastcall*)(void* pRoom, int32_t* coords);
using LevelIdFromRoomFn  = int32_t(__fastcall*)(void* pRoom);
using ClientFromPlayerFn = void*(__fastcall*)(void* pPlayer);
using RoomInSightFn      = void(__fastcall*)(void* pClient, uint64_t nLevel, uint64_t tileX, uint64_t tileY);
using SetTeleportedFn    = void(__fastcall*)(void* pUnit, int32_t bSet);
using FindRoomAtFn       = void*(__fastcall*)(void* pAct, int32_t x, int32_t y);
using StreamRoomFn       = void*(__fastcall*)(uint8_t nTblSet, void* pAct, uint32_t tileX, uint32_t tileY);
using GetActFn           = uint8_t(__fastcall*)(uint8_t nTblSet, int32_t nLevel);
using ServerMessageFn    = void(__fastcall*)(void* pClient, uint8_t* packet);
using AttachSoundFn      = void(__fastcall*)(void* pUnit, int32_t nSoundEvent, void* pUpdateUnit);
using AllocUnitFn        = void*(__fastcall*)(int32_t type, int32_t cls, int32_t x, int32_t y, void* game, void* room, uint16_t a7, int32_t mode, int32_t a9, void* a10);
using WarpUnitFn         = void(__fastcall*)(void* game, void* player, uint64_t level, uint64_t tileCalc);

const D2RL::PluginContext* g_ctx = nullptr;
OperateFn          g_origTrapDoor  = nullptr;
OperateFn          g_origPad       = nullptr;
GetRoomFn          g_getRoom       = nullptr;
GetCoordsFn        g_getCoords     = nullptr;
GetAnimModeFn      g_getAnimMode   = nullptr;
ChangeAnimModeFn   g_changeMode    = nullptr;
FreeCoordsFn       g_freeCoords    = nullptr;
MovePlayerFn       g_movePlayer    = nullptr;
RoomCoordsFn       g_roomCoords    = nullptr;
LevelIdFromRoomFn  g_levelId       = nullptr;
ClientFromPlayerFn g_client        = nullptr;
RoomInSightFn      g_roomInSight   = nullptr;
SetTeleportedFn    g_setTeleported = nullptr;
FindRoomAtFn       g_findRoomAt    = nullptr;
StreamRoomFn       g_streamRoom    = nullptr;
GetActFn           g_getAct        = nullptr;
ServerMessageFn    g_serverMessage = nullptr;
AttachSoundFn      g_attachSound   = nullptr;
AllocUnitFn        g_allocUnit     = nullptr;
WarpUnitFn         g_warpUnit      = nullptr;
bool               g_installed     = false;

std::atomic<uint32_t> g_gateJumps { 0 };
std::atomic<uint32_t> g_trapJumps { 0 };
std::atomic<uint32_t> g_sealed { 0 };
std::atomic<uint32_t> g_failed { 0 };

template <typename T>
auto Field(const void* p, size_t off) noexcept -> T {
	return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(p) + off);
}
template <typename T>
void SetField(void* p, size_t off, T v) noexcept {
	*reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off) = v;
}

void Log(const char* fmt, ...) {
	char    buf[256] {};
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (g_ctx != nullptr) {
		g_ctx->LogInfo(buf);
	}
}

// ---------------------------------------------------------------------------
// Config.
// ---------------------------------------------------------------------------
struct Spot {
	int32_t x = 0;
	int32_t y = 0;
};
struct Arena {
	std::vector<Spot> gates;
	std::vector<Spot> trapdoors; // pairs: 0-1, 2-3, ...
	std::vector<Spot> exits;     // trapdoors back to the entrance level (DS1 units, not in `trapdoors`)
};
struct Entrance {
	int32_t x = 0, y = 0, arena = 0; // absolute sub-tiles in the entrance level (`arena where`), target arena level
	int32_t object = 74;             // objects.txt id spawned (the mod's labelled copies of the Trapdoor)
};
struct Config {
	bool                   enabled      = true;
	uint8_t                messageColor = 4;      // +8 of the chat packet (D2RCore passes it as the chat colour)
	std::string            senderName   = "Arena"; // shown before the line
	bool                   messageToAll = true;    // message_scope "all" (default) / "player"
	int32_t                refusalSound = 0x14;    // SUNIT_AttachSound event: the "can't use" voice line; 0 = none
	std::string            sealedText   = "The other trapdoor is still closed.";
	std::map<int32_t, Arena> arenas;    // level id -> spots
	int32_t                entranceLevel = 40; // [arena.entrance] level (Lut Gholein)
	std::vector<Entrance>  entrances;          // plugin-spawned trapdoors in that level
	// 1.9.0 [arena.lobby]: the team duel obelisk, spawned in the entrance level (absolute subtiles, `arena where`); 0 = none
	std::vector<std::pair<int32_t, int32_t>> lobbySpots; // 1.9.3: up to kLobbySpotsMax obelisks, all open the one lobby
	int32_t                lobbyObject = 589;  // objects.txt arena_lobby (desert obelisk copy, OperateFn 16)
	int32_t                lobbyRange  = 20;   // subtiles: farther from the obelisk closes the lobby window
};

Config g_config;

auto Trim(const std::string& s) -> std::string {
	const size_t b = s.find_first_not_of(" \t\r");
	if (b == std::string::npos) {
		return {};
	}
	const size_t e = s.find_last_not_of(" \t\r");
	return s.substr(b, e - b + 1);
}
auto Ints(std::string v) -> std::vector<int32_t> {
	std::vector<int32_t> out;
	if (v.size() < 2 || v.front() != '[' || v.back() != ']') {
		return out;
	}
	v = v.substr(1, v.size() - 2);
	size_t p = 0;
	while (p < v.size()) {
		const size_t      comma = v.find(',', p);
		const std::string item  = Trim(v.substr(p, comma == std::string::npos ? std::string::npos : comma - p));
		if (!item.empty()) {
			out.push_back(static_cast<int32_t>(std::strtol(item.c_str(), nullptr, 10)));
		}
		p = comma == std::string::npos ? v.size() : comma + 1;
	}
	return out;
}
auto Spots(const std::vector<int32_t>& v) -> std::vector<Spot> {
	std::vector<Spot> out;
	for (size_t i = 0; i + 1 < v.size(); i += 2) {
		out.push_back({ v[i], v[i + 1] });
	}
	return out;
}

void ParsePvpLine(const std::string& section, const std::string& key, const std::string& val);

// [arena] enabled, message_color, sealed_message; [arena.level.<id>] gates = [x, y, ...], trapdoors = [x, y, ...]
void ParseConfig(const char* toml, Config& out) {
	if (toml == nullptr) {
		return;
	}
	const std::string text(toml);
	std::string       section;
	size_t            pos = 0;
	while (pos < text.size()) {
		const size_t nl   = text.find('\n', pos);
		std::string  line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
		pos               = nl == std::string::npos ? text.size() : nl + 1;
		bool   inString   = false;
		for (size_t i = 0; i < line.size(); ++i) { // strip comments outside "..."
			if (line[i] == '"') {
				inString = !inString;
			} else if (line[i] == '#' && !inString) {
				line.resize(i);
				break;
			}
		}
		line = Trim(line);
		if (line.empty()) {
			continue;
		}
		if (line.front() == '[' && line.back() == ']') {
			section = Trim(line.substr(1, line.size() - 2));
			continue;
		}
		const size_t eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		const std::string key = Trim(line.substr(0, eq));
		const std::string val = Trim(line.substr(eq + 1));
		ParsePvpLine(section, key, val);
		if (section == "arena") {
			if (key == "enabled") {
				out.enabled = val == "true" || val == "1";
			} else if (key == "message_color") {
				out.messageColor = static_cast<uint8_t>(std::strtoul(val.c_str(), nullptr, 10));
			} else if (key == "sealed_message" && val.size() >= 2 && val.front() == '"' && val.back() == '"') {
				out.sealedText = val.substr(1, val.size() - 2);
			} else if (key == "message_scope") {
				out.messageToAll = val != "\"player\"";
			} else if (key == "refusal_sound") {
				out.refusalSound = static_cast<int32_t>(std::strtol(val.c_str(), nullptr, 10));
			} else if (key == "sender_name" && val.size() >= 2 && val.front() == '"' && val.back() == '"') {
				out.senderName = val.substr(1, val.size() - 2);
			}
		} else if (section.rfind("arena.level.", 0) == 0) {
			const int32_t level = static_cast<int32_t>(std::strtol(section.c_str() + 12, nullptr, 10));
			if (level <= 0) {
				continue;
			}
			if (key == "gates") {
				out.arenas[level].gates = Spots(Ints(val));
			} else if (key == "trapdoors") {
				out.arenas[level].trapdoors = Spots(Ints(val));
			} else if (key == "exit") {
				out.arenas[level].exits = Spots(Ints(val));
			}
		} else if (section == "arena.lobby") {
			if (key == "spot") {
				const auto v = Ints(val); // x, y pairs (1.9.3: up to 4 obelisks)
				out.lobbySpots.clear();
				for (size_t i = 0; i + 1 < v.size() && out.lobbySpots.size() < 4; i += 2) {
					if (v[i] != 0 && v[i + 1] != 0) {
						out.lobbySpots.emplace_back(v[i], v[i + 1]);
					}
				}
			} else if (key == "object") {
				out.lobbyObject = static_cast<int32_t>(std::strtol(val.c_str(), nullptr, 10));
			} else if (key == "walk_away") {
				out.lobbyRange = std::clamp(static_cast<int32_t>(std::strtol(val.c_str(), nullptr, 10)), 5, 80);
			}
		} else if (section == "arena.entrance") {
			if (key == "level") {
				out.entranceLevel = static_cast<int32_t>(std::strtol(val.c_str(), nullptr, 10));
			} else if (key == "trapdoors") {
				const auto v = Ints(val);
				out.entrances.clear();
				for (size_t i = 0; i + 4 <= v.size(); i += 4) {
					out.entrances.push_back({ v[i], v[i + 1], v[i + 2], v[i + 3] });
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Game helpers.
// ---------------------------------------------------------------------------
// The arena level an object stands in (nullptr = not an arena) and the level's origin in subtiles.
auto ArenaOf(void* unit, int32_t* level, int32_t* originX, int32_t* originY) -> const Arena* {
	void* room = unit != nullptr ? g_getRoom(unit) : nullptr;
	if (room == nullptr) {
		return nullptr;
	}
	*level       = g_levelId(room);
	const auto it = g_config.arenas.find(*level);
	if (it == g_config.arenas.end()) {
		return nullptr;
	}
	void* drlgRoom = Field<void*>(room, kRoomDrlgRoom);
	void* lvl      = drlgRoom != nullptr ? Field<void*>(drlgRoom, kDrlgRoomLevel) : nullptr;
	if (lvl == nullptr) {
		return nullptr;
	}
	*originX = Field<int32_t>(lvl, kLevelPos) * 5;
	*originY = Field<int32_t>(lvl, kLevelPos + 4) * 5;
	return &it->second;
}

// Index of the spot nearest to the object (the one that was clicked).
auto Nearest(const std::vector<Spot>& spots, void* object, int32_t ox, int32_t oy) -> size_t {
	int32_t xy[2] {};
	g_getCoords(object, xy);
	size_t  best  = 0;
	int64_t bestD = INT64_MAX;
	for (size_t i = 0; i < spots.size(); ++i) {
		const int64_t dx = xy[0] - (ox + spots[i].x), dy = xy[1] - (oy + spots[i].y);
		if (dx * dx + dy * dy < bestD) {
			bestD = dx * dx + dy * dy, best = i;
		}
	}
	return best;
}

auto ActOf(void* game, int32_t level) -> void* {
	const uint8_t  act   = g_getAct(Field<uint8_t>(game, kGameTableSet), level);
	auto* const*   acts  = Field<void* const*>(game, kGameActs);
	const uint64_t count = Field<uint64_t>(game, kGameActs + 8);
	return acts != nullptr && act < count ? acts[act] : nullptr;
}

// The active room holding subtile x, y (streamed in when stream is set and it isn't active yet).
auto RoomAt(void* game, int32_t level, int32_t x, int32_t y, bool stream) -> void* {
	void* act = ActOf(game, level);
	if (act == nullptr) {
		return nullptr;
	}
	if (void* room = g_findRoomAt(act, x, y); room != nullptr || !stream) {
		return room;
	}
	(void)g_streamRoom(Field<uint8_t>(game, kGameTableSet), act, static_cast<uint32_t>(x / 5), static_cast<uint32_t>(y / 5));
	return g_findRoomAt(act, x, y);
}

// Move the player next to subtile x, y of the level, the way OperateFunction27 moves to the other pad.
auto Teleport(void* game, void* player, int32_t level, int32_t x, int32_t y) -> bool {
	void* room = RoomAt(game, level, x, y, true);
	if (room == nullptr) {
		Log("arena: no room at %d,%d in level %d; no teleport.", x, y, level);
		return false;
	}
	int32_t xy[2] { x, y };
	if (g_freeCoords(room, xy, kArrivalSize, kFreeMask, 0) == nullptr) {
		Log("arena: no free spot near %d,%d in level %d; no teleport.", x, y, level);
		return false;
	}
	const int32_t rx = Field<int32_t>(room, kRoomSubtiles), ry = Field<int32_t>(room, kRoomSubtiles + 4);
	if (xy[0] < rx || xy[0] >= rx + Field<int32_t>(room, kRoomSubtiles + 8) || xy[1] < ry || xy[1] >= ry + Field<int32_t>(room, kRoomSubtiles + 12)) {
		if (void* other = RoomAt(game, level, xy[0], xy[1], false); other != nullptr) {
			room = other;
		}
	}
	if (g_movePlayer(game, player, room, xy[0], xy[1], 0, 0) == 0) {
		Log("arena: the move to %d,%d in level %d failed.", xy[0], xy[1], level);
		return false;
	}
	int32_t coords[8] {};
	g_roomCoords(room, coords);
	if (void* client = g_client(player); client != nullptr) {
		g_roomInSight(client, static_cast<uint32_t>(g_levelId(room)), static_cast<uint16_t>(coords[4]), static_cast<uint16_t>(coords[5]));
	}
	g_setTeleported(player, 1);
	return true;
}

// The chat line: packet 0x26 in the shape D2RCore's client filter puts into the game chat (FUN_C0DE539F240, called
// by DisplayServerChatMessage 0xC0DE58260D0 from the client 0x26 handler FUN_14012C190): +1 type 1, +3 unit type 0
// (player), +0x0A sender name, +0x47 text, +8 colour -> chat line "name: text". Tested shapes that don't show in
// chat (G4, 2026-10-31): type 1 + unit type 2 goes to the game's own display (nothing shown); type 0xD2 goes to the
// D2RLoader console (AppendConsoleMessage).
void SendLine(void* client, uint32_t guid, const std::string& text) {
	std::array<uint8_t, 0x148> packet {};
	packet[0] = 0x26;
	packet[1] = 1;
	packet[3] = 0;
	std::memcpy(packet.data() + 4, &guid, 4);
	packet[8] = g_config.messageColor;
	std::memcpy(packet.data() + 0x0A, g_config.senderName.data(), std::min<size_t>(g_config.senderName.size(), 0x3B));
	std::memcpy(packet.data() + 0x47, text.data(), std::min<size_t>(text.size(), 0xFE));
	g_serverMessage(client, packet.data());
}

// The refusal: chat line (to the player or every player of the game, message_scope) and the player's voice line.
// Clients: game +0x138 first client, client +0x628 next (D2RCore HandleClientChatMessage 0xC0DE5825560).
void Refuse(void* game, void* player, const std::string& text) {
	const uint32_t guid = Field<uint32_t>(player, 0x08);
	if (!text.empty()) {
		if (g_config.messageToAll) {
			int sent = 0;
			for (void* c = Field<void*>(game, kGameClients); c != nullptr && sent < 64; c = Field<void*>(c, kClientNext), ++sent) {
				SendLine(c, guid, text);
			}
		} else if (void* client = g_client(player); client != nullptr) {
			SendLine(client, guid, text);
		}
	}
	if (g_config.refusalSound > 0) {
		g_attachSound(player, g_config.refusalSound, nullptr);
	}
	Log("arena: refused player %u: %s", guid, text.c_str());
}

// The object of `cls` standing on subtile x, y (its room or an adjacent one), if that room is active.
auto ObjectAt(void* game, int32_t level, int32_t cls, int32_t x, int32_t y) -> void* {
	void* room = RoomAt(game, level, x, y, false);
	if (room == nullptr) {
		return nullptr;
	}
	auto scan = [&](void* r) -> void* {
		for (void* u = r != nullptr ? Field<void*>(r, kRoomFirstUnit) : nullptr; u != nullptr; u = Field<void*>(u, kUnitRoomNext)) {
			if (Field<uint32_t>(u, 0x00) != kUnitObject || Field<int32_t>(u, 0x04) != cls) {
				continue;
			}
			int32_t xy[2] {};
			g_getCoords(u, xy);
			if (std::abs(xy[0] - x) <= 2 && std::abs(xy[1] - y) <= 2) {
				return u;
			}
		}
		return nullptr;
	};
	if (void* u = scan(room); u != nullptr) {
		return u;
	}
	void* const*  list  = Field<void* const*>(room, kRoomAdjacent);
	const int32_t count = Field<int32_t>(room, kRoomAdjCount);
	for (int32_t i = 0; list != nullptr && i < count; ++i) {
		if (void* u = scan(list[i]); u != nullptr) {
			return u;
		}
	}
	return nullptr;
}

// Trapdoors opened in the current game (level id * 256 + spot index). An opened trapdoor whose room is unloaded and
// built again could come back closed; this keeps it open for the pair check.
std::mutex                         g_stateLock;
std::map<void*, std::set<int32_t>> g_opened;

void MarkOpened(void* game, int32_t level, size_t index) {
	std::lock_guard lock(g_stateLock);
	g_opened[game].insert(level * 256 + static_cast<int32_t>(index));
}
auto WasOpened(void* game, int32_t level, size_t index) -> bool {
	std::lock_guard lock(g_stateLock);
	const auto      it = g_opened.find(game);
	return it != g_opened.end() && it->second.count(level * 256 + static_cast<int32_t>(index)) != 0;
}

std::mt19937& Rng() {
	static std::mt19937 rng { std::random_device {}() };
	return rng;
}

// ---------------------------------------------------------------------------
// Operate wrappers (game thread).
// ---------------------------------------------------------------------------
auto __fastcall OperateGate(uint8_t* op, int32_t nOperateFn) -> int32_t {
	void*   game   = Field<void*>(op, kOpGame);
	void*   object = Field<void*>(op, kOpObject);
	void*   player = Field<void*>(op, kOpPlayer);
	int32_t level = 0, ox = 0, oy = 0;
	const Arena* arena = ArenaOf(object, &level, &ox, &oy);
	if (arena == nullptr || arena->gates.size() < 2) {
		return g_origPad(op, nOperateFn);
	}
	if (player == nullptr || Field<uint32_t>(player, 0x00) != kUnitPlayer || g_getAnimMode(object) != kModeNeutral) {
		return 1;
	}
	const size_t self = Nearest(arena->gates, object, ox, oy);
	size_t       to   = std::uniform_int_distribution<size_t>(0, arena->gates.size() - 2)(Rng());
	if (to >= self) {
		++to;
	}
	if (Teleport(game, player, level, ox + arena->gates[to].x, oy + arena->gates[to].y)) {
		g_gateJumps.fetch_add(1, std::memory_order_relaxed);
	} else {
		g_failed.fetch_add(1, std::memory_order_relaxed);
	}
	return 1;
}

void EnterArena(void* game, void* player, int32_t arena, int spot = -1); // PvP section (spawn spots; -1 = random)
auto PickRespawnSpot(void* game, void* unit, int32_t level, int32_t ox, int32_t oy, Spot* out) -> bool; // 2.3.0, PvP section
auto ArenaHasDuel(int32_t level) -> bool;                                 // duel section
auto DuelJoinable(int32_t level) -> bool;                                 // duel section
auto RefuseWalkIn(void* game, void* player, int32_t level) -> bool;       // duel section
void RefuseDuelEntry(void* game, void* player);                           // duel section
void QueueLobbyOpen(uint32_t guid);                                       // 1.9.0 lobby section
void LeaveArena(void* game, void* player, int32_t arena);
auto IsDownPlayer(void* player) -> bool;

// Index of the configured spot within 2 sub-tiles of the object, or -1.
auto SpotIndex(const std::vector<Spot>& spots, void* object, int32_t ox, int32_t oy) -> int {
	int32_t xy[2] {};
	g_getCoords(object, xy);
	for (size_t i = 0; i < spots.size(); ++i) {
		if (std::abs(ox + spots[i].x - xy[0]) <= 2 && std::abs(oy + spots[i].y - xy[1]) <= 2) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

auto __fastcall OperateTrapDoor(uint8_t* op, int32_t nOperateFn) -> int32_t {
	void*      game     = Field<void*>(op, kOpGame);
	void*      object   = Field<void*>(op, kOpObject);
	void*      player   = Field<void*>(op, kOpPlayer);
	const bool isPlayer = player != nullptr && Field<uint32_t>(player, 0x00) == kUnitPlayer;
	// 1.9.0: the team duel obelisk (objects.txt arena_lobby, OperateFn 16) opens the lobby; the stock trapdoor code never runs
	if (object != nullptr && Field<uint32_t>(object, 0x00) == kUnitObject && Field<int32_t>(object, 0x04) == g_config.lobbyObject) {
		if (isPlayer) {
			QueueLobbyOpen(Field<uint32_t>(player, 0x08));
		}
		return 1;
	}
	// Entrance trapdoors (plugin-spawned in the entrance level) -> their arena; arena exit trapdoors -> back.
	if (void* room = g_getRoom(object); room != nullptr && !g_config.entrances.empty() && g_levelId(room) == g_config.entranceLevel) {
		int32_t xy[2] {};
		g_getCoords(object, xy);
		for (const Entrance& e : g_config.entrances) {
			if (std::abs(e.x - xy[0]) <= 2 && std::abs(e.y - xy[1]) <= 2) {
				if (isPlayer && ArenaHasDuel(e.arena) && !DuelJoinable(e.arena)) {
					RefuseDuelEntry(game, player); // 0.6.0: nobody walks into a running duel (1.0.0: except a walk-in one with room)
					return 1;
				}
				if (isPlayer && RefuseWalkIn(game, player, e.arena)) {
					return 1;
				}
				if (isPlayer) {
					EnterArena(game, player, e.arena);
				}
				return 1;
			}
		}
	}
	int32_t level = 0, ox = 0, oy = 0;
	const Arena* arena = ArenaOf(object, &level, &ox, &oy);
	if (arena != nullptr && !arena->exits.empty() && SpotIndex(arena->exits, object, ox, oy) >= 0) {
		if (g_getAnimMode(object) == kModeNeutral) {
			g_changeMode(object, kModeOpened); // shown open
		}
		if (isPlayer && !IsDownPlayer(player)) {
			LeaveArena(game, player, level);
		}
		return 1;
	}
	if (arena == nullptr || arena->trapdoors.empty()) {
		return g_origTrapDoor(op, nOperateFn);
	}
	const size_t  self    = Nearest(arena->trapdoors, object, ox, oy);
	const size_t  partner = self ^ 1;
	const int32_t mode    = g_getAnimMode(object);
	if (mode == kModeNeutral) { // open it (stock: mode 0 -> 2)
		g_changeMode(object, kModeOpened);
		MarkOpened(game, level, self);
		Log("arena: trapdoor %zu of level %d opened.", self, level);
		return 1;
	}
	if (mode != kModeOpened || player == nullptr || Field<uint32_t>(player, 0x00) != kUnitPlayer || partner >= arena->trapdoors.size()) {
		return 1;
	}
	const Spot& to    = arena->trapdoors[partner];
	void*       other = ObjectAt(game, level, Field<int32_t>(object, 0x04), ox + to.x, oy + to.y);
	const bool  open  = WasOpened(game, level, partner) || (other != nullptr && g_getAnimMode(other) == kModeOpened);
	if (!open) {
		Refuse(game, player, g_config.sealedText);
		g_sealed.fetch_add(1, std::memory_order_relaxed);
		return 1;
	}
	if (other != nullptr && g_getAnimMode(other) == kModeNeutral) {
		g_changeMode(other, kModeOpened); // its room was rebuilt
	}
	if (Teleport(game, player, level, ox + to.x, oy + to.y)) {
		g_trapJumps.fetch_add(1, std::memory_order_relaxed);
	} else {
		g_failed.fetch_add(1, std::memory_order_relaxed);
	}
	return 1;
}

struct Check {
	uint64_t       rva;
	const uint8_t* bytes;
	uint32_t       size;
	const char*    what;
};

// ---------------------------------------------------------------------------
// PvP (0.2.0, user 2026-10-31): inside an arena a player never really dies. The lethal blow (every player death runs
// gPlrModeChart[DEATH].pfnStartID = PLRMODE_StartID_Death) is replaced: full life/mana, frozen (STATE_UNINTERRUPTABLE:
// no new player mode is accepted) and not targetable for respawn_seconds, an on-screen countdown on that player's
// client, then a respawn at a random spawn spot of the arena. No death penalty, corpse, item drop or "Press ESC".
// Hostility: everyone in the same arena level is hostile (player-list flag 8, FRIENDLY_OpenHostility both ways)
// unless in the same party; removed again when they leave. Scores per game (arena.txt-style points), kill lines in
// chat, `arena scores`. Evidence: notes/arena.md §9, notes/arena-pvp-hostility.md.
// ---------------------------------------------------------------------------
namespace PvpRva {
inline constexpr uint64_t PlrModeChart    = 0x1d18fb0; // gPlrModeChart: 0x18-byte rows {StartXY, StartID, D2R 3rd}
inline constexpr uint64_t StartIdDeath    = 0x429d10;  // row 0 (DEATH) StartID = PLRMODE_StartID_Death
inline constexpr uint64_t StartXyNeutral  = 0x429660;  // row 1 (NEUTRAL) StartXY = PLRMODE_StartXY_Neutral
inline constexpr uint64_t StartXyMove     = 0x4297e0;  // rows 2/3 (WALK/RUN) StartXY = PLRMODE_StartXY_WalkRunKnockback; PLRMODES_ChangeMode calls [chart + mode*0x18] indirectly
inline constexpr uint64_t ReadStat        = 0x2f5020;  // int ReadWideUnitStat(unit, stat, layer) - FF 25 thunk
inline constexpr uint64_t SetStat         = 0x2f7d10;  // void SetWideUnitStat(unit, stat, value, layer) - FF 25 thunk
inline constexpr uint64_t ToggleState     = 0x3354c0;  // void STATES_ToggleState(unit, state, bSet)
inline constexpr uint64_t ServerUnit      = 0x48fe80;  // unit SUNIT_GetServerUnit(game, type, guid)
inline constexpr uint64_t OpenHostility   = 0x5e8cc0;  // FRIENDLY_OpenHostility(game, a, b): flag 8 a -> b, packet 0x8C
inline constexpr uint64_t RemoveHostility = 0x5e8c40;  // FRIENDLY_RemoveHostility(game, a, b)
inline constexpr uint64_t CheckFlag       = 0x508020;  // uint PLAYERLIST_CheckFlag(a, b, flags)
inline constexpr uint64_t PartyId         = 0x48e480;  // uint16 SUNIT_GetPartyId(unit), 0xFFFF = none
inline constexpr uint64_t LocalPlayer     = 0x9a480;   // client: unit maybe_CLIENT_GetLocalPlayer(index)
inline constexpr uint64_t LocalIndex      = 0x8b2d0;   // client: index maybe_CLIENT_GetLocalDataContext()
inline constexpr uint64_t UpdateAttribute = 0x4f3ad0;  // D2GAME_UpdateAttribute(unit, stat, value, unit, 0): sends the stat to the owner (Rcv0x41_Resurrect)
inline constexpr uint64_t SetTargetable   = 0x34ee20;  // SUNIT_SetTargetableFlag(unit, bSet)
inline constexpr uint64_t ChangeMode      = 0x42d2c0;  // PLRMODES_ChangeMode(game, unit, skill, mode, x, y, bAllowReEnter)
inline constexpr uint64_t PlayerEvents    = 0x1d192b0; // player event table (maybe_D2GAME_EVENTS_PlayerEventDispatcher 0x14042CE30), 15 x 8
inline constexpr uint64_t StatRegen       = 0x42e600;  // [3] STATREGEN = D2GAME_EVENTS_StatRegen_6FC80F80 (life floor 0x100)
inline constexpr uint64_t EndAnim         = 0x42de40;  // [1] ENDANIM = D2MOO sub_6FC81250: mode DEATH -> corpse, pets, DEAD
inline constexpr uint64_t DeathUi         = 0x1e8eb0;  // client: closes every interface state, opens state 9 (YouDiedModal)
inline constexpr uint64_t CloseUiState    = 0xc7d30;   // client: UI_CloseInterfaceState(state, flag)
inline constexpr uint64_t GetUiState      = 0xce500;   // client: byte UI_GetUIState(state)
inline constexpr uint64_t DeadCheck       = 0xd0770;   // client, every UI update (FUN_1400C6ED0): local player SUNIT_IsDead -> show YouDiedModal
inline constexpr uint64_t OpenUiState     = 0xcd7c0;   // client: bool UI_OpenInterfaceState(state, a2, a3, a4) - state 9 also from FUN_1414FCEA0
inline constexpr uint64_t ExecEvents      = 0x44ce80;  // SUNITDMG_ExecuteEvents(game, attacker, defender, bMissile, pDamage): life loss
inline constexpr uint64_t CalcTotal       = 0x44df10;  // SUNITDMG_CalculateTotalDamage(game, attacker, defender, pDamage): dwDmgTotal
inline constexpr uint64_t RemoveStates    = 0x53c1f0;  // bool D2GAME_NPC_RemoveStates(unit): every curable state (town healer: poison, cold, freeze, curses)
inline constexpr uint64_t ResyncPos       = 0x4f37f0;  // void maybe_PLRMSG_ResyncClientPosition(unit): packet 0x15, client snaps to the server x/y
inline constexpr uint64_t RemoveDeathStats = 0x2f7ad0; // RemoveWideDeathStats(unit) thunk (FF 25): D2Common_10469, every statlist but item/basic/plrstaydeath
inline constexpr uint64_t StayDeathFlags  = 0x3356c0;  // STATES_UpdateStayDeathFlags(unit, bIsBoss): clears every non-plrstaydeath state bit
inline constexpr uint64_t RefreshPassives = 0x4388b0;  // D2GAME_RefreshPassiveSkills(unit): passive states back (stock resurrect)
inline constexpr uint64_t Unshift         = 0x33ac20;  // BOOL SKILLS_RemoveTransformStatesFromShapeshiftedUnit(unit) (act change, druid death)
inline constexpr uint64_t CheckItemType   = 0x373890;  // ITEMS_CheckItemTypeId(item, itemtype id)
inline constexpr uint64_t PrepareTable    = 0x1d3ea30; // gpPSpellPrepareTable: [3] potions, [4] antidote/thawing, [6] stamina; bool(game, unit, item)
inline constexpr uint64_t PacketTable     = 0x1d2a790; // gpPacketCallbackTable: client->server handlers, 8-byte slots
// duel "busy" checks (0.6.0; read 2026-11-01 in /d2rloader_full.dmp)
inline constexpr uint64_t NpcInteract     = 0x545ac0;  // bool maybe_PLAYER_D2R_IsInteractingWithNpc(player): PlayerData.dwNpcInteractGUID + NPC near
inline constexpr uint64_t GetInventory    = 0x34a360;  // inventory UNITS_GetInventory(unit, file, line) (maybe_PLRTRADE_D2R_CanPlayersTrade)
inline constexpr uint64_t CursorItem      = 0x388a70;  // item INVENTORY_GetCursorItem(inventory) (same caller)
inline constexpr uint64_t TradeState      = 0x4851f0;  // maybe_CLIENTS_GetTradeState: *out = client + 0x6B8 (only checked, not called)
// pets (0.7.0; read 2026-11-01 in /d2rloader_full.dmp, notes/arena-research/pets.md)
inline constexpr uint64_t KillMonster     = 0x44db20;  // void SUNITDMG_KillMonster(game, defender, attacker, bPetKill): the monster death entry
inline constexpr uint64_t MinionOwner     = 0x4a53c0;  // unit AIGENERAL_GetMinionOwner(monster)
inline constexpr uint64_t TargetUnit      = 0x48fe20;  // unit SUNIT_GetTargetUnit(game, unit)
inline constexpr uint64_t StateStatList   = 0x2f5940;  // list STATLIST_GetStatListFromUnitAndState(unit, state)
inline constexpr uint64_t ListOwnerGuid   = 0x2f6e20;  // uint32 STATLIST_GetOwnerGUID(list)
inline constexpr uint64_t ListOwnerType   = 0x2f6e30;  // uint32 STATLIST_GetOwnerType(list)
inline constexpr uint64_t IteratePets     = 0x4ff990;  // void PLAYERPETS_IteratePets(game, player, cb(game, player, pet, arg), arg, char flag)
inline constexpr uint64_t PetType         = 0x4ff3b0;  // int PLAYERPETS_GetPetTypeFromPetGUID(player, petGuid): pettype.txt row, 0 = not a pet
inline constexpr uint64_t WarpPet         = 0x596d40;  // int maybe_AIUTIL_WarpPetToOwner(game, owner, pet): free spot near the owner + AI idle 5
inline constexpr uint64_t IdleNeutral     = 0x4a6d10;  // void AITACTICS_IdleInNeutralMode(game, unit, nFrames): neutral + AI event in n frames
inline constexpr uint64_t SpawnMonster    = 0x492cf0;  // unit D2GAME_SpawnMonster(game, room, x, y, monstatsId, mode, a7 -1, int16 flags) (fetish shrine: mode 1, -1, 8)
inline constexpr uint64_t SrvStartTable   = 0x238e710; // gpSkillSrvStartFnTable: skills.txt srvstfunc, int(game, unit, skill, level), 0 = cancel
inline constexpr uint64_t DeleteEvent     = 0x48b890;  // void D2GAME_EVENTS_Delete(game, unit, nEventType, nCustomId): type 2 = AI think
} // namespace PvpRva
namespace PvpExpected {
inline constexpr uint8_t kReadStat[]    = { 0xFF, 0x25, 0xF2, 0x51, 0xB3, 0x03 };
inline constexpr uint8_t kSetStat[]     = { 0xFF, 0x25, 0x32, 0x25, 0xB3, 0x03 };
inline constexpr uint8_t kToggleState[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };
inline constexpr uint8_t kServerUnit[]  = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20 };
inline constexpr uint8_t kHostility[]   = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30 };
inline constexpr uint8_t kCheckFlag[]   = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };
inline constexpr uint8_t kPartyId[]     = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9 };
inline constexpr uint8_t kLocalPlayer[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x83, 0xF9, 0x08 };
inline constexpr uint8_t kLocalIndex[]  = { 0x8B, 0x05, 0x2E, 0x84, 0x99, 0x02, 0xC3 };
inline constexpr uint8_t kUpdateAttribute[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83 };
inline constexpr uint8_t kSetTargetable[]   = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x18 };
inline constexpr uint8_t kChangeMode[]      = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC };
// mov [rsp+8],rbx / mov [rsp+0x10],rbp / mov [rsp+0x18],rsi / push rdi: 16 bytes, no RIP-relative operand (inline hook)
inline constexpr uint8_t kDeathUi[]      = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };
inline constexpr uint8_t kCloseUiState[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56 };
inline constexpr uint8_t kGetUiState[]   = { 0x48, 0x63, 0xC1, 0x48, 0x8D, 0x0D, 0x96, 0xC8, 0x95, 0x02, 0x0F, 0xB6 };
// mov [rsp+0x18],rsi / push rbp / lea rbp,[rsp-0x220]: 14 bytes, no RIP-relative operand (inline hook)
inline constexpr uint8_t kDeadCheck[]    = { 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0xE0, 0xFD, 0xFF, 0xFF };
// mov [rsp+0x18],rbx / push rbp,rsi,r12,r14,r15 / lea rbp,[rsp-0x2c0]: 21 bytes, no RIP-relative operand (inline hook)
inline constexpr uint8_t kOpenUiState[]  = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57,
                                             0x48, 0x8D, 0xAC, 0x24, 0x40, 0xFD, 0xFF, 0xFF };
// push rbp,rbx,rsi,rdi,r14,r15 / lea rbp,[rsp-0x118]: 17 bytes, no RIP-relative operand (inline hook)
inline constexpr uint8_t kExecEvents[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xE8, 0xFE, 0xFF, 0xFF };
// push rbp,rbx,rsi,rdi,r12,r13,r14,r15 / lea rbp,[rsp-0x298]: 21 bytes, no RIP-relative operand (inline hook)
inline constexpr uint8_t kCalcTotal[]  = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                                           0x48, 0x8D, 0xAC, 0x24, 0x68, 0xFD, 0xFF, 0xFF };
inline constexpr uint8_t kRemoveStates[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56 };
inline constexpr uint8_t kResyncPos[]    = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9 };
inline constexpr uint8_t kRemoveDeathStats[] = { 0xFF, 0x25, 0xBA, 0x27, 0xB3, 0x03 };
inline constexpr uint8_t kStayDeathFlags[]   = { 0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20 };
inline constexpr uint8_t kUnshift[]          = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF9 };
inline constexpr uint8_t kNpcInteract[]  = { 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9 };
inline constexpr uint8_t kGetInventory[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF1, 0x48, 0x85, 0xC9 };
inline constexpr uint8_t kCursorItem[]   = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9 };
// lea rax,[rcx+0x6B8] / mov [rdx],rax / ret: the trade state offset used by BusyReason
inline constexpr uint8_t kTradeState[]   = { 0x48, 0x8D, 0x81, 0xB8, 0x06, 0x00, 0x00, 0x48, 0x89, 0x02, 0xC3 };
inline constexpr uint8_t kCheckItemType[]    = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };
// mov [rsp+8],rbx / mov [rsp+0x10],rbp / mov [rsp+0x18],rsi / push rdi: 16 bytes, no RIP-relative operand (inline hook)
inline constexpr uint8_t kKillMonster[]  = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };
inline constexpr uint8_t kMinionOwner[]  = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };
inline constexpr uint8_t kTargetUnit[]    = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA };
inline constexpr uint8_t kStateStatList[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xDA };
inline constexpr uint8_t kListOwnerGuid[] = { 0x48, 0x85, 0xC9, 0x75, 0x03, 0x33, 0xC0, 0xC3, 0x8B, 0x41, 0x0C, 0xC3 };
inline constexpr uint8_t kListOwnerType[] = { 0xB8, 0x06, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x03 };
inline constexpr uint8_t kIteratePets[]  = { 0x40, 0x55, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 };
inline constexpr uint8_t kPetType[]      = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xF2, 0x48, 0x8B, 0xF9 };
inline constexpr uint8_t kWarpPet[]      = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
inline constexpr uint8_t kIdleNeutral[]  = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x45, 0x85, 0xC0 };
// mov r11,rsp / sub rsp,0x68 / mov eax,[rsp+0x90]
inline constexpr uint8_t kSpawnMonster[] = { 0x4C, 0x8B, 0xDC, 0x48, 0x83, 0xEC, 0x68, 0x8B, 0x84, 0x24, 0x90, 0x00, 0x00, 0x00 };
// gpSkillSrvStartFnTable slots of the corpse skills other than Raise Skeleton / Mage (stock pointers, read 2026-11-02):
// 17 Corpse/Poison Explosion + Engorge, 21 Revive, 33 Find Potion + Grim Ward, 34 Find Item
inline constexpr uint64_t kCorpseSlots[][2] = { { 17, 0x55ffb0 }, { 21, 0x560470 }, { 33, 0x569bb0 }, { 34, 0x569c00 } };
inline constexpr uint8_t kDeleteEvent[]  = { 0x40, 0x53, 0x56, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x45, 0x8B, 0xF1 };
// gpPSpellPrepareTable slots (stock pointers, read 2026-11-01)
inline constexpr uint64_t kPrepareSlots[][2] = { { 3, 0x580a80 }, { 4, 0x580d20 }, { 6, 0x580c10 } };
// gpPacketCallbackTable slots 0x01..0x12 and 0x42 (stock handlers, read 2026-11-01)
inline constexpr uint64_t kPacketSlots[][2] = {
	{ 0x01, 0x4ac050 }, { 0x02, 0x4ace20 }, { 0x03, 0x4ace40 }, { 0x04, 0x4ace60 }, { 0x05, 0x4ace80 }, { 0x06, 0x4acf80 },
	{ 0x07, 0x4ad030 }, { 0x08, 0x4ad0e0 }, { 0x09, 0x4ad100 }, { 0x0A, 0x4ad120 }, { 0x0B, 0x4ad140 }, { 0x0C, 0x4ad230 },
	{ 0x0D, 0x4ad330 }, { 0x0E, 0x4ad3e0 }, { 0x0F, 0x4ad490 }, { 0x10, 0x4ad4b0 }, { 0x11, 0x4ad4d0 }, { 0x12, 0x4ad4f0 },
	{ 0x42, 0x4ac070 },
};
} // namespace PvpExpected

constexpr size_t   kGameFrame        = 0x170; // D2GameStrc dwGameFrame (PLRMODE_StartID_Death, PLAYERMODE_Change)
constexpr size_t   kClientUnitType   = 0x26C; // D2ClientStrc unit type / GUID of its player
constexpr size_t   kClientUnitGuid   = 0x270;
constexpr size_t   kClientName       = 0x2C;  // CLIENTS_GetName 0x140484F90: lea rax,[rcx+0x2c]
constexpr size_t   kClientTradeState = 0x6B8; // maybe_CLIENTS_GetTradeState: +0 trade id (0 = none; CancelTrade returns at once), +8 partner
constexpr size_t   kUnitFlags        = 0x124; // UNITS_SetFlag 0x14034E190
constexpr uint32_t kFlagTargetable   = 0x2 | 0x4; // D2MOO UNITFLAG_TARGETABLE | UNITFLAG_CANBEATTACKED
constexpr int32_t  kStateUninterrupt = 0x36;
constexpr uint32_t kHostileFlag      = 8;
constexpr int32_t  kFramesPerSecond  = 25;
constexpr int32_t  kPlrModeDeath     = 0;    // falling animation; the client's "You have died" modal needs 0x11 DEAD
constexpr int32_t  kPlrModeNeutral   = 1;
constexpr int32_t  kPlrModeDead      = 0x11;
constexpr uint32_t kUiStateInventory = 1;    // D2R interface state of the inventory: 0 -> 1 when the left-slot countdown opened it (probe, log 2026-11-01 10:19:45)
constexpr uint32_t kUiStateYouDied   = 9;    // client interface state of YouDiedModal (FUN_1400D00B0)

using StartIdFn     = void(__fastcall*)(void* game, void* unit, int32_t mode, void* target);
using StartXyFn     = void(__fastcall*)(void* game, void* unit, int32_t mode, int32_t x, int32_t y);
using ReadStatFn    = int32_t(__fastcall*)(void* unit, int32_t stat, int32_t layer);
using SetStatFn     = void(__fastcall*)(void* unit, int32_t stat, int32_t value, int32_t layer);
using ToggleStateFn = void(__fastcall*)(void* unit, int32_t state, int32_t bSet);
using ServerUnitFn  = void*(__fastcall*)(void* game, int32_t type, uint32_t guid);
using HostilityFn   = uint64_t(__fastcall*)(void* game, void* a, void* b);
using CheckFlagFn   = uint32_t(__fastcall*)(void* a, void* b, uint32_t flags);
using PartyIdFn     = uint64_t(__fastcall*)(void* unit);
using LocalPlayerFn = void*(__fastcall*)(uint32_t index);
using LocalIndexFn  = uint32_t(__fastcall*)();
using UpdateAttrFn  = void(__fastcall*)(void* unit, int32_t stat, uint32_t value, void* unit2, int32_t a5);
using TargetableFn  = void(__fastcall*)(void* unit, int32_t bSet);
using ChangeModeFn  = uint64_t(__fastcall*)(void* game, void* unit, void* skill, int32_t mode, int32_t x, int32_t y, int32_t bAllowReEnter);
using PlrEventFn    = void(__fastcall*)(void* game, void* unit, uint64_t a3, uint64_t a4, uint64_t a5);
using DeathUiFn     = void(__fastcall*)(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
using CloseUiFn     = void(__fastcall*)(uint32_t state, uint32_t flag, uint64_t a3, uint64_t a4);
using GetUiFn       = uint8_t(__fastcall*)(int32_t state);
using OpenUiFn      = uint64_t(__fastcall*)(uint32_t state, uint64_t a2, uint64_t a3, uint64_t a4);
using UnitFn        = uint64_t(__fastcall*)(void* unit);
UnitFn        g_removeStates  = nullptr;
UnitFn        g_resync        = nullptr;
UnitFn        g_removeDeathStats = nullptr;
UnitFn        g_refreshPassives  = nullptr;
UnitFn        g_unshift          = nullptr;
using StayDeathFn  = void(__fastcall*)(void* unit, int32_t bIsBoss);
using ItemTypeFn   = uint64_t(__fastcall*)(void* item, int32_t type);
StayDeathFn   g_stayDeath     = nullptr;
ItemTypeFn    g_itemType      = nullptr;
using GetInventoryFn = void*(__fastcall*)(void* unit, const char* file, int32_t line);
using CursorItemFn   = void*(__fastcall*)(void* inventory);
using NpcInteractFn = bool(__fastcall*)(void* player); // returns in AL only (SETNZ AL / XOR AL,AL, read 2026-11-02)
NpcInteractFn  g_npcInteract  = nullptr;
GetInventoryFn g_getInventory = nullptr;
CursorItemFn   g_cursorItem   = nullptr;

StartIdFn     g_origDeath     = nullptr;
StartXyFn     g_origNeutral   = nullptr;
StartXyFn     g_origMove      = nullptr;
ReadStatFn    g_readStat      = nullptr;
using StateStatListFn = void*(__fastcall*)(void* unit, int32_t state);
using ListOwnerFn     = uint32_t(__fastcall*)(void* list);
using TargetUnitFn    = void*(__fastcall*)(void* game, void* unit);
TargetUnitFn    g_targetUnit    = nullptr;
StateStatListFn g_stateStatList = nullptr;
ListOwnerFn     g_listOwnerGuid = nullptr;
ListOwnerFn     g_listOwnerType = nullptr;
SetStatFn     g_setStat       = nullptr;
ToggleStateFn g_toggleState   = nullptr;
ServerUnitFn  g_serverUnit    = nullptr;
HostilityFn   g_openHostile   = nullptr;
HostilityFn   g_removeHostile = nullptr;
CheckFlagFn   g_checkFlag     = nullptr;
PartyIdFn     g_partyId       = nullptr;
LocalPlayerFn g_localPlayer   = nullptr;
LocalIndexFn  g_localIndex    = nullptr;
UpdateAttrFn  g_updateAttr    = nullptr;
TargetableFn  g_setTargetable = nullptr;
ChangeModeFn  g_changeMode2   = nullptr;
PlrEventFn    g_origEndAnim   = nullptr;
std::atomic<DeathUiFn> g_origDeathUi { nullptr };
CloseUiFn     g_closeUi       = nullptr;
GetUiFn       g_getUi         = nullptr;
std::atomic<uint32_t> g_deathUiBlocked { 0 };
std::atomic<OpenUiFn> g_origOpenUi { nullptr };
std::atomic<DeathUiFn> g_origDeadCheck { nullptr };
bool          g_pvpInstalled  = false;

const D2RL::ThreadService*  g_threads = nullptr;
const D2RL::NetworkService* g_net     = nullptr;
const D2RL::PanelService*   g_panels  = nullptr;
const D2RL::WidgetService*  g_widgets = nullptr;
D2RL::Network::ChannelHandle         g_channel = D2RL::Network::InvalidChannelHandle;

struct PvpConfig {
	bool        enabled        = true;
	double      respawnSeconds = 5.0;
	bool        killMessages   = true;
	bool        killBonus      = false; // 2.2.0: kill bonus in fights without duel rules (duels: the rule row)
	int32_t     killBonusLife  = 50;    // % of the missing life a kill refills
	int32_t     killBonusMana  = 75;    // % of the missing mana
	bool        walkInKillBonus = true; // 2.3.1 (user): kill bonus in every walk-in duel (1 v 1 and Battle Royale), toml walk_in_kill_bonus
	int32_t     respawnAvoidDeath = 25; // 2.3.0: subtiles - no respawn spot this close to where you went down
	int32_t     respawnAvoidEnemy = 20; // 2.3.0: subtiles - avoid respawn spots this close to a living enemy
	bool        hostileMessages = true;
	std::string respawnText    = "Respawning in";
	bool        lastReturn     = true;            // last player left in an arena -> HUD countdown, then back to their last act town
	double      returnSeconds  = 5.0;
	bool        traceDamage    = false;           // [arena] trace_damage: one log line per damage event on an arena player (diagnosis)
	bool        diagnostics    = false;           // 2.4.12 [arena] diagnostics: the test logs (UI probes, HUD/scoreboard state, stake trade progress)
	double      aloneSeconds   = 60.0;            // alone in an arena with no opponent -> back to town after this; 0 = stay (user 2026-11-07)
	std::string aloneText      = "Waiting for an opponent. Returning to town";
	std::string returnTitle    = "Your opponent left";
	std::string returnText     = "Returning to town in";
	bool        countdownHud   = true;            // "hud": one always-open panel, text written into the text box (buff-hud technique); "window": 0.2.15 left-slot windows
	int32_t     suicide = -1, playerKill = 3, playerKillPercent = 0, playerDeath = -1, playerDeathPercent = 0, monsterDeath = -1;
	std::map<int32_t, std::vector<Spot>> spawns; // level id -> spawn spots (DS1 subtiles)
	std::map<int32_t, std::vector<Spot>> startSpawns; // 2.1.0: [arena.level.N] start_spawns - extra spots for a duel start only
	std::map<int32_t, std::vector<Spot>> bodySpots; // level id -> body positions (arena/tools/body_spots.py)
	bool        cleanStart = true;               // strip buffs/curses/states (not passives) + full life/mana at hostility and respawn
	std::array<int32_t, 4> potionLimit { 5, 5, 2, 0 }; // healing, mana, rejuvenation, other (antidote/thawing/stamina); 0 = no limit
	double      potionCooldown = 1.0;            // seconds between two potions of the same kind
	bool        mercPotions    = false;          // mercenaries may drink in the arena
	bool        scoreboard     = true;           // top-centre line with the scores of the players in the same arena
	int32_t     scoreboardY    = 8;              // HD pixels from the top (the monster hover bar starts at 63)
	bool        scoreboardArt  = true;           // 1 v 1: the duel art (banners + medallion) instead of the line
	bool        duelEnabled       = true;        // [arena.duel]
	uint32_t    mapSightMask      = 4;           // 1.8.4: collision bits that block sight for the "in sight" automap rule (4 = missile barrier, as the stock client)
	bool        wagerEnabled      = true;        // [arena.wager] gold stakes on duels (0.9.0)
	uint32_t    wagerMax          = 0;           // 0 = no limit
	int32_t     stakeStat         = 368;         // itemstatcost rows added by arena/tools/build_data.py
	int32_t     winningsStat      = 369;
	bool        duelWalkIn        = true;        // a second player entering an arena starts a duel (user 2026-11-07)
	bool        playerMenu        = true;        // 1.2.0: clicking another player opens Trade / Duel (like an NPC menu)
	int32_t     duelMaxPlayers    = 4;           // 1.0.0: 2 + walk_in_extras: players who can be in a walk-in duel (free for all)
	int32_t     duelJoinScore     = -1;          // a player joining a walk-in duel after the fight began starts with this score
	double      duelJoinProtect   = 3.0;         // a joining player is frozen and can't be hit this long after being placed
	int32_t     duelPoints        = 9;           // first to this arena score wins (player_kill 3 -> 3 kills)
	double      duelInviteSeconds = 30.0;
	double      duelCountdown     = 5.0;         // frozen start countdown once both are in the arena
	double      duelEndSeconds    = 3.0;         // after the winning kill, then both go to town
	double      duelMoveTimeout   = 60.0;        // both must be in the arena within this (loading screens)
	double      duelCrossActExtra = 5.0;         // extra wait when a player changes act: the load signal comes before the HD assets
	bool        duelScreenCheck   = true;        // wait for the act-changing client's loading screen to close (0.8.3)
	double      duelScreenExtra   = 1.0;         // after the loading screen closed (fade out)
	double      duelScreenWait    = 5.0;         // a loading screen not seen within this: the fixed wait above instead
	std::string duelBusyText      = "A duel is going on in this arena.";
	bool        petsKeep          = true;        // [arena.pets] Iron Golem / bound demons don't die in the arena; back at the owner's respawn
	bool        petsAllGolems     = false;       // also clay / blood / fire golems
	bool        petsHoldDown      = true;        // all pets and the merc stand still while their owner is down (user 2026-11-07)
	bool        bodiesEnabled     = true;        // [arena.bodies] dead bodies in the arenas for Raise Skeleton / Skeletal Mage
	int32_t     bodiesCount       = 25;          // per arena (user 2026-11-07)
	int32_t     bodiesMonster     = 0;           // monstats.txt row (0 = skeleton1)
	double      bodiesRespawn     = 10.0;        // seconds until a used body is replaced
	std::string bodiesRefuse      = "Only skeletons and skeletal mages can be raised in the arena.";
};
PvpConfig g_pvp;

void ParsePvpLine(const std::string& section, const std::string& key, const std::string& val) {
	const int32_t num = static_cast<int32_t>(std::strtol(val.c_str(), nullptr, 10));
	if (section == "arena.potions") {
		static constexpr const char* kKinds[] = { "healing", "mana", "rejuvenation", "other" };
		for (int k = 0; k < 4; ++k) {
			if (key == kKinds[k]) {
				g_pvp.potionLimit[k] = std::max(0, num);
			}
		}
		if (key == "cooldown_seconds") {
			g_pvp.potionCooldown = std::clamp(std::strtod(val.c_str(), nullptr), 0.0, 60.0);
		} else if (key == "mercenary") {
			g_pvp.mercPotions = val == "true" || val == "1";
		}
		return;
	}
	if (section == "arena.wager") {
		const long n = std::strtol(val.c_str(), nullptr, 10);
		if (key == "enabled") {
			g_pvp.wagerEnabled = val == "true" || val == "1";
		} else if (key == "max_gold") {
			g_pvp.wagerMax = static_cast<uint32_t>(std::clamp<long>(n, 0, 100000000));
		} else if (key == "stake_stat") {
			g_pvp.stakeStat = static_cast<int32_t>(std::clamp<long>(n, 1, 510));
		} else if (key == "winnings_stat") {
			g_pvp.winningsStat = static_cast<int32_t>(std::clamp<long>(n, 1, 510));
		}
		return;
	}
	if (section == "arena.bodies") {
		if (key == "enabled") {
			g_pvp.bodiesEnabled = val == "true" || val == "1";
		} else if (key == "count") {
			g_pvp.bodiesCount = std::clamp(num, 0, 100);
		} else if (key == "monster") {
			g_pvp.bodiesMonster = std::max(0, num);
		} else if (key == "respawn_seconds") {
			g_pvp.bodiesRespawn = std::clamp(std::strtod(val.c_str(), nullptr), 1.0, 600.0);
		} else if (key == "refuse_message" && val.size() >= 2 && val.front() == '"' && val.back() == '"') {
			g_pvp.bodiesRefuse = val.substr(1, val.size() - 2);
		}
		return;
	}
	if (section == "arena.pets") {
		if (key == "enabled") {
			g_pvp.petsKeep = val == "true" || val == "1";
		} else if (key == "all_golems") {
			g_pvp.petsAllGolems = val == "true" || val == "1";
		} else if (key == "hold_when_owner_down") {
			g_pvp.petsHoldDown = val == "true" || val == "1";
		}
		return;
	}
	if (section == "arena.duel") {
		const double d = std::strtod(val.c_str(), nullptr);
		if (key == "enabled") {
			g_pvp.duelEnabled = val == "true" || val == "1";
		} else if (key == "walk_in") {
			g_pvp.duelWalkIn = val == "true" || val == "1";
		} else if (key == "player_menu") {
			g_pvp.playerMenu = val == "true" || val == "1";
		} else if (key == "map_sight_mask") {
			g_pvp.mapSightMask = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 0)) & 0xFFFF;
		} else if (key == "walk_in_extras") {
			g_pvp.duelMaxPlayers = 2 + std::clamp(num, 0, 2); // user 2026-11-07: at most 2 extra players, 0 = nobody joins
		} else if (key == "join_score") {
			g_pvp.duelJoinScore = num;
		} else if (key == "join_protect_seconds") {
			g_pvp.duelJoinProtect = std::clamp(d, 0.0, 30.0);
		} else if (key == "points") {
			g_pvp.duelPoints = std::max(1, num);
		} else if (key == "invite_seconds") {
			g_pvp.duelInviteSeconds = std::clamp(d, 5.0, 300.0);
		} else if (key == "countdown_seconds") {
			g_pvp.duelCountdown = std::clamp(d, 0.0, 30.0);
		} else if (key == "end_seconds") {
			g_pvp.duelEndSeconds = std::clamp(d, 0.0, 30.0);
		} else if (key == "arrive_seconds") {
			g_pvp.duelMoveTimeout = std::clamp(d, 10.0, 600.0);
		} else if (key == "cross_act_extra_seconds") {
			g_pvp.duelCrossActExtra = std::clamp(d, 0.0, 60.0);
		} else if (key == "loading_screen_check") {
			g_pvp.duelScreenCheck = val == "true" || val == "1";
		} else if (key == "loading_screen_extra_seconds") {
			g_pvp.duelScreenExtra = std::clamp(d, 0.0, 30.0);
		} else if (key == "loading_screen_wait_seconds") {
			g_pvp.duelScreenWait = std::clamp(d, 1.0, 60.0);
		} else if (key == "busy_message" && val.size() >= 2 && val.front() == '"' && val.back() == '"') {
			g_pvp.duelBusyText = val.substr(1, val.size() - 2);
		}
		return;
	}
	if (section == "arena") {
		if (key == "pvp") {
			g_pvp.enabled = val == "true" || val == "1";
		} else if (key == "respawn_seconds") {
			g_pvp.respawnSeconds = std::clamp(std::strtod(val.c_str(), nullptr), 0.0, 60.0);
		} else if (key == "kill_messages") {
			g_pvp.killMessages = val == "true" || val == "1";
		} else if (key == "kill_bonus") {
			g_pvp.killBonus = val == "true" || val == "1";
		} else if (key == "walk_in_kill_bonus" || key == "royale_kill_bonus") { // 2.3.0 name kept as an alias
			g_pvp.walkInKillBonus = val == "true" || val == "1";
		} else if (key == "respawn_avoid_death") {
			g_pvp.respawnAvoidDeath = std::clamp(static_cast<int32_t>(std::strtol(val.c_str(), nullptr, 10)), 0, 200);
		} else if (key == "respawn_avoid_enemy") {
			g_pvp.respawnAvoidEnemy = std::clamp(static_cast<int32_t>(std::strtol(val.c_str(), nullptr, 10)), 0, 200);
		} else if (key == "kill_bonus_life") {
			g_pvp.killBonusLife = std::clamp(static_cast<int32_t>(std::strtol(val.c_str(), nullptr, 10)), 0, 100);
		} else if (key == "kill_bonus_mana") {
			g_pvp.killBonusMana = std::clamp(static_cast<int32_t>(std::strtol(val.c_str(), nullptr, 10)), 0, 100);
		} else if (key == "clean_start") {
			g_pvp.cleanStart = val == "true" || val == "1";
		} else if (key == "hostility_messages") {
			g_pvp.hostileMessages = val == "true" || val == "1";
		} else if (key == "last_player_return") {
			g_pvp.lastReturn = val == "true" || val == "1";
		} else if (key == "return_seconds") {
			g_pvp.returnSeconds = std::clamp(std::strtod(val.c_str(), nullptr), 0.0, 60.0);
		} else if (key == "trace_damage") {
			g_pvp.traceDamage = val == "true" || val == "1";
		} else if (key == "diagnostics") {
			g_pvp.diagnostics = val == "true" || val == "1";
		} else if (key == "alone_seconds") {
			g_pvp.aloneSeconds = std::clamp(std::strtod(val.c_str(), nullptr), 0.0, 3600.0);
		} else if (key == "alone_text" && val.size() >= 2 && val.front() == '"' && val.back() == '"') {
			g_pvp.aloneText = val.substr(1, val.size() - 2);
		} else if (key == "return_title" && val.size() >= 2 && val.front() == '"' && val.back() == '"') {
			g_pvp.returnTitle = val.substr(1, val.size() - 2);
		} else if (key == "return_text" && val.size() >= 2 && val.front() == '"' && val.back() == '"') {
			g_pvp.returnText = val.substr(1, val.size() - 2);
		} else if (key == "scoreboard") {
			g_pvp.scoreboard = val == "true" || val == "1";
		} else if (key == "scoreboard_art") {
			g_pvp.scoreboardArt = val == "true" || val == "1";
		} else if (key == "scoreboard_y") {
			g_pvp.scoreboardY = std::clamp(num, 0, 2000);
		} else if (key == "countdown_style") {
			g_pvp.countdownHud = val != "\"window\"";
		} else if (key == "respawn_text" && val.size() >= 2 && val.front() == '"' && val.back() == '"') {
			g_pvp.respawnText = val.substr(1, val.size() - 2);
		}
	} else if (section == "arena.score") {
		if (key == "suicide") g_pvp.suicide = num;
		else if (key == "player_kill") g_pvp.playerKill = num;
		else if (key == "player_kill_percent") g_pvp.playerKillPercent = num;
		else if (key == "player_death") g_pvp.playerDeath = num;
		else if (key == "player_death_percent") g_pvp.playerDeathPercent = num;
		else if (key == "monster_death") g_pvp.monsterDeath = num;
	} else if (section.rfind("arena.level.", 0) == 0 && key == "start_spawns") { // 2.1.0
		const int32_t level = static_cast<int32_t>(std::strtol(section.c_str() + 12, nullptr, 10));
		if (level > 0) {
			g_pvp.startSpawns[level] = Spots(Ints(val));
		}
	} else if (section.rfind("arena.level.", 0) == 0 && key == "spawns") {
		const int32_t level = static_cast<int32_t>(std::strtol(section.c_str() + 12, nullptr, 10));
		if (level > 0) {
			g_pvp.spawns[level] = Spots(Ints(val));
		}
	} else if (section.rfind("arena.level.", 0) == 0 && key == "bodies") {
		const int32_t level = static_cast<int32_t>(std::strtol(section.c_str() + 12, nullptr, 10));
		if (level > 0) {
			g_pvp.bodySpots[level] = Spots(Ints(val));
		}
	}
}

// Game-thread state of the current game. A different game pointer starts a new set (scores are per game).
struct Down {
	uint32_t guid;
	int32_t  due; // game frame
};
struct HeldPet {
	uint32_t pet;   // monster guid
	uint32_t owner; // player guid
	int32_t  level;
	bool     keepLife = false; // held because its owner is down, not "killed": no heal on release
};
struct BodySlot {
	int32_t  x, y;     // level position (sub-tiles), fixed for the game
	uint32_t guid = 0; // the dead-mode monster standing in for a corpse, 0 = none now
	int32_t  due  = 0; // frame from which a new one may be placed
};
struct ArenaBodies {
	std::vector<BodySlot> slots;
};
std::map<int32_t, ArenaBodies> g_bodies; // level -> bodies (game thread)
std::vector<HeldPet> g_heldPets; // pets "down" in an arena, back at the owner's respawn (g_pvpLock)
// 2.2.1 (user 2026-11-08): held pets' auras must not work. The aura gate (ArenaAuraDo) reads this snapshot without
// g_pvpLock (the aura tick can run while game code we called holds it); PublishHeldLocked() refreshes it after every change.
constexpr size_t                          kHeldSnapMax = 64;
std::array<std::atomic<uint32_t>, kHeldSnapMax> g_heldSnap {};
std::atomic<size_t>                       g_heldSnapN { 0 };
void PublishHeldLocked() { // caller holds g_pvpLock
	const size_t n = std::min(g_heldPets.size(), kHeldSnapMax);
	for (size_t i = 0; i < n; ++i) {
		g_heldSnap[i].store(g_heldPets[i].pet, std::memory_order_relaxed);
	}
	g_heldSnapN.store(n, std::memory_order_release);
}
auto IsHeldPetGuid(uint32_t guid) -> bool {
	const size_t n = g_heldSnapN.load(std::memory_order_acquire);
	for (size_t i = 0; i < n; ++i) {
		if (g_heldSnap[i].load(std::memory_order_relaxed) == guid) {
			return true;
		}
	}
	return false;
}
std::atomic<int32_t> g_heldCount { 0 };
struct PlayerScore {
	std::string name;
	int32_t     score = 0, kills = 0, deaths = 0;
};
std::mutex                             g_pvpLock;
void*                                  g_game = nullptr;
std::vector<Down>                      g_down;
std::atomic<int32_t>                   g_downCount { 0 }; // g_down.size(), read without the lock by the damage/packet hooks
std::array<std::atomic<uint32_t>, 0x43> g_packetDrops {}; // per client packet id, dropped while down (log at respawn)
std::map<uint32_t, PlayerScore>        g_scores;
std::vector<uint32_t>                  g_cleanQueue; // players to clean-start in the next tick (new hostility)
std::set<uint64_t>                     g_pairs;     // hostility the plugin opened: (low guid << 32) | high guid
std::map<uint32_t, int32_t>            g_lastLevel; // guid -> level at the last neutral
std::map<uint32_t, D2RL::Network::PeerHandle> g_peers; // host: player GUID -> its client's channel peer
std::map<uint32_t, int32_t>            g_lastTown;   // guid -> last act town level the player stood in
std::map<int32_t, int32_t>             g_arenaCount; // arena level -> players in it at the last SyncHostility
struct PendingReturn {
	uint32_t guid;
	int32_t  level;
	int32_t  due  = 0;
	bool     told = false;
	bool     duelEnd = false; // sent home by the end of a duel: goes even if another player stands in the level
};
std::vector<PendingReturn>             g_returns;    // last players left in an arena, waiting for the return to town
struct AloneWait {
	uint32_t guid;
	int32_t  due; // game frame
};
std::map<int32_t, AloneWait>           g_alone;      // g_pvpLock: arena level -> its only player, nobody to fight yet
std::atomic<int32_t>                   g_duelLoadLevel { 0 }; // UI: duel level the host asked about, 0 = none
std::atomic<uint32_t>                  g_tickEpoch { 1 };   // moves on when a session ends or starts: queued game work may have been dropped
std::atomic<uint32_t>                  g_tickQueued { 0 };  // epoch a tick is queued for, 0 = none; an old epoch no longer blocks
std::atomic<bool>                      g_anyInArena { false };
std::atomic<uint32_t>                  g_deathsHandled { 0 };

// Countdown shared with the UI thread: from the network (remote client) or from the host's own down list.
std::atomic<uint32_t> g_localDownGuid { 0 };
std::atomic<uint64_t> g_localDownUntil { 0 }; // GetTickCount64 ms
std::atomic<uint64_t> g_countdownUntil { 0 };
std::atomic<uint32_t> g_localReturnGuid { 0 };  // host process: its own player got a return-to-town countdown
std::atomic<uint64_t> g_localReturnUntil { 0 };
std::atomic<uint64_t> g_returnUntil { 0 };      // UI: return-to-town countdown end (ms), 0 = none
std::atomic<bool>     g_uiPolling { false };
std::atomic<uint32_t> g_helloGuid { 0 };
bool                  g_counting   = false; // UI thread: a countdown ran
uint32_t              g_uiPolls    = 0;     // UI thread

constexpr uint16_t kMsgHello = 1, kMsgCountdown = 2, kMsgRespawned = 3, kMsgReturn = 4; // kMsgReturn: ms, 0 = cancelled
constexpr uint16_t kMsgScore = 5;       // scoreboard line (text, "" = none) for the player's own arena
constexpr uint16_t kMsgDuelCmd = 6;       // client -> host: u8 kind + name (challenge)
constexpr uint16_t kMsgNotice = 7;        // host -> client: u32 ms (0 = clear), u8 1 = append seconds / 2 = background notice with seconds, text
constexpr uint16_t kMsgDuelCountdown = 8; // host -> client: u32 ms of the frozen start countdown (0 = stop)
constexpr uint16_t kMsgDuelLoadCheck = 9; // host -> client: i32 duel level, i32 1 = changes act (expect a loading screen); answer when loaded
constexpr uint16_t kMsgDuelLoaded = 10;   // client -> host: this client finished loading the duel level
constexpr uint16_t kMsgDuelResult = 11;   // host -> client: u8 1 = won / 2 = lost, u32 ms until the warp to town
constexpr uint16_t kMsgWagerCmd   = 12;   // client -> host (1.1.0 wager window): u8 kind (WagerCmdKind), u32 value
constexpr uint16_t kMsgWagerState = 13;   // host -> client: the wager window as this player sees it (WagerStateMsg + names + note)
constexpr uint16_t kMsgLobbyState = 19;   // host -> client (1.9.0): the team duel lobby as this player sees it (LobbyStateMsg + members + note)
constexpr uint16_t kMsgMapHide    = 18;   // host -> client (1.8.0): u8 1 = hide these players on the automap / 0 = show all again, then u32 guids
constexpr uint16_t kMsgSkillCd    = 17;   // host -> client (1.7.0): u32 skill id, u32 frames - start that cooldown on the local player
constexpr uint16_t kMsgWagerFlag  = 16;   // host -> client (1.5.0): u8 1 = the next trade with this pair is a wager (labels), 0 = no more
constexpr uint16_t kMsgStartTrade = 15;   // host -> challenger's client (1.4.0): u32 guid - send the stock trade request (a wager trade)
constexpr uint16_t kMsgTeamMates   = 21;  // host -> client (1.9.3): u8 1 = these players are teammates (party look on the automap) / 0 = none, then u32 guids
constexpr uint16_t kMsgSuddenDeath = 20;  // host -> client (1.9.3): u8 SuddenKind (0 = off) - the centred sudden death banner until the result
constexpr uint16_t kMsgCloseTrade = 22;  // host -> client (2.4.11): cancel the stock trade popup / window (the stake trade is off)
constexpr uint16_t kMsgDuelInvite = 14;   // host -> client (1.2.0): u32 ms (0 = close), u8 1 = wager window, u32 gold each, challenger name
constexpr char     kTeamMark = '\x1E';  // 2.0.0 scoreboard art message: mark total0 mark total1 (mark side+name mark score) per player
constexpr size_t   kScoreReserve = 216; // longest scoreboard line in bytes (72 x U+200B placeholder; payload max 224)
std::map<uint32_t, std::string> g_scoreSent;  // host (g_pvpLock): guid -> line its client has (delivered over the network)
std::mutex                      g_scoreLock;  // guards the two below (game, network and UI threads)
std::map<uint32_t, std::string> g_hostScore;  // host process: guid -> current line (the hosting player's own comes from here)
std::string                     g_netScore;   // client: line received from the host
std::atomic<bool>               g_scoreHidden { false }; // hotkey / `arena scoreboard`
// ---- duels (0.6.0, game thread; user 2026-11-01) ----
// Stage 1: `arena duel <name>` / `arena accept` / `arena decline` / `arena cancel` (console or Controls keys), a notice
// under the scoreboard + chat lines. On accept the host takes a free arena level (no players, no duel), warps both
// in (LEVEL_WarpUnit: any act; opposite spawn spots), freezes them for countdown_seconds once both are in (0x36 +
// untargetable + the movement/skill packet gate), then they fight; the first to reach `points` (the arena score)
// wins. Monster deaths and suicides don't change a duelist's score. Leaving the arena or the game = forfeit. At the
// end (end_seconds after the winning kill) the loser is respawned and both go to their last act town.
// notes/arena-research/match-flow.md, notes/arena.md section 27.
enum DuelCmdKind : uint8_t { kDuelChallenge = 1, kDuelAccept = 2, kDuelDecline = 3, kDuelCancel = 4, kDuelWager = 5, kDuelTrade = 6, kDuelPlainTrade = 7 };
enum WagerCmdKind : uint8_t { kWagerOffer = 1, kWagerReady = 2, kWagerClose = 3, kWagerRule = 4,
	                             kStakeTradeClosed = 5, // 2.4.10: client -> host: my stock trade popup / window closed without the exchange
	                             // 1.9.0 team duel lobby (same command message): open = clicked the obelisk (host side only), join value = team 0 / 1
	                             kLobbyOpen = 10, kLobbyJoin = 11, kLobbyReady = 12, kLobbyRule = 13, kLobbyClose = 14 }; // kWagerRule: value = row | more << 8
struct DuelCmd {
	uint32_t    guid;
	uint8_t     kind;
	std::string name;
};
struct Invite {
	uint32_t from, to;
	int32_t  expires;   // game frame
	uint32_t stake = 0; // gold each (0.9.0)
	bool     panel = false; // 1.1.0: `arena wager`: accepting opens the wager window instead of starting the duel
};
enum DuelState { kDuelMoving, kDuelCountdown, kDuelFighting, kDuelEnding };
// 1.6.0 duel rules (user 2026-11-08): set by the challenger on the Duel Rules page, both accept. Walk-ins / console
// duels use DefaultRules() (arena.toml).
enum DuelType : uint8_t { kTypeKills = 0, kTypePoints = 1, kTypeTimed = 2 };
#pragma pack(push, 1)
struct DuelRules {
	uint8_t type         = kTypePoints;
	uint8_t target       = 9;  // kills or points to win (not used by Timed)
	uint8_t killWorth    = 3;  // points per kill (Points / Timed)
	uint8_t deathPenalty = 1;  // points lost per death (Points / Timed)
	uint8_t minutes      = 0;  // Timed: length; Kills / Points: a cap, 0 = none
	uint8_t potionCd     = 1;  // seconds between two potions of a kind
	uint8_t potionLimit  = 0;  // potions of each kind per player, 0 = no limit
	uint8_t stakes       = 0;  // 1 = a stake through the trade window after the rules
	uint8_t teleCd       = 0;  // 1.7.0: Teleport cooldown in seconds (0 = stock)
	uint8_t mapShow      = 1;  // 1.8.0: 1 = the opponent shows on the automap, 0 = hidden; 1.8.3: 2-5 = shown within 10 / 20 / 30 / 40 subtiles; 1.8.4: 6 = in sight
	uint8_t killBonus    = 0;  // 2.2.0: 1 = a kill refills part of the killer's missing life / mana ([arena] kill_bonus_life / _mana)
};
#pragma pack(pop)
DuelRules DefaultRules();
struct Duel {
	uint32_t    a, b;
	std::string nameA, nameB;
	int32_t     level;
	DuelState   state;
	int32_t     due;        // frame: moving timeout / countdown end / end of the winner pause
	uint32_t    winner = 0; // 0 = no winner (cancelled)
	bool        arrived = false; // both stand in the level on the server: frozen, the countdown waits for their clients
	bool        waitNoticed = false;
	bool        walkIn = false; // came in by the trapdoor: already in the level, placed on spawn spots after loading
	bool        crossAct = false;   // a player came from another act: full loading screen
	bool        crossA = false, crossB = false;
	uint32_t    stake = 0;    // gold each, held in the stake stat (0.9.0)
	bool        wager = false; // 1.4.0: gold or items at stake (items: g_stakedItems)
	bool        paid  = false; // the wager is settled (paid, refunded or lost)
	int32_t     extraDue = 0;       // frame the cross-act extra wait ends, 0 = not started
	bool        extraStarted = false;
	// 1.0.0 (user 2026-11-07): players who walked into a running walk-in duel (free for all). a and b are the first two;
	// when one of them leaves, a joined player takes the slot.
	std::vector<std::pair<uint32_t, std::string>> joined;
	DuelRules   rules;              // 1.6.0
	bool        customRules = false; // from the rules page (potion rules override the toml)
	int32_t     endFrame    = 0;    // time limit end (0 = none)
	bool        suddenDeath = false; // time up with a tie: the next kill wins
	// 1.9.0 team duels (lobby at the obelisk in Lut Gholein): guid -> team 0 / 1, empty = not a team duel. a / b = the first
	// player of each team (logs, scoreboard sides); joined stays empty.
	std::map<uint32_t, uint8_t>     team;
	std::map<uint32_t, std::string> names;
	std::set<uint32_t>              cross;           // members coming from another act
	int32_t                         teamScore[2] {};
	std::string                     teamName[2];
};
// A joined player until they can fight: waits for its client to load, is placed on a free spawn spot, stays frozen
// (and can't be hit) for join_protect_seconds, or until the duel's own countdown ends.
struct JoinWait {
	uint32_t guid;
	int32_t  level;
	int32_t  loadDue;        // frame: placed even without a loaded answer
	int32_t  protectDue = 0; // frame, once placed
	bool     placed     = false;
	int32_t  score      = 0; // the score it joined with (SyncHostility's new-fight reset puts it back)
};
std::vector<JoinWait> g_joinWait; // g_pvpLock
auto IsMember(const Duel& d, uint32_t guid) -> bool {
	if (!d.team.empty()) {
		return d.team.count(guid) != 0;
	}
	return d.a == guid || d.b == guid || std::any_of(d.joined.begin(), d.joined.end(), [&](const auto& j) { return j.first == guid; });
}
auto Members(const Duel& d) -> std::vector<uint32_t> {
	if (!d.team.empty()) {
		std::vector<uint32_t> t;
		for (const auto& [g, side] : d.team) {
			t.push_back(g);
		}
		return t;
	}
	std::vector<uint32_t> m { d.a, d.b };
	for (const auto& j : d.joined) {
		m.push_back(j.first);
	}
	return m;
}
auto MemberName(const Duel& d, uint32_t guid) -> std::string {
	if (const auto it = d.names.find(guid); it != d.names.end()) {
		return it->second;
	}
	if (guid == d.a) {
		return d.nameA;
	}
	if (guid == d.b) {
		return d.nameB;
	}
	for (const auto& j : d.joined) {
		if (j.first == guid) {
			return j.second;
		}
	}
	return std::string();
}
// 1.9.0 team duel lobby (host, g_pvpLock): the players who clicked the obelisk. members[0] leads (sets the rules).
constexpr uint8_t kNoTeam       = 255;
constexpr uint8_t kInDraw       = 2;   // 1.9.1 mixed teams: in the lobby's draw, team decided when all are ready
constexpr int     kLobbyTeamMax = 4;
struct LobbyMember {
	uint32_t    guid;
	std::string name;
	uint8_t     team  = kNoTeam;
	bool        ready = false;
};
struct Lobby {
	std::vector<LobbyMember> members;
	DuelRules                rules;
	int32_t                  agreeFrame = 0;
	bool                     mix = false;     // 1.9.1 (user 2026-11-08): teams drawn at random, as even as possible
	int32_t                  revealFrame = 0; // 1.9.1: teams drawn and shown; the duel starts at this frame (0 = not drawn)
};
Lobby g_lobby;
auto InLobbyLocked(uint32_t guid) -> bool {
	return std::any_of(g_lobby.members.begin(), g_lobby.members.end(), [&](const LobbyMember& m) { return m.guid == guid; });
}
// user 2026-11-08: 1v1 to 4v4, 1v2 and 1v3 (never 1v4 or more)
auto TeamSizesOk(int a, int b) -> bool {
	return a >= 1 && b >= 1 && a <= kLobbyTeamMax && b <= kLobbyTeamMax && (a == b || (std::min(a, b) == 1 && std::max(a, b) <= 3));
}
// 1.9.1 mixed teams: 2-8 in the draw, split as evenly as possible (an odd count gives 1v2, 2v3, 3v4)
auto MixSizesOk(int a, int b) -> bool {
	return a >= 1 && b >= 1 && a <= kLobbyTeamMax && b <= kLobbyTeamMax && std::abs(a - b) <= 1;
}
constexpr int32_t kRevealSeconds = 3; // the drawn teams show this long before the lobby closes
std::map<uint32_t, uint8_t> g_duelLoaded; // g_pvpLock, host: players whose client answered kMsgDuelLoadCheck -> 1 load signal, 2 loading screen closed
std::mutex            g_duelCmdLock;
std::vector<DuelCmd>  g_duelCmds;      // g_duelCmdLock: from consoles / the network, run by the tick
std::vector<Invite>   g_invites;       // g_pvpLock
std::vector<Duel>     g_duels;         // g_pvpLock
// 1.1.0 wager window (host): two players set their stakes like a trade, both accept, then the duel starts. Each side
// offers its own gold (the winner takes both). items is reserved for staked items (later, notes/arena-research/wager.md
// W3): always empty for now, and the messages already carry its count.
struct WagerSide {
	uint32_t              guid = 0;
	std::string           name;
	uint32_t              gold  = 0;
	bool                  ready = false;
	std::string           note; // shown to this side only (e.g. "You don't carry 5000 gold.")
	std::vector<uint32_t> items;
};
struct WagerSession {
	WagerSide side[2];
	DuelRules rules;           // 1.6.0: the Duel Rules page
	int32_t   lastFrame = 0; // 1.2.2: last open / command; closed after kWagerIdleSeconds without one
	int32_t   agreeFrame = 0; // 1.3.0: accepting works from this frame on (kWagerAgreeSeconds after the last stake change)
};
constexpr int32_t kWagerIdleSeconds  = 120;
constexpr int32_t kWagerAgreeSeconds = 2; // user 2026-11-07: no switching the stake at the last second
constexpr int32_t kWagerMaxDistance  = 0x33; // subtiles: the stock trade distance (maybe_PLRTRADE_D2R_CanPlayersTrade)
int32_t           g_wagerFrame       = 0; // game thread: frame of the running wager step (for the agree wait sent to clients)
std::vector<WagerSession> g_wagerSessions; // g_pvpLock
// 1.4.0 item stakes through the stock trade window (user 2026-11-08): the player menu's Duel marks the pair, the
// challenger's client sends the stock trade request, both put in items and gold and accept; the ExecuteExchange hook
// takes the two offers as the stakes (nothing is swapped) and the duel starts on the next tick. Items stay with their
// owners until the payout moves the loser's to the winner.
struct TradeWager {
	uint32_t  a, b;    // a = challenger
	int32_t   expires; // frame
	DuelRules rules;   // 1.6.0
	bool      tradeSeen = false; // 2.4.3: a stock trade between them was open at some point
	int32_t   started   = 0;     // 2.4.4: frame of the mark (no trade window within kStakeTradeOpenSeconds -> cancelled)
	uint64_t  startedMs = 0;     // 2.4.10: wall clock of the mark (the frame timer ran 30-45 s for 20 s)
	int32_t   posA[2] {}, posB[2] {}; // 2.4.10: where each stood at the mark (a few steps away = walked away)
	uint32_t  closedBy  = 0;     // 2.4.10: a client reported its trade popup / window closed (guid)
	uint64_t  lastAgeLogged = 0; // 2.4.11 diagnostic
};
constexpr int32_t kStakeTradeWalkAway = 6; // 2.4.10: subtiles from the spot at the mark
constexpr int32_t kStakeTradeOpenSeconds = 120; // 2.4.11 (user: "Just close it after 2 mins"): wall clock, then closed
std::vector<TradeWager> g_tradeWagers; // g_pvpLock
struct TradeStart {
	uint32_t              a, b;
	uint32_t              goldA, goldB;
	std::vector<uint32_t> itemsA, itemsB;
	DuelRules             rules;
};
std::vector<TradeStart>                        g_tradeStarts; // g_pvpLock: accepted wager trades, started by the tick
std::map<uint32_t, std::vector<uint32_t>>      g_stakedItems; // g_pvpLock: owner guid -> staked item guids (running duels)
// 2.4.2 (user): it is a "wager window" only once there is a stake (Stakes row on, or gold put up); else the Duel Rules window.
auto WagerWindowName(const WagerSession& w) -> std::string {
	return w.rules.stakes != 0 || w.side[0].gold > 0 || w.side[1].gold > 0 ? "wager window" : "Duel Rules window";
}
auto WagerOfLocked(uint32_t guid) -> WagerSession* {
	for (WagerSession& w : g_wagerSessions) {
		if (w.side[0].guid == guid || w.side[1].guid == guid) {
			return &w;
		}
	}
	return nullptr;
}
struct WagerCmd {
	uint32_t guid;
	uint8_t  kind;
	uint32_t value;
};
std::mutex            g_wagerCmdLock;
std::vector<WagerCmd> g_wagerCmds; // g_wagerCmdLock: from the panel / network, run by the tick
// kMsgWagerState payload; then u8 name length + own name, u8 name length + other name, the note text.
#pragma pack(push, 1)
struct WagerStateMsg {
	uint8_t  open;       // 0 closed, 1 open, 2 closed because the duel starts
	uint32_t myGold, theirGold;
	uint8_t  myReady, theirReady;
	uint8_t  myItems, theirItems; // staked item counts (reserved, 0)
	uint16_t points;
	uint8_t  canSet;      // 1.3.0: 1 = this player set the stake (the challenger)
	uint16_t agreeWaitMs; // 1.3.0: accept works after this many ms
	DuelRules rules;      // 1.6.0
};
#pragma pack(pop)
std::set<uint32_t>    g_frozen;        // g_pvpLock: duelists in the start countdown
std::atomic<int32_t>  g_frozenCount { 0 };
std::atomic<bool>     g_duelBusy { false };  // invites or duels exist: keep the tick running
std::atomic<uint32_t> g_localGuid { 0 };     // this process's own player (UI poll), for host -> own player messages

auto DuelOfLocked(uint32_t guid) -> Duel*;
// A duel of this player in this arena level (a duelist who already left the arena during the end pause is not in it).
auto DuelHereLocked(uint32_t guid, int32_t level) -> bool;
auto DuelOfLocked(uint32_t guid) -> Duel* {
	for (Duel& d : g_duels) {
		if (IsMember(d, guid)) {
			return &d;
		}
	}
	return nullptr;
}
// 1.9.0: 0 = not both in one team duel, 1 = teammates, 2 = opponents.
auto TeamRelationLocked(uint32_t a, uint32_t b) -> int {
	const Duel* d = DuelOfLocked(a);
	if (d == nullptr || d->team.empty()) {
		return 0;
	}
	const auto ia = d->team.find(a), ib = d->team.find(b);
	if (ia == d->team.end() || ib == d->team.end()) {
		return 0;
	}
	return ia->second == ib->second ? 1 : 2;
}
auto DuelHereLocked(uint32_t guid, int32_t level) -> bool {
	const Duel* d = DuelOfLocked(guid);
	return d != nullptr && d->level == level;
}
constexpr int32_t  kTownLevels[] = { 1, 40, 75, 103, 109 }; // levels.txt Act 1-5 towns (excel_txt, read 2026-11-01)
auto IsTownLevel(int32_t level) -> bool {
	return std::find(std::begin(kTownLevels), std::end(kTownLevels), level) != std::end(kTownLevels);
}

std::atomic<uint64_t> g_wagerLabelsUntil { 0 }; // 1.5.0: NowMs until which trade texts read as a wager (0 = normal)
std::atomic<uint64_t> g_pendingSkillCd { 0 };   // 1.7.0: skill << 32 | frames for the local player (UI poll applies it)
std::atomic<bool>     g_closeInvPending { false }; // 1.8.2: a duel starts: close the inventory (UI poll)
// 2.4.14 (user screenshot 2026-11-14 12:05:43, RF1): sometimes the challenged client showed the empty right half of the
// side-panel border next to the stock "Agree to a wager duel" popup; the challenger never does (its trade request closes
// interface state 1 first, D2CLIENT_TRADE_BeginNewTradeOffer). On the wager flag the UI poll logs the inventory state and
// arena's hide / restore flags once, and closes the inventory the same way if it still counts as open.
std::atomic<bool>     g_stakeInvCheck { false };
std::mutex            g_mapHideLock;
std::atomic<int>      g_mapRange { 0 };         // 1.8.3: 0 = hide g_mapHidden always, else only beyond this many subtiles; 1.8.4: 0xFF = only out of sight
std::set<uint32_t>    g_mapHidden;              // 1.8.0: players this client must not draw on the automap (hook: RE pending)
std::set<uint32_t>    g_teamMates;              // 1.9.3: team duel teammates, drawn with the party marker (g_mapHideLock)
std::set<uint32_t>    g_lockTold;              // 1.5.0 game thread: duelists told about the item lock
auto NowMs() -> uint64_t {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
auto LevelOf(void* unit) -> int32_t {
	void* room = unit != nullptr ? g_getRoom(unit) : nullptr;
	return room != nullptr ? g_levelId(room) : 0;
}
auto IsPvpLevel(int32_t level) -> bool {
	return g_config.arenas.count(level) != 0;
}
auto NameOf(void* unit) -> std::string {
	void* client = unit != nullptr ? g_client(unit) : nullptr;
	if (client == nullptr) {
		return "someone";
	}
	const char* name = static_cast<const char*>(client) + kClientName;
	return std::string(name, strnlen(name, 32));
}
void RestoreFull(void* unit) {
	static constexpr int32_t kPairs[][2] = { { 6, 7 }, { 8, 9 }, { 10, 11 } }; // life, mana, stamina (8-bit fixed point)
	for (const auto& p : kPairs) {
		g_setStat(unit, p[0], g_readStat(unit, p[1], 0), 0);
	}
}
void Freeze(void* unit, bool on) {
	g_toggleState(unit, kStateUninterrupt, on ? 1 : 0);
	auto* flags = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(unit) + kUnitFlags);
	*flags      = on ? (*flags & ~kFlagTargetable) : (*flags | kFlagTargetable);
}
void Broadcast(void* game, const std::string& text) {
	int sent = 0;
	for (void* c = Field<void*>(game, kGameClients); c != nullptr && sent < 64; c = Field<void*>(c, kClientNext), ++sent) {
		SendLine(c, Field<uint32_t>(c, kClientUnitGuid), text);
	}
}
void NoteGame(void* game) {
	std::lock_guard lock(g_pvpLock);
	if (g_game != game) {
		g_game = game;
		g_down.clear();
		g_downCount = 0;
		g_lobby     = Lobby {};
		g_scores.clear();
		g_pairs.clear();
		g_lastLevel.clear();
		g_lastTown.clear();
		g_arenaCount.clear();
		g_returns.clear();
		g_alone.clear();
		g_scoreSent.clear();
		g_invites.clear();
		g_duels.clear();
		g_frozen.clear();
		g_frozenCount = 0;
		g_heldPets.clear();
		PublishHeldLocked();
		g_heldCount = 0;
		g_bodies.clear();
	}
}

void __cdecl UiPoll(const D2RL::PluginContext* ctx, void*) noexcept;
void StartUiPoll() {
	if (g_threads != nullptr && !g_uiPolling.exchange(true)) {
		if (g_threads->runOnUiThread(g_ctx, UiPoll, nullptr) != D2RL::Threads::Result::Success) {
			g_uiPolling = false;
		}
	}
}
void __cdecl Tick(const D2RL::PluginContext*, void* queuedEpoch) noexcept;
void QueueTick() {
	if (g_threads == nullptr) {
		return;
	}
	const uint32_t epoch = g_tickEpoch.load();
	if (g_tickQueued.exchange(epoch) != epoch) {
		if (g_threads->runOnGameThread(g_ctx, Tick, reinterpret_cast<void*>(static_cast<uintptr_t>(epoch))) != D2RL::Threads::Result::Success) {
			g_tickQueued = 0;
		}
	}
}

// Host -> the dead player's client: countdown length (ms) or "respawned". The hosting process also shows it itself
// when its own player is the one down (no network needed in a single-player game).
void TellClient(uint32_t guid, uint16_t msg, uint32_t ms) {
	if (msg == kMsgCountdown) {
		g_localDownUntil = NowMs() + ms;
		g_localDownGuid  = guid;
	} else {
		g_localDownGuid = 0;
	}
	StartUiPoll();
	D2RL::Network::PeerHandle peer = D2RL::Network::InvalidPeerHandle;
	{
		std::lock_guard lock(g_pvpLock);
		if (const auto it = g_peers.find(guid); it != g_peers.end()) {
			peer = it->second;
		}
	}
	if (g_net != nullptr && peer != D2RL::Network::InvalidPeerHandle) {
		const auto r = g_net->sendToClient(g_ctx, g_channel, peer, msg, &ms, sizeof(ms));
		Log("arena: message %u to player %u's client: result %d.", msg, guid, static_cast<int>(r));
	} else {
		Log("arena: message %u for player %u not sent (%s).", msg, guid, g_net == nullptr ? "no network" : "no hello from that client yet");
	}
}

// Host -> the last player in an arena: return-to-town countdown (ms) or 0 = cancelled. Chat line too (also the only
// notice with countdown_style = "window").
void TellReturn(void* game, void* unit, uint32_t guid, uint32_t ms) {
	g_localReturnUntil = ms != 0 ? NowMs() + ms : 0;
	g_localReturnGuid  = guid;
	StartUiPoll();
	if (void* c = g_client(unit); c != nullptr && ms != 0) {
		char line[160] {};
		std::snprintf(line, sizeof(line), "%s. %s %u seconds.", g_pvp.returnTitle.c_str(), g_pvp.returnText.c_str(), (ms + 999) / 1000);
		SendLine(c, guid, line);
	}
	(void)game;
	D2RL::Network::PeerHandle peer = D2RL::Network::InvalidPeerHandle;
	{
		std::lock_guard lock(g_pvpLock);
		if (const auto it = g_peers.find(guid); it != g_peers.end()) {
			peer = it->second;
		}
	}
	if (g_net != nullptr && peer != D2RL::Network::InvalidPeerHandle) {
		(void)g_net->sendToClient(g_ctx, g_channel, peer, kMsgReturn, &ms, sizeof(ms));
	}
}

void ResetPotions(uint32_t guid); // potion section

// Host -> one player: the scoreboard line of the arena it stands in ("" = none). Returns true when it reached that
// player's client over the network; the hosting player's own line only goes into g_hostScore (no network needed).
bool TellScore(uint32_t guid, const std::string& text, D2RL::Network::PeerHandle peer) {
	bool changed = false;
	{
		std::lock_guard lock(g_scoreLock);
		std::string& cur = g_hostScore[guid];
		changed          = cur != text;
		cur              = text;
	}
	if (changed) {
		StartUiPoll();
	}
	if (g_net == nullptr || peer == D2RL::Network::InvalidPeerHandle) {
		return false;
	}
	const std::string payload = text.empty() ? std::string(1, '\0') : text.substr(0, kScoreReserve);
	return g_net->sendToClient(g_ctx, g_channel, peer, kMsgScore, payload.data(), static_cast<uint32_t>(payload.size())) == D2RL::Network::Result::Success;
}

// "Name score (kills/deaths)" per player in the arena, best score first, as many as fit kScoreReserve.
struct ScoreEntry {
	std::string name;
	int32_t     score, kills, deaths;
	uint32_t    guid;
};
// 2.0.0 scoreboard art message (kTeamMark): the two side totals, then every player as side + name and score.
// Side 0 = Team One (left), 1 = Team Two (right); at most 4 a side are drawn. 8 players fit kScoreReserve (names <= 16).
struct TeamPlayer {
	uint8_t     side;
	std::string name;
	int32_t     score;
};
auto TeamText(int32_t total0, int32_t total1, const std::vector<TeamPlayer>& players) -> std::string;
// 2.1.0: free-for-all: totals "*" = the client writes "Battle Royale" in the medallion instead of a score.
auto RoyaleText(const std::vector<TeamPlayer>& players) -> std::string {
	std::string t = TeamText(0, 0, players);
	const size_t a = t.find(kTeamMark, 1);   // mark total0 mark total1 ...
	const size_t b = t.find(kTeamMark, a + 1);
	return std::string(1, kTeamMark) + "*" + kTeamMark + "*" + (b == std::string::npos ? std::string() : t.substr(b));
}
auto TeamText(int32_t total0, int32_t total1, const std::vector<TeamPlayer>& players) -> std::string {
	std::string t = kTeamMark + std::to_string(total0) + kTeamMark + std::to_string(total1);
	for (const TeamPlayer& p : players) {
		t += kTeamMark + std::string(1, static_cast<char>('0' + std::min<uint8_t>(p.side, 2))) + p.name.substr(0, 16) + kTeamMark + std::to_string(p.score);
	}
	return t;
}
auto ScoreLine(std::vector<ScoreEntry> list) -> std::string {
	std::sort(list.begin(), list.end(), [](const ScoreEntry& a, const ScoreEntry& b) { return a.score != b.score ? a.score > b.score : a.name < b.name; });
	std::string line;
	for (const ScoreEntry& e : list) {
		char part[96] {};
		std::snprintf(part, sizeof(part), "%s%s %d (%d/%d)", line.empty() ? "" : "      ", e.name.c_str(), e.score, e.kills, e.deaths);
		if (line.size() + std::strlen(part) > kScoreReserve) {
			break;
		}
		line += part;
	}
	return line;
}

// Everyone in the same arena level hostile unless in the same party (game thread).
void SyncHostility(void* game) {
	struct P {
		void*    unit;
		uint32_t guid;
		int32_t  level;
		uint32_t party;
	};
	std::vector<P> players;
	int            n = 0;
	for (void* c = Field<void*>(game, kGameClients); c != nullptr && n < 64; c = Field<void*>(c, kClientNext), ++n) {
		if (Field<uint32_t>(c, kClientUnitType) != kUnitPlayer) {
			continue;
		}
		const uint32_t guid = Field<uint32_t>(c, kClientUnitGuid);
		if (void* u = g_serverUnit(game, kUnitPlayer, guid); u != nullptr) {
			players.push_back({ u, guid, LevelOf(u), static_cast<uint32_t>(g_partyId(u) & 0xFFFF) });
		}
	}
	bool                     any = false;
	std::vector<std::string> lines; // sent after the lock is released
	std::unique_lock         lock(g_pvpLock);
	std::set<uint64_t>       seen;
	std::map<int32_t, int32_t>  counts; // arena level -> players
	std::map<int32_t, uint32_t> single; // arena level -> one player in it
	for (const P& p : players) {
		if (IsTownLevel(p.level)) {
			g_lastTown[p.guid] = p.level;
		}
		if (IsPvpLevel(p.level)) {
			++counts[p.level];
			single[p.level] = p.guid;
		}
	}
	for (const auto& [level, before] : g_arenaCount) { // someone left (level change or game): one player remains
		const auto it = counts.find(level);
		if (g_pvp.lastReturn && before >= 2 && it != counts.end() && it->second == 1 && DuelOfLocked(single[level]) == nullptr) {
			const uint32_t guid = single[level];
			if (std::none_of(g_returns.begin(), g_returns.end(), [&](const PendingReturn& r) { return r.guid == guid; })) {
				g_returns.push_back({ guid, level });
				Log("arena: player %u is the last one in level %d: return to town queued.", guid, level);
			}
		}
	}
	// Scores count per fight (user 2026-11-01, S6): a fight starts when an arena level gets its 2nd player (everyone in
	// it starts at 0); a player who joins a running fight starts at 0 too. `arena scores` shows the same.
	for (const P& p : players) {
		if (!IsPvpLevel(p.level)) {
			continue;
		}
		const auto    b       = g_arenaCount.find(p.level);
		const int32_t before  = b != g_arenaCount.end() ? b->second : 0;
		const auto    last    = g_lastLevel.find(p.guid);
		const bool    newHere = last == g_lastLevel.end() || last->second != p.level;
		if (((counts[p.level] >= 2 && before < 2) || (newHere && before >= 2)) && g_scores.erase(p.guid) != 0) {
			Log("arena: score of player %u reset (new fight in level %d).", p.guid, p.level);
		}
		if (newHere) {
			if (const auto jw = std::find_if(g_joinWait.begin(), g_joinWait.end(), [&](const JoinWait& w) { return w.guid == p.guid; }); jw != g_joinWait.end()) {
				PlayerScore& js = g_scores[p.guid];
				js              = PlayerScore {};
				js.name         = NameOf(p.unit);
				js.score        = jw->score;
			}
		}
	}
	g_arenaCount = counts;
	if (!g_returns.empty()) {
		any = true; // keep the tick running
	}
	std::vector<uint32_t> entered; // came into an arena level since the last sync (any way: trapdoor, debug teleport)
	for (size_t i = 0; i < players.size(); ++i) {
		any |= IsPvpLevel(players[i].level);
		if (IsPvpLevel(players[i].level)) {
			const auto it = g_lastLevel.find(players[i].guid);
			if (it == g_lastLevel.end() || it->second != players[i].level) {
				entered.push_back(players[i].guid);
			}
		}
		g_lastLevel[players[i].guid] = players[i].level;
		for (size_t j = i + 1; j < players.size(); ++j) {
			const P&       a = players[i];
			const P&       b = players[j];
			const uint64_t key = (static_cast<uint64_t>(std::min(a.guid, b.guid)) << 32) | std::max(a.guid, b.guid);
			seen.insert(key);
			bool       want = IsPvpLevel(a.level) && a.level == b.level && !(a.party != 0xFFFF && a.party == b.party);
			if (const int rel = TeamRelationLocked(a.guid, b.guid); rel != 0) { // 1.9.0: never teammates, always the other team
				want = IsPvpLevel(a.level) && a.level == b.level && rel == 2;
			}
			if (want) {
				bool opened = false;
				if (g_checkFlag(a.unit, b.unit, kHostileFlag) == 0) {
					g_openHostile(game, a.unit, b.unit), opened = true;
				}
				if (g_checkFlag(b.unit, a.unit, kHostileFlag) == 0) {
					g_openHostile(game, b.unit, a.unit), opened = true;
				}
				if (opened) {
					g_pairs.insert(key);
					g_cleanQueue.push_back(a.guid);
					g_cleanQueue.push_back(b.guid);
					Log("arena: %u and %u are hostile in level %d.", a.guid, b.guid, a.level);
					lines.push_back(NameOf(a.unit) + " and " + NameOf(b.unit) + " are now hostile.");
				}
			} else if (g_pairs.erase(key) != 0) {
				g_removeHostile(game, a.unit, b.unit);
				g_removeHostile(game, b.unit, a.unit);
				Log("arena: hostility between %u and %u removed.", a.guid, b.guid);
				lines.push_back(NameOf(a.unit) + " and " + NameOf(b.unit) + " are no longer hostile.");
			}
		}
	}
	for (auto it = g_pairs.begin(); it != g_pairs.end();) { // a player left the game
		it = seen.count(*it) != 0 ? std::next(it) : g_pairs.erase(it);
	}
	g_anyInArena = any;
	struct ScoreSend {
		uint32_t                  guid;
		std::string               text;
		D2RL::Network::PeerHandle peer;
	};
	std::vector<ScoreSend> scoreSends;
	if (g_pvp.scoreboard) {
		std::map<int32_t, std::vector<ScoreEntry>> byLevel;
		for (const P& p : players) {
			if (IsPvpLevel(p.level)) {
				const auto it = g_scores.find(p.guid);
				ScoreEntry e { NameOf(p.unit), 0, 0, 0, p.guid };
				if (it != g_scores.end()) {
					e.score = it->second.score, e.kills = it->second.kills, e.deaths = it->second.deaths;
				}
				// 2.3.0 (user): a Kills duel shows kills, not points (a walk-in's join points / a royale death would differ)
				if (const Duel* kd = DuelOfLocked(p.guid); kd != nullptr && kd->level == p.level && kd->rules.type == kTypeKills) {
					e.score = e.kills;
				}
				byLevel[p.level].push_back(e);
			}
		}
		std::map<int32_t, std::string> lineOf;
		for (auto& [level, list] : byLevel) {
			lineOf[level] = ScoreLine(list);
		}
		for (const P& p : players) {
			std::string text;
			if (IsPvpLevel(p.level)) {
				const std::vector<ScoreEntry>& list = byLevel[p.level];
				if (const Duel* td = DuelOfLocked(p.guid); td != nullptr && !td->team.empty() && td->level == p.level) {
					// 1.9.0: the two team totals (kills / deaths summed over the members)
					std::vector<ScoreEntry> two { { td->teamName[0], td->teamScore[0], 0, 0, td->a }, { td->teamName[1], td->teamScore[1], 0, 0, td->b } };
					for (const auto& [g, side] : td->team) {
						if (const auto sit = g_scores.find(g); sit != g_scores.end()) {
							two[side].kills += sit->second.kills, two[side].deaths += sit->second.deaths;
						}
					}
					if (g_pvp.scoreboardArt) { // 2.0.0: Team One left, Team Two right, every player on a plate
						std::vector<TeamPlayer> members;
						for (const auto& [g, side] : td->team) {
							const auto sit = g_scores.find(g);
							const int32_t shown = sit == g_scores.end() ? 0 : td->rules.type == kTypeKills ? sit->second.kills : sit->second.score;
							members.push_back({ side, MemberName(*td, g), shown });
						}
						std::stable_sort(members.begin(), members.end(), [](const TeamPlayer& x, const TeamPlayer& y) { return x.side < y.side; });
						text = TeamText(td->teamScore[0], td->teamScore[1], members);
					} else {
						text = ScoreLine(two);
					}
				} else if (g_pvp.scoreboardArt && list.size() == 2) {
					// 2.0.0: 1 v 1 on the same art. In a duel the challenger is on the left for both; otherwise your own name.
					const Duel* d    = DuelOfLocked(p.guid);
					const bool  aFst = d != nullptr && d->team.empty() ? list[0].guid == d->a : list[0].guid == p.guid;
					const ScoreEntry& l = aFst ? list[0] : list[1];
					const ScoreEntry& r = aFst ? list[1] : list[0];
					text = TeamText(l.score, r.score, { { 0, l.name, l.score }, { 1, r.name, r.score } });
				} else if (g_pvp.scoreboardArt && list.size() >= 3 && list.size() <= 2 * 4) {
					// 2.1.0 (user): a free-for-all of 3+ = "Battle Royale": the players split over both sides (3 -> 2 + 1,
					// 4 -> 2 + 2, ... 8 -> 4 + 4), each score on its own plate, no totals. Fixed order (guid) so plates don't jump.
					std::vector<ScoreEntry> byGuid = list;
					std::sort(byGuid.begin(), byGuid.end(), [](const ScoreEntry& x, const ScoreEntry& y) { return x.guid < y.guid; });
					const size_t            left = (byGuid.size() + 1) / 2;
					std::vector<TeamPlayer> all;
					for (size_t k = 0; k < byGuid.size(); ++k) {
						all.push_back({ static_cast<uint8_t>(k < left ? 0 : 1), byGuid[k].name, byGuid[k].score });
					}
					text = RoyaleText(all);
				} else if (g_pvp.scoreboardArt && list.size() == 1) { // 2.0.3 (user): alone in the arena -> one plate
					text = TeamText(list[0].score, 0, { { 2, list[0].name, list[0].score } });
				} else {
					text = lineOf[p.level];
				}
			}
			const auto  sent = g_scoreSent.find(p.guid);
			const auto  peer = g_peers.find(p.guid);
			// no peer = the hosting player (or no hello yet): g_hostScore is refreshed every sync, cheap
			if ((sent == g_scoreSent.end() ? std::string() : sent->second) != text || peer == g_peers.end()) {
				scoreSends.push_back({ p.guid, std::move(text), peer != g_peers.end() ? peer->second : D2RL::Network::InvalidPeerHandle });
			}
		}
	}
	const bool clean = !g_cleanQueue.empty();
	lock.unlock();
	for (const ScoreSend& ss : scoreSends) {
		if (TellScore(ss.guid, ss.text, ss.peer)) {
			std::lock_guard sl(g_pvpLock);
			g_scoreSent[ss.guid] = ss.text;
		}
	}
	for (uint32_t guid : entered) {
		ResetPotions(guid); // potion limits count per arena visit
	}
	if (clean) {
		QueueTick(); // CleanStart runs in the tick: SyncHostility is also called from inside a player mode start
	}
	if (g_pvp.hostileMessages) {
		for (const std::string& line : lines) {
			Broadcast(game, line);
		}
	}
}


// Clean start (user 2026-11-01: "everything except passives"): un-shift, the stock death strip (every statlist but
// item/basic/plrstaydeath, every non-plrstaydeath state bit), the passives back as the stock resurrect does, full
// life/mana/stamina sent to the owner. notes/arena-research/combat-rules.md 1.3 (A).
void CleanStart(void* game, void* unit) {
	if (g_unshift(unit) != 0) {
		g_changeMode2(game, unit, nullptr, kPlrModeNeutral, 0, 0, 1); // the form's mode would keep running in human graphics
	}
	g_removeDeathStats(unit);
	g_stayDeath(unit, 0);
	g_refreshPassives(unit);
	RestoreFull(unit);
	for (const int32_t stat : { 6, 8, 10 }) {
		g_updateAttr(unit, stat, static_cast<uint32_t>(g_readStat(unit, stat, 0)), unit, 0);
	}
}

// ---- pets in the arena (0.7.0, user 2026-11-01: Iron Golem / bound demon come back with the owner) ----
// A kept pet never dies in an arena: SUNITDMG_KillMonster (every monster death path) is intercepted for it. It is healed,
// made untargetable, put in neutral with its AI event deleted (it stands still; nothing changes its mode while it can't
// be hit), and shows the stun stars. Same unit = same stats, items, bound stats; nothing is re-created. It comes back
// next to its owner at the owner's next arena respawn, or where it is when the owner leaves the arena / the game.
// notes/arena-research/pets.md.
using KillMonsterFn = void(__fastcall*)(void* game, void* defender, void* attacker, int32_t bPetKill);
using MinionOwnerFn = void*(__fastcall*)(void* monster);
using PetTypeFn     = int32_t(__fastcall*)(void* player, uint32_t petGuid);
using WarpPetFn     = int32_t(__fastcall*)(void* game, void* owner, void* pet);
using IdleNeutralFn = void(__fastcall*)(void* game, void* unit, int32_t frames);
using DeleteEventFn = void(__fastcall*)(void* game, void* unit, int32_t type, int32_t customId);
std::atomic<KillMonsterFn> g_origKillMonster { nullptr };
MinionOwnerFn g_minionOwner = nullptr;
PetTypeFn     g_petType     = nullptr;
using PetCallbackFn = void(__fastcall*)(void* game, void* player, void* pet, void* arg);
using IteratePetsFn = void(__fastcall*)(void* game, void* player, PetCallbackFn callback, void* arg, char flag);
IteratePetsFn g_iteratePets = nullptr;
WarpPetFn     g_warpPet     = nullptr;
IdleNeutralFn g_idleNeutral = nullptr;
DeleteEventFn g_deleteEvent = nullptr;
constexpr uint32_t kUnitMonster       = 1;
constexpr int32_t  kPetTypeGolem      = 3;   // pettype.txt rows (excel_txt): golem (all four golems)
constexpr int32_t  kPetTypeDemon      = 20;  // demon
constexpr int32_t  kPetTypeBindDemon  = 21;  // binddemon (Bind Demon)
constexpr uint32_t kMonIronGolem      = 291; // monstats.txt irongolem
constexpr int32_t  kStateStunned      = 21;  // states.txt stunned: overlay "stun" only (stars), no stat
constexpr uint32_t kFlagUpdateLife    = 0x8000; // D2MOO UNITFLAG_UPGRLIFENHITCLASS: life bar refresh on clients

auto KeepPet(void* owner, void* pet) -> bool {
	const int32_t type = g_petType(owner, Field<uint32_t>(pet, 0x08));
	if (type == kPetTypeDemon || type == kPetTypeBindDemon) {
		return true;
	}
	return type == kPetTypeGolem && (g_pvp.petsAllGolems || Field<uint32_t>(pet, 0x04) == kMonIronGolem);
}
void HealPet(void* pet) {
	g_removeStates(pet); // poison, curses
	g_setStat(pet, 6, g_readStat(pet, 7, 0), 0);
	*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pet) + kUnitFlags) |= kFlagUpdateLife;
}
void HoldPet(void* game, void* owner, void* pet, int32_t level, bool keepLife = false) {
	const uint32_t guid = Field<uint32_t>(pet, 0x08);
	if (!keepLife) {
		HealPet(pet);
	}
	bool again = false;
	{
		std::lock_guard lock(g_pvpLock);
		again = std::any_of(g_heldPets.begin(), g_heldPets.end(), [&](const HeldPet& h) { return h.pet == guid; });
		if (!again) {
			g_heldPets.push_back({ guid, Field<uint32_t>(owner, 0x08), level, keepLife });
			g_heldCount = static_cast<int32_t>(g_heldPets.size());
			PublishHeldLocked();
		}
	}
	if (again) {
		return; // already down (aura or poison tick): healed again
	}
	auto* flags = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pet) + kUnitFlags);
	*flags &= ~kFlagTargetable;
	g_idleNeutral(game, pet, 1);    // neutral (stops the walk / attack) ...
	g_deleteEvent(game, pet, 2, 0); // ... then no AI tick any more
	g_toggleState(pet, kStateStunned, 1);
	Log("arena: pet %u (monster %u) of player %u is down in level %d until its owner respawns.", guid, Field<uint32_t>(pet, 0x04), Field<uint32_t>(owner, 0x08), level);
	QueueTick();
}
void ReleasePet(void* game, const HeldPet& h, bool warp) {
	void* pet = g_serverUnit(game, kUnitMonster, h.pet);
	if (pet == nullptr) {
		return;
	}
	void* owner = g_serverUnit(game, kUnitPlayer, h.owner);
	g_toggleState(pet, kStateStunned, 0);
	if (!h.keepLife) {
		HealPet(pet);
	}
	*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pet) + kUnitFlags) |= kFlagTargetable;
	const bool warped = warp && owner != nullptr && LevelOf(owner) == LevelOf(pet) && g_warpPet(game, owner, pet) != 0;
	if (!warped) {
		g_idleNeutral(game, pet, 5); // AI back (WarpPetToOwner does this itself)
	}
	Log("arena: pet %u of player %u is back%s.", h.pet, h.owner, warped ? " next to its owner" : "");
}
// Owner down (0.8.0, user 2026-11-07): every living pet and the merc stand still until the owner's respawn, or a downed
// player's summons could keep fighting and even win a duel. Same hold as a "killed" pet, without the heal.
void __fastcall HoldLivingPet(void* game, void* owner, void* pet, void* arg) {
	const int32_t level = *static_cast<const int32_t*>(arg);
	if (pet != nullptr && LevelOf(pet) == level && g_readStat(pet, 6, 0) >= 256) {
		HoldPet(game, owner, pet, level, true);
	}
}
void HoldOwnerPets(void* game, void* owner) {
	if (g_pvp.petsHoldDown && g_iteratePets != nullptr) {
		int32_t level = LevelOf(owner);
		g_iteratePets(game, owner, HoldLivingPet, &level, 0);
	}
}
// Every held pet of this owner back (respawn: warp = next to the owner; leaving: where it stands, before the warp so the
// stock pet follow takes it along).
void ReleasePets(void* game, uint32_t owner, bool warp) {
	if (g_heldCount.load(std::memory_order_relaxed) == 0) {
		return;
	}
	std::vector<HeldPet> mine;
	{
		std::lock_guard lock(g_pvpLock);
		for (auto it = g_heldPets.begin(); it != g_heldPets.end();) {
			if (it->owner == owner) {
				mine.push_back(*it);
				it = g_heldPets.erase(it);
			} else {
				++it;
			}
		}
		g_heldCount = static_cast<int32_t>(g_heldPets.size());
		PublishHeldLocked();
	}
	for (const HeldPet& h : mine) {
		ReleasePet(game, h, warp);
	}
}
// Tick: a held pet whose unit is gone (re-cast golem) is forgotten; one whose owner left the game or the level is let go.
void CheckHeldPets(void* game) {
	if (g_heldCount.load(std::memory_order_relaxed) == 0) {
		return;
	}
	std::vector<HeldPet> all, gone;
	{
		std::lock_guard lock(g_pvpLock);
		all = g_heldPets;
	}
	for (const HeldPet& h : all) {
		void* pet   = g_serverUnit(game, kUnitMonster, h.pet);
		void* owner = g_serverUnit(game, kUnitPlayer, h.owner);
		if (pet == nullptr || owner == nullptr || LevelOf(owner) != h.level) {
			gone.push_back(h);
		}
	}
	if (gone.empty()) {
		return;
	}
	{
		std::lock_guard lock(g_pvpLock);
		std::erase_if(g_heldPets, [&](const HeldPet& h) { return std::any_of(gone.begin(), gone.end(), [&](const HeldPet& g) { return g.pet == h.pet; }); });
		g_heldCount = static_cast<int32_t>(g_heldPets.size());
		PublishHeldLocked();
	}
	for (const HeldPet& h : gone) {
		ReleasePet(game, h, false);
	}
}
// SUNITDMG_KillMonster 0x14044DB20 (19 callers: missile/melee damage, stat regen, DEATH_DELAY, orphan pets, golem re-cast).
// Only a real damage death (life < 1) of a kept pet with a player owner in an arena level is stopped; the golem re-cast
// (life > 0) and the pet removal path pass through.
void __fastcall ArenaKillMonster(void* game, void* defender, void* attacker, int32_t bPetKill) {
	const KillMonsterFn orig = g_origKillMonster.load(std::memory_order_acquire);
	if (g_pvp.petsKeep && game != nullptr && defender != nullptr && Field<uint32_t>(defender, 0x00) == kUnitMonster && g_readStat(defender, 6, 0) < 256) {
		const int32_t level = LevelOf(defender);
		if (IsPvpLevel(level)) {
			void* owner = g_minionOwner(defender);
			if (owner != nullptr && Field<uint32_t>(owner, 0x00) == kUnitPlayer && KeepPet(owner, defender)) {
				HoldPet(game, owner, defender, level);
				return;
			}
		}
	}
	if (orig != nullptr) {
		orig(game, defender, attacker, bPetKill);
	}
}

// ---- bodies for Raise Skeleton / Skeletal Mage (0.7.1, user 2026-11-02: "no revives - just the skeleton warriors and
// skeletal mages") ----
// While a player is in an arena, the plugin keeps [arena.bodies] count monsters lying there in DEAD mode (stock D2 does
// the same for the Bloody Foothills bodies: D2MOO MonsterRegion.cpp preset classes 29-32). SKILLS_SrvSt15/SrvDo031 use
// them like any corpse (UNITS_IsCorpseUseable: monster, mode 0x0C, monstats2 corpseSel). A used body is replaced after
// respawn_seconds. The other corpse skills (srvst slots 17, 21, 33, 34) are refused in the arena levels.
using SpawnMonsterFn = void*(__fastcall*)(void* game, void* room, int32_t x, int32_t y, int32_t monster, int32_t mode, int32_t a7, int16_t flags);
using SrvStartFn     = int32_t(__fastcall*)(void* game, void* unit, int32_t skill, int32_t level);
SpawnMonsterFn g_spawnMonster = nullptr;
std::array<SrvStartFn, 0x5B> g_origSrvStart {};
constexpr int32_t kMonModeDead = 12;
void Refuse(void* game, void* player, const std::string& text);

template <int Slot>
int32_t __fastcall CorpseSkillGate(void* game, void* unit, int32_t skill, int32_t level) {
	if (unit != nullptr && Field<uint32_t>(unit, 0x00) == kUnitPlayer && IsPvpLevel(LevelOf(unit))) {
		Refuse(game, unit, g_pvp.bodiesRefuse);
		return 0;
	}
	return g_origSrvStart[Slot](game, unit, skill, level);
}

void ProcessBodies(void* game, int32_t frame) {
	if (g_spawnMonster == nullptr || g_pvp.bodiesCount <= 0 || frame % 5 != 0) {
		return;
	}
	std::map<int32_t, std::pair<int32_t, int32_t>> occupied; // arena level with a player -> level origin (subtiles)
	int n = 0;
	for (void* c = Field<void*>(game, kGameClients); c != nullptr && n < 64; c = Field<void*>(c, kClientNext), ++n) {
		if (Field<uint32_t>(c, kClientUnitType) != kUnitPlayer) {
			continue;
		}
		int32_t level = 0, ox = 0, oy = 0;
		if (void* u = g_serverUnit(game, kUnitPlayer, Field<uint32_t>(c, kClientUnitGuid)); u != nullptr && ArenaOf(u, &level, &ox, &oy) != nullptr) {
			occupied[level] = { ox, oy };
		}
	}
	const int32_t delay = static_cast<int32_t>(g_pvp.bodiesRespawn * kFramesPerSecond + 0.5);
	for (auto& [level, b] : g_bodies) {
		for (BodySlot& slot : b.slots) {
			if (slot.guid == 0) {
				continue;
			}
			void* m = g_serverUnit(game, kUnitMonster, slot.guid);
			if (m == nullptr || g_getAnimMode(m) != kMonModeDead) { // raised (removed) or room freed
				slot.guid = 0;
				slot.due  = frame + delay;
			}
		}
	}
	for (const auto& [level, origin] : occupied) {
		ArenaBodies& b = g_bodies[level];
		if (b.slots.empty()) { // first visit: every slot gets its own fixed spot, so the bodies cover the whole arena
			const auto list = g_pvp.bodySpots.find(level);
			const auto sp   = list != g_pvp.bodySpots.end() && !list->second.empty() ? list : g_pvp.spawns.find(level);
			if (sp == g_pvp.spawns.end() || sp->second.empty()) {
				continue;
			}
			for (int32_t i = 0; i < g_pvp.bodiesCount; ++i) {
				const Spot& s = sp->second[static_cast<size_t>(i) % sp->second.size()];
				b.slots.push_back({ origin.first + s.x + std::uniform_int_distribution<int32_t>(-2, 2)(Rng()),
				                    origin.second + s.y + std::uniform_int_distribution<int32_t>(-2, 2)(Rng()) });
			}
		}
		for (BodySlot& slot : b.slots) {
			if (slot.guid != 0 || frame - slot.due < 0) {
				continue;
			}
			// A room only exists while a player is near: the slot waits for its own room (B8 2026-11-07: moving on to the
			// next spot put nearly all the bodies around the first player to arrive).
			void* room = RoomAt(game, level, slot.x, slot.y, false);
			if (room == nullptr) {
				continue;
			}
			void* m = g_spawnMonster(game, room, slot.x, slot.y, g_pvp.bodiesMonster, kMonModeDead, -1, 8);
			if (m == nullptr) {
				slot.due = frame + 25; // try again in a second
				continue;
			}
			// 1 life: Corpse Explosion's damage comes from the corpse's stored max life (0 would fall back to monster
			// stats, D2MOO SKILLS_SrvDo055_CorpseExplosion). It is refused in the arenas; this keeps it harmless regardless.
			g_setStat(m, 7, 256, 0);
			g_setStat(m, 6, 256, 0);
			slot.guid = Field<uint32_t>(m, 0x08);
			Log("arena: body (monster %d) placed at %d,%d in level %d.", g_pvp.bodiesMonster, slot.x, slot.y, level);
		}
	}
}

void Respawn(void* game, uint32_t guid, bool toSpawnSpot = true) {
	void* unit = g_serverUnit(game, kUnitPlayer, guid);
	if (unit == nullptr) {
		return;
	}
	// As the stock resurrect (D2GAME_PACKETCALLBACK_Rcv0x41_Resurrect 0x1404B6160) without the warp to town: the client
	// "died" locally when it saw life 0, so it needs the new life sent explicitly (UpdateAttribute) - P2 2026-10-31: the
	// dead player stayed dead on its own screen after a respawn that only used SetWideUnitStat + SetCombatMode.
	Freeze(unit, false);
	if (g_pvp.cleanStart) {
		CleanStart(game, unit);
	} else {
		g_removeStates(unit); // curses / poison / cold applied before or during the countdown
		RestoreFull(unit);
		for (const int32_t stat : { 6, 8, 10 }) {
			g_updateAttr(unit, stat, static_cast<uint32_t>(g_readStat(unit, stat, 0)), unit, 0);
		}
	}
	g_setTargetable(unit, 1);
	g_toggleState(unit, kStateUninterrupt, 1);
	g_toggleState(unit, kStateUninterrupt, 0);
	int32_t      level = 0, ox = 0, oy = 0;
	const Arena* arena = ArenaOf(unit, &level, &ox, &oy);
	const auto   it    = g_pvp.spawns.find(level);
	if (toSpawnSpot && arena != nullptr && it != g_pvp.spawns.end() && !it->second.empty()) {
		Spot s {};
		if (!PickRespawnSpot(game, unit, level, ox, oy, &s)) { // 2.3.0: away from the enemies and the death spot
			s = it->second[std::uniform_int_distribution<size_t>(0, it->second.size() - 1)(Rng())];
		}
		(void)Teleport(game, unit, level, ox + s.x, oy + s.y);
	}
	g_changeMode2(game, unit, nullptr, kPlrModeNeutral, 0, 0, 1); // after the move, as the stock resurrect after its warp
	TellClient(guid, kMsgRespawned, 0);
	std::string drops;
	for (size_t i = 0; i < g_packetDrops.size(); ++i) {
		if (const uint32_t n = g_packetDrops[i].exchange(0); n != 0) {
			char b[24] {};
			std::snprintf(b, sizeof(b), " 0x%02zX x%u", i, n);
			drops += b;
		}
	}
	Log("arena: player %u respawned in level %d (packets dropped while down:%s).", guid, level, drops.empty() ? " none" : drops.c_str());
	ReleasePets(game, guid, true);
}

// ---- entrance trapdoors (user 2026-11-01: arena entrances beside the Lut Gholein palace) ----
std::atomic<bool> g_wantEntrances { false };
// 1.9.4 (user 2026-11-08): no obelisks and no arena entrances in a single player game. CLIENT_GetGameType RVA 0x8AF90 (read 2026-11-08 in the dump:
// 8B 05 5E 84 99 02 C3 = mov eax, [g_client_nGameType 0x142A233F4]; ret). 0 = single player (CMDTBL_OnParty opens the
// party screen only when != 0); 5 / 7 = server in this process with others (CLIENT_IsServerInProcess mask 0xA1), other
// values = joined. The bytes differ -> treated as multiplayer (obelisks shown, as before).
using GameTypeFn = int32_t(__fastcall*)();
GameTypeFn g_gameType = nullptr;
auto IsSinglePlayerGame() -> bool {
	return g_gameType != nullptr && g_gameType() == 0;
}
// Spawns each configured entrance trapdoor whose room is active and that isn't there (new game, room rebuilt).
// 1.9.4 (user): none in single player (no exit from the arenas there but a town portal; duels need other players).
void EnsureEntrances(void* game) {
	if (IsSinglePlayerGame()) {
		return;
	}
	for (const Entrance& e : g_config.entrances) {
		void* room = RoomAt(game, g_config.entranceLevel, e.x, e.y, false);
		if (room == nullptr || ObjectAt(game, g_config.entranceLevel, e.object, e.x, e.y) != nullptr) {
			continue;
		}
		void* obj = g_allocUnit(static_cast<int32_t>(kUnitObject), e.object, e.x, e.y, game, room, 1, kModeOpened, 0, nullptr);
		Log("arena: entrance trapdoor to level %d at %d,%d %s.", e.arena, e.x, e.y, obj != nullptr ? "spawned" : "could not be spawned");
	}
}
// 1.9.0: the team duel obelisks at [arena.lobby] spot (1.9.3: each pair), when the room is active and it isn't there yet.
// Every obelisk opens the same host lobby (g_lobby), so they work as one.
constexpr size_t kLobbySpotsMax = 4;
std::mutex g_lobbySpotLock; // g_config.lobbySpots: the tick changes it (`arena lobby here n`), the console copies it
auto LobbySpots() -> std::vector<std::pair<int32_t, int32_t>> {
	std::lock_guard lock(g_lobbySpotLock);
	return g_config.lobbySpots;
}
void EnsureLobby(void* game) {
	if (IsSinglePlayerGame()) {
		static std::atomic<int> told { 0 };
		if (told.exchange(1) == 0) {
			Log("arena: single player game (game type 0): no arena entrances and no team duel obelisks.");
		}
		return;
	}
	for (const auto& [x, y] : LobbySpots()) {
		void* room = RoomAt(game, g_config.entranceLevel, x, y, false);
		if (room == nullptr || ObjectAt(game, g_config.entranceLevel, g_config.lobbyObject, x, y) != nullptr) {
			continue;
		}
		void* obj = g_allocUnit(static_cast<int32_t>(kUnitObject), g_config.lobbyObject, x, y, game, room, 1, kModeNeutral, 0, nullptr);
		Log("arena: team duel obelisk at %d,%d %s.", x, y, obj != nullptr ? "spawned" : "could not be spawned");
	}
}
std::atomic<uint64_t> g_lobbyHere { 0 };    // `arena lobby here [n]` (host): x << 32 | y, the tick moves spot n and spawns there
std::atomic<int>      g_lobbyHereIndex { 0 }; // 1.9.3: which obelisk (0-based), set before g_lobbyHere
auto IsDown(uint32_t guid) -> bool;
auto IsDownPlayer(void* player) -> bool {
	return player != nullptr && IsDown(Field<uint32_t>(player, 0x08));
}
// Into an arena: LEVEL_WarpUnit (same act: the level's start tile), then a random spawn spot, then NEUTRAL (as the
// stock resurrect / waypoint after a warp). notes/arena-research/match-flow.md section 1.
// 2.3.0: into the arena at a given spot (level-relative; nullptr = a random spawns spot).
void EnterArenaAt(void* game, void* player, int32_t arena, const Spot* at) {
	g_warpUnit(game, player, static_cast<uint32_t>(arena), 0);
	int32_t    level = 0, ox = 0, oy = 0;
	const auto it    = g_pvp.spawns.find(arena);
	if (ArenaOf(player, &level, &ox, &oy) != nullptr && level == arena) {
		if (at != nullptr) {
			(void)Teleport(game, player, level, ox + at->x, oy + at->y);
		} else if (it != g_pvp.spawns.end() && !it->second.empty()) {
			const Spot& s = it->second[std::uniform_int_distribution<size_t>(0, it->second.size() - 1)(Rng())];
			(void)Teleport(game, player, level, ox + s.x, oy + s.y);
		}
	}
	if (g_changeMode2 != nullptr) {
		g_changeMode2(game, player, nullptr, kPlrModeNeutral, 0, 0, 1);
	}
	Log("arena: player %u took the entrance to level %d (now in level %d).", Field<uint32_t>(player, 0x08), arena, LevelOf(player));
}
// 2.3.0 (user 2026-11-08): duel start - everyone on their own spot (spawns + start_spawns). Players are placed one at a
// time, the sides taking turns; each takes the free spot farthest from the enemies already placed, where being near a
// teammate counts a quarter as much (squared distance x 4): if two must be close, teammates first. The first is random.
// Out of free spots: the best spot again (Teleport finds free ground beside it). Empty = the level has no spots.
auto AssignStartSpots(int32_t level, const std::vector<uint8_t>& sides) -> std::vector<Spot> {
	std::vector<Spot> pool;
	if (const auto it = g_pvp.spawns.find(level); it != g_pvp.spawns.end()) {
		pool = it->second;
	}
	if (const auto it = g_pvp.startSpawns.find(level); it != g_pvp.startSpawns.end()) {
		pool.insert(pool.end(), it->second.begin(), it->second.end());
	}
	if (pool.empty() || sides.empty()) {
		return {};
	}
	std::vector<size_t> bySide[2];
	for (size_t i = 0; i < sides.size(); ++i) {
		bySide[sides[i] & 1].push_back(i);
	}
	std::shuffle(bySide[0].begin(), bySide[0].end(), Rng());
	std::shuffle(bySide[1].begin(), bySide[1].end(), Rng());
	std::vector<size_t> order;
	const int first = std::uniform_int_distribution<int>(0, 1)(Rng()); // which side starts
	for (size_t k = 0; k < std::max(bySide[0].size(), bySide[1].size()); ++k) {
		for (int t : { first, first ^ 1 }) {
			if (k < bySide[t].size()) {
				order.push_back(bySide[t][k]);
			}
		}
	}
	std::vector<Spot>                            out(sides.size());
	std::vector<bool>                            used(pool.size(), false);
	std::vector<std::pair<Spot, uint8_t>>        placed;
	const auto d2 = [](const Spot& a, const Spot& b) { return static_cast<double>(a.x - b.x) * (a.x - b.x) + static_cast<double>(a.y - b.y) * (a.y - b.y); };
	for (const size_t p : order) {
		const bool anyFree = std::find(used.begin(), used.end(), false) != used.end();
		int        best    = -1;
		double     bestSc  = -1;
		if (placed.empty()) {
			best = std::uniform_int_distribution<int>(0, static_cast<int>(pool.size()) - 1)(Rng());
		} else {
			for (size_t i = 0; i < pool.size(); ++i) {
				if (used[i] && anyFree) {
					continue;
				}
				double enemy = 1e18, mate = 1e18;
				for (const auto& [sp, side] : placed) {
					(side == (sides[p] & 1) ? mate : enemy) = std::min(side == (sides[p] & 1) ? mate : enemy, d2(pool[i], sp));
				}
				const double sc = std::min(enemy, mate * 4.0);
				if (sc > bestSc) {
					bestSc = sc, best = static_cast<int>(i);
				}
			}
		}
		used[best] = true;
		placed.push_back({ pool[best], static_cast<uint8_t>(sides[p] & 1) });
		out[p] = pool[best];
	}
	return out;
}
// 2.3.0 (user): respawn away from the enemies and not where you went down (the downed player still stands there).
// The level's spawns spots first; start_spawns only if every spawns spot is rejected. Tiers: 2 = clear of both radii,
// 1 = clear of the death spot only, 0 = neither; within a tier the spot farthest from the nearest enemy. Never fails.
auto PickRespawnSpot(void* game, void* unit, int32_t level, int32_t ox, int32_t oy, Spot* out) -> bool {
	const uint32_t        guid = Field<uint32_t>(unit, 0x08);
	std::vector<uint32_t> foes;
	bool                  inDuel = false;
	{
		std::lock_guard lock(g_pvpLock);
		if (const Duel* d = DuelOfLocked(guid); d != nullptr && d->level == level) {
			inDuel = true;
			for (uint32_t m : Members(*d)) {
				if (m != guid && (d->team.empty() || TeamRelationLocked(guid, m) == 2)) {
					foes.push_back(m);
				}
			}
		}
	}
	if (!inDuel) { // a free fight: every other player in the level
		int n = 0;
		for (void* c = Field<void*>(game, kGameClients); c != nullptr && n < 64; c = Field<void*>(c, kClientNext), ++n) {
			if (Field<uint32_t>(c, kClientUnitType) == kUnitPlayer && Field<uint32_t>(c, kClientUnitGuid) != guid) {
				foes.push_back(Field<uint32_t>(c, kClientUnitGuid));
			}
		}
	}
	std::vector<std::array<int32_t, 2>> foePos;
	for (uint32_t g : foes) {
		if (void* f = g_serverUnit(game, kUnitPlayer, g); f != nullptr && LevelOf(f) == level && !IsDownPlayer(f)) {
			int32_t xy[2] {};
			g_getCoords(f, xy);
			foePos.push_back({ xy[0], xy[1] });
		}
	}
	int32_t me[2] {};
	g_getCoords(unit, me);
	const int64_t rDie = static_cast<int64_t>(g_pvp.respawnAvoidDeath) * g_pvp.respawnAvoidDeath;
	const int64_t rFoe = static_cast<int64_t>(g_pvp.respawnAvoidEnemy) * g_pvp.respawnAvoidEnemy;
	struct Pick {
		int     tier = -1;
		int64_t foe  = -1;
		Spot    spot {};
	};
	const auto bestOf = [&](const std::vector<Spot>& spots) {
		Pick p;
		for (const Spot& s : spots) {
			const int64_t x = ox + s.x, y = oy + s.y;
			const int64_t die = (x - me[0]) * (x - me[0]) + (y - me[1]) * (y - me[1]);
			int64_t       foe = INT64_MAX;
			for (const auto& f : foePos) {
				foe = std::min(foe, (x - f[0]) * (x - f[0]) + (y - f[1]) * (y - f[1]));
			}
			const int tier = die >= rDie ? (foe >= rFoe ? 2 : 1) : 0;
			if (tier > p.tier || (tier == p.tier && foe > p.foe)) {
				p = { tier, foe, s };
			}
		}
		return p;
	};
	Pick pick;
	if (const auto it = g_pvp.spawns.find(level); it != g_pvp.spawns.end()) {
		pick = bestOf(it->second);
	}
	if (pick.tier < 2) { // only a better tier from start_spawns beats the original spots (user: respawns favour them)
		if (const auto it = g_pvp.startSpawns.find(level); it != g_pvp.startSpawns.end()) {
			if (const Pick extra = bestOf(it->second); extra.tier > pick.tier) {
				pick = extra;
			}
		}
	}
	if (pick.tier < 0) {
		return false;
	}
	const int     bestTier = pick.tier;
	const int64_t bestFoe  = pick.foe;
	const Spot    best     = pick.spot;
	*out = best;
	Log("arena: respawn spot for %u: %d,%d (tier %d, nearest enemy %.0f subtiles).", guid, best.x, best.y, bestTier,
	    bestFoe == INT64_MAX ? -1.0 : std::sqrt(static_cast<double>(bestFoe)));
	return true;
}
void EnterArena(void* game, void* player, int32_t arena, int spot) {
	g_warpUnit(game, player, static_cast<uint32_t>(arena), 0);
	int32_t    level = 0, ox = 0, oy = 0;
	const auto it    = g_pvp.spawns.find(arena);
	if (ArenaOf(player, &level, &ox, &oy) != nullptr && level == arena && it != g_pvp.spawns.end() && !it->second.empty()) {
		const size_t i = spot >= 0 && static_cast<size_t>(spot) < it->second.size()
		                   ? static_cast<size_t>(spot) : std::uniform_int_distribution<size_t>(0, it->second.size() - 1)(Rng());
		const Spot& s = it->second[i];
		(void)Teleport(game, player, level, ox + s.x, oy + s.y);
	}
	if (g_changeMode2 != nullptr) {
		g_changeMode2(game, player, nullptr, kPlrModeNeutral, 0, 0, 1);
	}
	Log("arena: player %u took the entrance to level %d (now in level %d).", Field<uint32_t>(player, 0x08), arena, LevelOf(player));
}
// Out of an arena: next to the entrance trapdoor of that arena (or the entrance level's start tile).
void LeaveArena(void* game, void* player, int32_t arena) {
	ReleasePets(game, Field<uint32_t>(player, 0x08), false);
	g_warpUnit(game, player, static_cast<uint32_t>(g_config.entranceLevel), 0);
	for (const Entrance& e : g_config.entrances) {
		if (e.arena == arena) {
			(void)Teleport(game, player, g_config.entranceLevel, e.x, e.y + 3);
			break;
		}
	}
	if (g_changeMode2 != nullptr) {
		g_changeMode2(game, player, nullptr, kPlrModeNeutral, 0, 0, 1);
	}
	Log("arena: player %u left level %d by its exit trapdoor (now in level %d).", Field<uint32_t>(player, 0x08), arena, LevelOf(player));
}

// Last player in an arena (game thread): countdown once they are up, cancelled if they leave or someone joins; then
// LEVEL_WarpUnit to their last act town (any act; LEVEL_ChangeAct path, notes/arena-research/match-flow.md section 1).
void ProcessReturns(void* game, int32_t frame) {
	std::vector<PendingReturn> work;
	{
		std::lock_guard lock(g_pvpLock);
		work = g_returns;
	}
	if (work.empty()) {
		return;
	}
	std::vector<PendingReturn> keep;
	for (PendingReturn r : work) {
		void* u = g_serverUnit(game, kUnitPlayer, r.guid);
		int32_t others = 0;
		int     n      = 0;
		for (void* c = Field<void*>(game, kGameClients); c != nullptr && n < 64; c = Field<void*>(c, kClientNext), ++n) {
			const uint32_t g = Field<uint32_t>(c, kClientUnitGuid);
			if (Field<uint32_t>(c, kClientUnitType) == kUnitPlayer && g != r.guid) {
				if (void* o = g_serverUnit(game, kUnitPlayer, g); o != nullptr && LevelOf(o) == r.level) {
					++others;
				}
			}
		}
		if (u == nullptr || g_client(u) == nullptr || LevelOf(u) != r.level || (others > 0 && !r.duelEnd)) {
			if (r.told && u != nullptr) {
				TellReturn(game, u, r.guid, 0);
			}
			Log("arena: return to town for player %u cancelled (%s).", r.guid, others > 0 ? "someone entered the arena" : "left the arena");
			continue;
		}
		if (IsDown(r.guid)) {
			r.told = false; // wait for the respawn, then count down
			keep.push_back(r);
			continue;
		}
		if (!r.told) {
			const uint32_t ms = static_cast<uint32_t>(g_pvp.returnSeconds * 1000.0 + 0.5);
			r.due  = frame + static_cast<int32_t>(g_pvp.returnSeconds * kFramesPerSecond + 0.5);
			r.told = true;
			TellReturn(game, u, r.guid, ms);
			keep.push_back(r);
			continue;
		}
		if (frame - r.due < 0) {
			keep.push_back(r);
			continue;
		}
		int32_t town = 40; // never stood in a town this game: Lut Gholein (the arena entrances)
		{
			std::lock_guard lock(g_pvpLock);
			if (const auto it = g_lastTown.find(r.guid); it != g_lastTown.end()) {
				town = it->second;
			}
		}
		ReleasePets(game, r.guid, false);
		g_warpUnit(game, u, static_cast<uint32_t>(town), 0);
		if (g_changeMode2 != nullptr) {
			g_changeMode2(game, u, nullptr, kPlrModeNeutral, 0, 0, 1);
		}
		TellReturn(game, u, r.guid, 0);
		Log("arena: player %u was the last one in level %d: sent to town level %d.", r.guid, r.level, town);
	}
	std::lock_guard lock(g_pvpLock);
	// entries queued by SyncHostility meanwhile (same thread, so only from this tick's SyncHostility before us) are in work
	g_returns = keep;
}

std::atomic<uint32_t> g_leaveGuid { 0 }; // console `arena leave` (test only, host): this player leaves the arena next tick

auto IsFrozen(uint32_t guid) -> bool {
	if (g_frozenCount.load(std::memory_order_relaxed) == 0) {
		return false;
	}
	std::lock_guard lock(g_pvpLock);
	return g_frozen.count(guid) != 0;
}
auto ArenaHasDuel(int32_t level) -> bool {
	std::lock_guard lock(g_pvpLock);
	return std::any_of(g_duels.begin(), g_duels.end(), [&](const Duel& d) { return d.level == level; });
}
// 1.0.0: a walk-in duel in its countdown or fight takes walk_in_extras more players. A challenged duel stays 1 v 1
// (the two chose each other, and a stake belongs to them), and so does one that is still loading or already over.
auto DuelJoinable(int32_t level) -> bool {
	if (!g_pvp.duelEnabled || !g_pvp.duelWalkIn || g_pvp.duelMaxPlayers <= 2) {
		return false;
	}
	std::lock_guard lock(g_pvpLock);
	return std::any_of(g_duels.begin(), g_duels.end(), [&](const Duel& d) {
		return d.level == level && d.walkIn && !d.wager && (d.state == kDuelCountdown || d.state == kDuelFighting);
	});
}
void RefuseDuelEntry(void* game, void* player) {
	Refuse(game, player, g_pvp.duelBusyText);
}
void SetFrozen(void* unit, uint32_t guid, bool on) {
	if (unit != nullptr) {
		Freeze(unit, on);
	}
	std::lock_guard lock(g_pvpLock);
	if (on) {
		g_frozen.insert(guid);
	} else {
		g_frozen.erase(guid);
	}
	g_frozenCount = static_cast<int32_t>(g_frozen.size());
}

void ClientHandle(uint16_t id, const void* data, uint32_t size); // client side (network section)
// Host -> one player's client: over the channel, or straight to this process's UI when it is the hosting player.
void SendToPlayer(uint32_t guid, uint16_t id, const void* data, uint32_t size) {
	if (guid != 0 && guid == g_localGuid.load()) {
		ClientHandle(id, data, size);
		return;
	}
	D2RL::Network::PeerHandle peer = D2RL::Network::InvalidPeerHandle;
	{
		std::lock_guard lock(g_pvpLock);
		if (const auto it = g_peers.find(guid); it != g_peers.end()) {
			peer = it->second;
		}
	}
	if (g_net != nullptr && peer != D2RL::Network::InvalidPeerHandle) {
		(void)g_net->sendToClient(g_ctx, g_channel, peer, id, data, size);
	}
}
// Notice line under the scoreboard for ms (0 = clear); seconds = append the remaining seconds. Also a chat line.
void AloneNotice(uint32_t guid, const std::string& text, uint32_t ms); // background notice with seconds (alone section)
DuelRules DefaultRules() {
	DuelRules r;
	r.type         = kTypePoints;
	r.target       = static_cast<uint8_t>(std::clamp(g_pvp.duelPoints, 1, 99));
	r.killWorth    = static_cast<uint8_t>(std::clamp(g_pvp.playerKill, 1, 20));
	r.deathPenalty = static_cast<uint8_t>(std::clamp(-g_pvp.playerDeath, 0, 20));
	r.minutes      = 0;
	r.potionCd     = static_cast<uint8_t>(std::clamp(static_cast<int>(g_pvp.potionCooldown + 0.5), 0, 10));
	r.potionLimit  = 0;
	r.stakes       = 0;
	r.killBonus    = g_pvp.killBonus ? 1 : 0;
	return r;
}
// Rules page arrows: row 0 type, 1 target, 2 kill worth, 3 death penalty, 4 minutes, 5 potion cooldown, 6 potion limit,
// 7 stakes.
void ApplyRule(DuelRules& r, int row, bool more) {
	const auto step = [&](uint8_t& v, int lo, int hi) { v = static_cast<uint8_t>(std::clamp(static_cast<int>(v) + (more ? 1 : -1), lo, hi)); };
	switch (row) {
	case 0:
		r.type = static_cast<uint8_t>((r.type + (more ? 1 : 2)) % 3);
		if (r.type == kTypeTimed && r.minutes == 0) {
			r.minutes = 5;
		}
		if (r.type == kTypeKills && r.target > 30) {
			r.target = 30;
		}
		break;
	case 1: step(r.target, 1, r.type == kTypeKills ? 30 : 99); break;
	case 2: step(r.killWorth, 1, 20); break;
	case 3: step(r.deathPenalty, 0, 20); break;
	case 4: step(r.minutes, r.type == kTypeTimed ? 1 : 0, 30); break;
	case 5: step(r.potionCd, 0, 10); break;
	case 6: step(r.potionLimit, 0, 30); break;
	case 7: r.stakes = r.stakes != 0 ? 0 : 1; break;
	case 8: step(r.teleCd, 0, 10); break;
	case 9: { // 1.8.3: shown, within 40, 30, 20, 10, hidden (right = less visible)
		static constexpr uint8_t kOrder[] = { 1, 6, 5, 4, 3, 2, 0 }; // 1.8.4: in sight after shown
		int at = 0;
		for (int i = 0; i < 7; ++i) {
			if (kOrder[i] == r.mapShow) {
				at = i;
			}
		}
		r.mapShow = kOrder[(at + (more ? 1 : 6)) % 7];
		break;
	}
	case 10: r.killBonus = r.killBonus != 0 ? 0 : 1; break; // 2.2.0
	default: break;
	}
}
int MapRange(const DuelRules& r) { return r.mapShow >= 2 && r.mapShow <= 5 ? (r.mapShow - 1) * 10 : 0; } // subtiles
auto DuelTermsText(const DuelRules& r) -> std::string {
	std::string t = r.type == kTypeKills ? "first to " + std::to_string(r.target) + " kills"
	              : r.type == kTypePoints ? "first to " + std::to_string(r.target) + " points"
	                                      : std::to_string(r.minutes) + " minutes, most points wins";
	if (r.type != kTypeTimed && r.minutes > 0) {
		t += ", " + std::to_string(r.minutes) + " minute limit";
	}
	if (r.teleCd > 0) {
		t += ", Teleport cooldown " + std::to_string(r.teleCd) + " s";
	}
	if (r.mapShow == 0) {
		t += ", opponent hidden on the map";
	} else if (r.mapShow == 6) {
		t += ", opponent on the map only in sight";
	} else if (r.mapShow >= 2) {
		t += ", opponent on the map within " + std::to_string(MapRange(r)) + " squares";
	}
	if (r.killBonus != 0) {
		t += ", kill bonus";
	}
	return t;
}
// 2.4.8 (user 2026-11-09: "Get rid of all above character messages"): no duel / wager notice over the character any
// more - only the clear (empty text) is still sent so an older notice goes; the text goes to chat when chat is set.
void DuelTell(void* game, uint32_t guid, const std::string& text, uint32_t ms, bool seconds, bool chat = true) {
	if (text.empty()) {
		std::string payload(5, '\0');
		payload[4] = seconds ? 1 : 0;
		SendToPlayer(guid, kMsgNotice, payload.data(), static_cast<uint32_t>(payload.size()));
	}
	(void)ms;
	if (chat && !text.empty() && game != nullptr) {
		if (void* u = g_serverUnit(game, kUnitPlayer, guid); u != nullptr) {
			if (void* c = g_client(u); c != nullptr) {
				SendLine(c, guid, text);
			}
		}
	}
}
// The duel result on the player's HUD for the end pause (0.8.5, user 2026-11-07: own text, bigger; winner in gold Exocet).
void SendDuelResult(uint32_t guid, uint8_t kind) {
	uint8_t        msg[5] { kind };
	const uint32_t ms = static_cast<uint32_t>(g_pvp.duelEndSeconds * 1000.0 + 0.5);
	std::memcpy(msg + 1, &ms, 4);
	SendToPlayer(guid, kMsgDuelResult, msg, sizeof(msg));
}
void SendDuelCountdown(uint32_t guid, uint32_t ms) {
	SendToPlayer(guid, kMsgDuelCountdown, &ms, sizeof(ms));
}
// 1.9.3 (user 2026-11-08, TD11): a player who falls in a team sudden death keeps "You are down" centred until the result.
// 1.9.6 (user): only that - no banner for the players still standing (kinds 1 / 2 are not sent any more).
// 1.9.7 (user): kinds 1 / 2 again, but only for kSuddenBannerMs at the start; kind 3 = "You were downed" until the result.
enum SuddenKind : uint8_t { kSuddenOff = 0, kSuddenNextKill = 1, kSuddenLastTeam = 2, kSuddenOut = 3 };
constexpr uint64_t kSuddenBannerMs = 3000;
void SendSudden(uint32_t guid, uint8_t kind) {
	SendToPlayer(guid, kMsgSuddenDeath, &kind, 1);
}

auto LowerAscii(std::string s) -> std::string {
	for (char& c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}
auto FindPlayerByName(void* game, const std::string& name) -> void* {
	if (name.size() > 1 && name[0] == '#') { // 1.2.0: "#<guid>" from the player menu
		return g_serverUnit(game, kUnitPlayer, static_cast<uint32_t>(std::strtoul(name.c_str() + 1, nullptr, 10)));
	}
	const std::string want = LowerAscii(Trim(name));
	int               n    = 0;
	for (void* c = Field<void*>(game, kGameClients); c != nullptr && n < 64; c = Field<void*>(c, kClientNext), ++n) {
		if (Field<uint32_t>(c, kClientUnitType) != kUnitPlayer) {
			continue;
		}
		void* u = g_serverUnit(game, kUnitPlayer, Field<uint32_t>(c, kClientUnitGuid));
		if (u != nullptr && LowerAscii(NameOf(u)) == want) {
			return u;
		}
	}
	return nullptr;
}
// Why this player can't start a duel now ("" = free). Server-side "busy" pieces (D2R has no PLAYER_IsBusy):
// NPC dialog (maybe_PLAYER_D2R_IsInteractingWithNpc), trade (client trade state id != 0), cursor item.
auto BusyReason(void* game, void* unit) -> std::string {
	const uint32_t guid = Field<uint32_t>(unit, 0x08);
	if (IsDown(guid)) {
		return "down";
	}
	{
		std::lock_guard lock(g_pvpLock);
		if (DuelOfLocked(guid) != nullptr) {
			return "already in a duel";
		}
		if (const WagerSession* ws = WagerOfLocked(guid); ws != nullptr) {
			return "in the " + WagerWindowName(*ws); // 2.4.1 / 2.4.2 (user)
		}
		if (InLobbyLocked(guid)) {
			return "in the team duel lobby";
		}
	}
	if (g_npcInteract(unit)) {
		return "talking to someone";
	}
	if (void* c = g_client(unit); c != nullptr && Field<int32_t>(c, kClientTradeState) != 0) {
		return "trading";
	}
	if (void* inv = g_getInventory(unit, __FILE__, __LINE__); inv != nullptr && g_cursorItem(inv) != nullptr) {
		return "holding an item";
	}
	(void)game;
	return "";
}
// An arena level with no players and no duel (config order), 0 = none.
// 2.1.0 (user 2026-11-08: Arena Tomb 2 never came up - the first free arena in level order always won): a random free
// arena; arenas with at least `players` spawn spots (spawns + start_spawns) first, any free one otherwise. 0 = none free.
auto FreeArena(void* game, int players = 2) -> int32_t {
	std::set<int32_t> used;
	int               n = 0;
	for (void* c = Field<void*>(game, kGameClients); c != nullptr && n < 64; c = Field<void*>(c, kClientNext), ++n) {
		if (Field<uint32_t>(c, kClientUnitType) == kUnitPlayer) {
			if (void* u = g_serverUnit(game, kUnitPlayer, Field<uint32_t>(c, kClientUnitGuid)); u != nullptr) {
				used.insert(LevelOf(u));
			}
		}
	}
	std::vector<int32_t> fits, other;
	for (const auto& [level, a] : g_config.arenas) {
		if (used.count(level) == 0 && !ArenaHasDuel(level)) {
			const auto   sp    = g_pvp.spawns.find(level);
			const auto   ss    = g_pvp.startSpawns.find(level);
			const size_t spots = (sp != g_pvp.spawns.end() ? sp->second.size() : 0) + (ss != g_pvp.startSpawns.end() ? ss->second.size() : 0);
			(spots >= static_cast<size_t>(players) ? fits : other).push_back(level);
		}
	}
	const std::vector<int32_t>& pool = fits.empty() ? other : fits;
	if (pool.empty()) {
		return 0;
	}
	return pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(Rng())];
}
// Back to the last act town the player stood in (Lut Gholein if none), as the last-player return.
void WarpHome(void* game, void* unit) {
	const uint32_t guid = Field<uint32_t>(unit, 0x08);
	int32_t        town = 40;
	{
		std::lock_guard lock(g_pvpLock);
		if (const auto it = g_lastTown.find(guid); it != g_lastTown.end()) {
			town = it->second;
		}
	}
	ReleasePets(game, guid, false);
	g_warpUnit(game, unit, static_cast<uint32_t>(town), 0);
	g_changeMode2(game, unit, nullptr, kPlrModeNeutral, 0, 0, 1);
	Log("arena: duel over: player %u sent to town level %d.", guid, town);
}

// ---- gold wagers (0.9.0, user 2026-11-07; design notes/arena-research/wager.md) ----
// `arena duel <name> <gold>`: both must carry the gold in their inventory when challenging and accepting. On accept the
// stake moves from gold (stat 14) into a saved custom stat (arena_stake, itemstatcost row from build_data.py) and every
// player is saved, so a stake can't be spent and survives a crash. The winner gets both stakes into gold, then stash gold,
// the rest into a saved arena_winnings stat that is moved into gold as soon as there is room. A player who leaves the game
// during the countdown or the fight loses: the save hook below settles it before the leaver's file is written. A cancelled
// duel refunds both. A stake on a character with no running duel (host crash) is refunded when it is next seen.
using SaveAllFn   = void(__fastcall*)(void* game, void* player, void* a3, void* a4);
using SaveStampFn = void(__fastcall*)(void* game, void* a2, void* a3, void* a4);
std::atomic<SaveAllFn> g_origSaveAll { nullptr };
SaveStampFn            g_saveStamp = nullptr;
constexpr int32_t      kStatLevel = 12, kStatGold = 14, kStatGoldBank = 15;
constexpr int64_t      kStashGoldLimit = 2500000; // UNITS_GetStashGoldLimit 0x14034B310: MOV EAX, 0x2625A0

auto GoldOf(void* u) -> int64_t {
	return g_readStat(u, kStatGold, 0);
}
void SaveNow(void* game) {
	if (g_saveStamp != nullptr) {
		g_saveStamp(game, nullptr, nullptr, nullptr);
	}
}
// "Bob 5000" -> name "Bob", 5000; no number at the end -> 0 and the whole text as the name.
auto SplitStake(const std::string& text, std::string& name) -> uint32_t {
	const std::string t   = Trim(text);
	const size_t      cut = t.find_last_of(' ');
	if (cut != std::string::npos && cut + 1 < t.size() && t.find_first_not_of("0123456789", cut + 1) == std::string::npos && t.size() - cut - 1 <= 9) {
		name = Trim(t.substr(0, cut));
		return static_cast<uint32_t>(std::strtoul(t.c_str() + cut + 1, nullptr, 10));
	}
	name = t;
	return 0;
}
// Gold into the inventory up to its limit (level x 10000, UNITS_GetInventoryGoldLimit 0x14034B320), then the stash up to
// 2,500,000; returns what did not fit. Plain stat writes as the stock trade does (PLRTRADE_AddGold zeroes over the limit).
auto GiveGold(void* u, int64_t amount) -> int64_t {
	if (amount <= 0) {
		return 0;
	}
	const int64_t gold = GoldOf(u), invMax = static_cast<int64_t>(g_readStat(u, kStatLevel, 0)) * 10000;
	const int64_t a    = std::clamp<int64_t>(invMax - gold, 0, amount);
	if (a > 0) {
		g_setStat(u, kStatGold, static_cast<int32_t>(gold + a), 0);
		amount -= a;
	}
	const int64_t bank = g_readStat(u, kStatGoldBank, 0);
	const int64_t b    = std::clamp<int64_t>(kStashGoldLimit - bank, 0, amount);
	if (b > 0) {
		g_setStat(u, kStatGoldBank, static_cast<int32_t>(bank + b), 0);
		amount -= b;
	}
	return amount;
}
void GiveOrKeep(void* u, int64_t amount) {
	if (const int64_t rest = GiveGold(u, amount); rest > 0) {
		g_setStat(u, g_pvp.winningsStat, static_cast<int32_t>(g_readStat(u, g_pvp.winningsStat, 0) + rest), 0);
		if (void* c = g_client(u); c != nullptr) { // 0.9.3: W8, the player couldn't tell it was held
			SendLine(c, Field<uint32_t>(u, 0x08), std::to_string(rest) + " gold doesn't fit in your inventory or stash. It is held for you and added as soon as there is room.");
		}
	}
}
// A stake back to its owner (cancelled duel, leaving while moving in, orphan after a crash). No save here.
void RefundStake(void* game, uint32_t guid, const char* why) {
	void* u = g_serverUnit(game, kUnitPlayer, guid);
	if (u == nullptr) {
		return;
	}
	const int32_t stake = g_readStat(u, g_pvp.stakeStat, 0);
	if (stake <= 0) {
		return;
	}
	g_setStat(u, g_pvp.stakeStat, 0, 0);
	GiveOrKeep(u, stake);
	if (void* c = g_client(u); c != nullptr) {
		SendLine(c, guid, "Your duel stake of " + std::to_string(stake) + " gold was returned.");
	}
	Log("arena: wager: %d gold stake of player %u returned (%s).", stake, guid, why);
}
// Both stakes to the winner. The caller marked the duel paid under g_pvpLock.
void PayPot(void* game, uint32_t winner, uint32_t loser, bool save) {
	void*   uw = g_serverUnit(game, kUnitPlayer, winner);
	void*   ul = g_serverUnit(game, kUnitPlayer, loser);
	int64_t pot = 0, lost = 0;
	if (ul != nullptr) {
		lost = g_readStat(ul, g_pvp.stakeStat, 0);
		g_setStat(ul, g_pvp.stakeStat, 0, 0);
	}
	if (uw == nullptr) { // the winner is gone: nobody to pay, the loser keeps their own
		if (ul != nullptr) {
			GiveOrKeep(ul, lost);
		}
		Log("arena: wager: winner %u is gone; %lld gold back to %u.", winner, static_cast<long long>(lost), loser);
	} else {
		pot = lost + g_readStat(uw, g_pvp.stakeStat, 0);
		g_setStat(uw, g_pvp.stakeStat, 0, 0);
		GiveOrKeep(uw, pot);
		Broadcast(game, NameOf(uw) + " wins " + std::to_string(pot) + " gold.");
		Log("arena: wager: %u wins %lld gold (%lld from %u).", winner, static_cast<long long>(pot), static_cast<long long>(lost), loser);
	}
	if (save) {
		SaveNow(game);
	}
}
// ---- 1.4.0 item stakes: native item movers (notes/arena-research/wager.md R2.2-R2.5, read 2026-11-08) ----
using CaptureLocFn  = void*(__fastcall*)(void* out16, void* item);                         // ITEMS_CaptureItemLocation 0x140382D20
using MakeStoredFn  = void*(__fastcall*)(void* out16, uint32_t page, uint32_t packedXY);  // ITEMS_MakeStoredLocation 0x140382E10
using ParentInvFn   = void*(__fastcall*)(void* item);                                     // ITEMS_GetParentInventory 0x14038AC50
using FreePosFn     = int32_t(__fastcall*)(void* inv, void* item, uint32_t record, int16_t* x, int16_t* y, uint8_t page); // INVENTORY_GetFreePosition 0x1403865B0
using CommitMoveFn  = bool(__fastcall*)(void* player, const void* moveSrc, const void* removeLocs, const uint32_t* moveGuids, const uint32_t* removeGuids,
                                         size_t nMove, size_t nRemove, bool flag);         // ITEMS_CommitValidatedMoveAndRemoval 0x140472590
using PlaceItemFn   = bool(__fastcall*)(void* player, const void* dst, const uint32_t* guids, size_t n, bool flag); // ITEMS_PlaceItemForPlayer 0x140471500
using ResyncTradeFn = void(__fastcall*)(void* game, void* p1, void* p2);                  // PLRTRADE_D2R_FinishTradeResync 0x14052C100
using InvRecordFn   = uint32_t(__fastcall*)(void* unit, uint32_t page, uint32_t lod);     // UNITS_GetInventoryRecordId 0x14034A410
using ExecExchangeFn = bool(__fastcall*)(void* game, void* c1, void* p1, void* t1, void* c2, void* p2, void* t2); // maybe_PLRTRADE_D2R_ExecuteExchange
CaptureLocFn  g_captureLoc  = nullptr;
MakeStoredFn  g_makeStored  = nullptr;
ParentInvFn   g_parentInv   = nullptr;
FreePosFn     g_freePos     = nullptr;
CommitMoveFn  g_commitMove  = nullptr;
PlaceItemFn   g_placeItem   = nullptr;
ResyncTradeFn g_resyncTrade = nullptr;
InvRecordFn   g_invRecord   = nullptr;
std::atomic<ExecExchangeFn> g_origExchange { nullptr };
bool          g_itemStakes  = false; // all of the above bound and the exchange hook in
constexpr size_t kGameLodFlag = 0x106; // game byte: != 1 -> the LoD inventory records (Rcv0x54 CtrlClickMoveItem)

// The loser's staked items to the winner, one at a time (removed from the loser, placed in a free cell of the winner's
// inventory; put back if the place fails). An item that is gone, not stored any more, or does not fit stays where it is.
void PayItems(void* game, uint32_t winner, uint32_t loser, bool save) {
	std::vector<uint32_t> items;
	{
		std::lock_guard lock(g_pvpLock);
		if (const auto it = g_stakedItems.find(loser); it != g_stakedItems.end()) {
			items = it->second;
			g_stakedItems.erase(it);
		}
	}
	void* uw = g_serverUnit(game, kUnitPlayer, winner);
	void* ul = g_serverUnit(game, kUnitPlayer, loser);
	if (items.empty() || !g_itemStakes || uw == nullptr || ul == nullptr) {
		if (!items.empty()) {
			Log("arena: wager: %zu staked item(s) of %u not moved (winner or loser gone).", items.size(), loser);
		}
		return;
	}
	void*          invL  = g_getInventory(ul, __FILE__, __LINE__);
	void*          invW  = g_getInventory(uw, __FILE__, __LINE__);
	const uint32_t rec   = g_invRecord(uw, 0, Field<uint8_t>(game, kGameLodFlag) != 1 ? 1 : 0);
	int            moved = 0, kept = 0;
	for (uint32_t guid : items) {
		void*   item = g_serverUnit(game, 4, guid);
		uint8_t src[16] {}, dst[16] {};
		if (item == nullptr || invL == nullptr || g_parentInv(item) != invL) {
			++kept;
			Log("arena: wager: staked item %u is no longer with %u.", guid, loser);
			continue;
		}
		g_captureLoc(src, item);
		int16_t x = 0, y = 0;
		if (Field<uint32_t>(src, 0) != 0 || invW == nullptr || g_freePos(invW, item, rec, &x, &y, 0) == 0) {
			++kept; // equipped / belt / cursor now, or no room
			Log("arena: wager: staked item %u stays with %u (mode %u or no room).", guid, loser, Field<uint32_t>(src, 0));
			continue;
		}
		g_makeStored(dst, 0, (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16) | static_cast<uint16_t>(x));
		if (!g_commitMove(ul, nullptr, src, nullptr, &guid, 0, 1, false)) {
			++kept;
			continue;
		}
		if (!g_placeItem(uw, dst, &guid, 1, false)) {
			if (!g_placeItem(ul, src, &guid, 1, false)) {
				Log("arena: wager: staked item %u could not be placed back!", guid);
			}
			++kept;
			continue;
		}
		++moved;
	}
	g_resyncTrade(game, ul, uw); // both inventories to every client (the stock trade does this after an exchange)
	if (moved > 0 || kept > 0) {
		char line[160] {};
		std::snprintf(line, sizeof(line), "%s wins %d staked item(s)%s.", NameOf(uw).c_str(), moved, kept > 0 ? " (some could not be moved)" : "");
		Broadcast(game, line);
	}
	Log("arena: wager: %d item(s) of %u moved to %u, %d kept.", moved, loser, winner, kept);
	if (save) {
		SaveNow(game);
	}
}

// Hook on maybe_PLRTRADE_D2R_ExecuteExchange (game thread, inside HandleTradeConfirm after both accepted the same
// offers): a marked wager pair takes the offers as stakes and returns true without moving anything, so the stock code
// closes both trade windows as completed (2E 03 + FinishTradeResync). false = the stock cancel (2E 04).
// Trade state (client + 0x6B8): +0xC offered gold, +0x18 offer vector {16-byte entries, item guid first}, +0x20 count;
// +0x48 / +0x50: placements the client made for the partner's items (the stock room check).
bool __fastcall ArenaExecExchange(void* game, void* c1, void* p1, void* t1, void* c2, void* p2, void* t2) {
	const ExecExchangeFn orig = g_origExchange.load(std::memory_order_acquire);
	if (game == nullptr || p1 == nullptr || p2 == nullptr || t1 == nullptr || t2 == nullptr) {
		return orig(game, c1, p1, t1, c2, p2, t2);
	}
	const uint32_t g1 = Field<uint32_t>(p1, 0x08), g2 = Field<uint32_t>(p2, 0x08);
	uint32_t       a = 0;
	DuelRules      markRules = DefaultRules();
	{
		std::lock_guard lock(g_pvpLock);
		const int32_t frame = Field<int32_t>(game, kGameFrame);
		for (auto it = g_tradeWagers.begin(); it != g_tradeWagers.end(); ++it) {
			if (((it->a == g1 && it->b == g2) || (it->a == g2 && it->b == g1)) && frame - it->expires < 0) {
				a         = it->a;
				markRules = it->rules;
				g_tradeWagers.erase(it);
				break;
			}
		}
	}
	if (a == 0) {
		return orig(game, c1, p1, t1, c2, p2, t2); // a normal trade
	}
	{
		const uint8_t zero = 0; // labels back to "trade" after this one
		SendToPlayer(g1, kMsgWagerFlag, &zero, 1);
		SendToPlayer(g2, kMsgWagerFlag, &zero, 1);
	}
	const auto offer = [](void* t, std::vector<uint32_t>& items) {
		const auto*  vec = Field<uint8_t*>(t, 0x18);
		const size_t n   = Field<size_t>(t, 0x20);
		for (size_t i = 0; vec != nullptr && i < n && i < 120; ++i) {
			items.push_back(Field<uint32_t>(vec + i * 16, 0));
		}
		return Field<uint32_t>(t, 0x0C);
	};
	TradeStart ts {};
	std::vector<uint32_t> i1, i2;
	const uint32_t gold1 = offer(t1, i1), gold2 = offer(t2, i2);
	// the stock client places the partner's items before it can accept (+0x48 count); fewer = no room
	const bool room = Field<size_t>(t1, 0x50) >= i2.size() && Field<size_t>(t2, 0x50) >= i1.size();
	std::string bad;
	if (!room) {
		bad = "Not enough room for the other player's stake.";
	} else if (GoldOf(p1) < gold1 || GoldOf(p2) < gold2) {
		bad = "Both players must carry the gold they put up.";
	} else if (g_pvp.wagerMax > 0 && (gold1 > g_pvp.wagerMax || gold2 > g_pvp.wagerMax)) {
		bad = "The highest wager is " + std::to_string(g_pvp.wagerMax) + " gold.";
	}
	if (!bad.empty()) {
		for (uint32_t g : { g1, g2 }) {
			DuelTell(game, g, bad + " The wager duel is off.", 6000, false);
		}
		Log("arena: wager trade of %u and %u refused: %s", g1, g2, bad.c_str());
		return false;
	}
	// 1.4.1 (IS5: a shield equipped in the trade window went back to the inventory): the window's own rearrangements
	// (t+0x60, equip / belt / grid moves) are part of the exchange. Run the stock exchange with the offers, the gold and
	// the partner placements emptied so only those rearrangements happen; restore the counts after.
	{
		const uint32_t gs1 = Field<uint32_t>(t1, 0x0C), gs2 = Field<uint32_t>(t2, 0x0C);
		const size_t   o1 = Field<size_t>(t1, 0x20), o2 = Field<size_t>(t2, 0x20), r1 = Field<size_t>(t1, 0x50), r2 = Field<size_t>(t2, 0x50);
		SetField<uint32_t>(t1, 0x0C, 0), SetField<uint32_t>(t2, 0x0C, 0);
		SetField<size_t>(t1, 0x20, 0), SetField<size_t>(t2, 0x20, 0), SetField<size_t>(t1, 0x50, 0), SetField<size_t>(t2, 0x50, 0);
		const bool ok = orig(game, c1, p1, t1, c2, p2, t2);
		SetField<uint32_t>(t1, 0x0C, gs1), SetField<uint32_t>(t2, 0x0C, gs2);
		SetField<size_t>(t1, 0x20, o1), SetField<size_t>(t2, 0x20, o2), SetField<size_t>(t1, 0x50, r1), SetField<size_t>(t2, 0x50, r2);
		if (!ok) {
			for (uint32_t g : { g1, g2 }) {
				DuelTell(game, g, "The trade window's own item moves failed. The wager duel is off.", 6000, false);
			}
			Log("arena: wager trade of %u and %u: own rearrangements failed.", g1, g2);
			return false;
		}
	}
	ts.a = a, ts.b = a == g1 ? g2 : g1;
	ts.rules = markRules;
	ts.goldA  = a == g1 ? gold1 : gold2, ts.goldB = a == g1 ? gold2 : gold1;
	ts.itemsA = a == g1 ? i1 : i2, ts.itemsB = a == g1 ? i2 : i1;
	{
		std::lock_guard lock(g_pvpLock);
		g_tradeStarts.push_back(ts);
		g_duelBusy = true;
	}
	Log("arena: wager trade of %u and %u accepted: gold %u / %u, items %zu / %zu.", ts.a, ts.b, ts.goldA, ts.goldB, ts.itemsA.size(), ts.itemsB.size());
	QueueTick();
	return true; // nothing moved: the stock code closes both windows as a completed trade
}

// 1.8.1 automap rule (notes/arena-automap.md, RE session 2026-11-08): every live unit marker goes through
// AUTOMAP_GetUnitMarkerAndName 0x1400D78F0 (bool (unit, int* outMarker, int* outNameType); only caller AUTOMAP_RenderUnit),
// HD and legacy alike. Stock draws any other player as marker 5, hostile or not. For a player this client was told to
// hide (kMsgMapHide), report no marker.
using MarkerFn = bool(__fastcall*)(void* unit, int32_t* outMarker, int32_t* outNameType);
// COLLISION_HasLineOfSight 0x14033A960: true = nothing in mask on the line (subtiles); wraps COLLISION_RayTrace with stack copies
using LineOfSightFn = bool(__fastcall*)(int32_t x1, int32_t y1, int32_t x2, int32_t y2, void* room, uint16_t mask);
LineOfSightFn g_lineOfSight = nullptr;
std::atomic<MarkerFn> g_origMarker { nullptr };
bool __fastcall ArenaUnitMarker(void* unit, int32_t* outMarker, int32_t* outNameType) {
	if (unit != nullptr && Field<uint32_t>(unit, 0x00) == kUnitPlayer) {
		bool hide = false, mate = false;
		{
			std::lock_guard lock(g_mapHideLock);
			hide = !g_mapHidden.empty() && g_mapHidden.count(Field<uint32_t>(unit, 0x08)) != 0;
			mate = !g_teamMates.empty() && g_teamMates.count(Field<uint32_t>(unit, 0x08)) != 0;
		}
		if (mate && !hide) {
			// 1.9.3 (user 2026-11-08, TD13): teammates look like party members - marker 1 (party frame of the automap
			// unit sprite) + nameType 0 (party label colour). AUTOMAP_RenderUnit 0x1400D76E0 then draws them only with
			// the automap "show party" option on (DAT_142a2cf98), as stock does for a real party. Dead look (2) stays.
			const bool r = g_origMarker.load(std::memory_order_acquire)(unit, outMarker, outNameType);
			if (r && outMarker != nullptr && *outMarker == 5) {
				*outMarker = 1;
				if (outNameType != nullptr) {
					*outNameType = 0;
				}
			}
			return r;
		}
		if (const int range = g_mapRange.load(std::memory_order_relaxed); hide && range > 0) { // 1.8.3: shown within range
			void* me = g_localPlayer != nullptr ? g_localPlayer(g_localIndex()) : nullptr;
			if (me != nullptr && g_getCoords != nullptr) {
				int32_t a[2] {}, b[2] {};
				g_getCoords(me, a);
				g_getCoords(unit, b);
				if (range == 0xFF) { // 1.8.4: COLLISION_HasLineOfSight from the local player's room (notes/arena-automap-los.md); no room = not in sight
					void* room = g_getRoom != nullptr ? g_getRoom(me) : nullptr;
					hide       = g_lineOfSight != nullptr && (room == nullptr || !g_lineOfSight(a[0], a[1], b[0], b[1], room, static_cast<uint16_t>(g_pvp.mapSightMask)));
				} else {
					const int64_t dx = a[0] - b[0], dy = a[1] - b[1];
					hide = dx * dx + dy * dy > static_cast<int64_t>(range) * range;
				}
			}
		}
		if (hide) {
			if (outMarker != nullptr) {
				*outMarker = -1;
			}
			if (outNameType != nullptr) {
				*outNameType = -1;
			}
			return false;
		}
	}
	return g_origMarker.load(std::memory_order_acquire)(unit, outMarker, outNameType);
}
// 2.2.1 aura gate (user 2026-11-08): a pet / merc held by the arena (owner down, or a kept pet "killed") keeps its aura
// overlay but the aura must not work. Every aura's server tick is a skills.txt srvdofunc run through gpSkillSrvDoFnTable
// (RVA 0x238EA00, 8-byte pointers; skill-ext uses the same table). Aura rows (aura = 1) use slots 65 (basic auras:
// Might, Prayer, Thorns, Defiance, resists, Concentration, Fanaticism, Druid spirits ...), 66 (Holy Fire / Shock,
// Sanctuary, Conviction, merc pierce auras), 81 (Holy Freeze) and 82 (Redemption). Slot 65 = SKILLS_SrvDo065_BasicAura
// 0x140563D90 (read 2026-11-08: it applies the aura to every unit in range via SKILLS_AuraCallback_BasicAura); slots 66 /
// 81 / 82 held 0x1405630E0 / 0x140563720 / 0x140564260 in the dump. Signature (D2MOO SrvDo): int (pGame, pUnit, nSkillId,
// nSkillLevel). A held pet's tick returns 0 at once: nothing is applied, the aura states on others run out by themselves.
using SrvDoFn = int32_t(__fastcall*)(void* game, void* unit, int32_t skill, int32_t level);
constexpr uint64_t kRvaSrvDoTable = 0x238EA00;
struct AuraSlot {
	uint32_t slot;
	uint64_t rva; // the stock function (expected slot value)
};
constexpr AuraSlot kAuraSlots[] = { { 65, 0x563D90 }, { 66, 0x5630E0 }, { 81, 0x563720 }, { 82, 0x564260 } };
std::array<std::atomic<SrvDoFn>, 4> g_origAuraDo {};
std::atomic<uint32_t> g_auraGated { 0 }; // ticks skipped (console `arena`)
template <int I>
int32_t __fastcall ArenaAuraDo(void* game, void* unit, int32_t skill, int32_t level) {
	if (g_heldSnapN.load(std::memory_order_relaxed) != 0 && unit != nullptr && Field<uint32_t>(unit, 0x00) == kUnitMonster
	    && IsHeldPetGuid(Field<uint32_t>(unit, 0x08))) {
		g_auraGated.fetch_add(1, std::memory_order_relaxed);
		return 0;
	}
	return g_origAuraDo[I].load(std::memory_order_acquire)(game, unit, skill, level);
}
void InstallAuraGate(const D2RL::PluginContext* ctx, const D2RL::MutationService* mut) {
	const uint64_t exe = ctx->exeBase;
	const uint64_t wrappers[4] = { reinterpret_cast<uint64_t>(&ArenaAuraDo<0>), reinterpret_cast<uint64_t>(&ArenaAuraDo<1>),
		                           reinterpret_cast<uint64_t>(&ArenaAuraDo<2>), reinterpret_cast<uint64_t>(&ArenaAuraDo<3>) };
	D2RL::Mutations::TransactionHandle tx = D2RL::Mutations::InvalidTransactionHandle;
	if (mut->beginTransaction(ctx, &tx) != D2RL::Mutations::Result::Success) {
		ctx->LogWarn("arena: aura gate: beginTransaction failed; held pets keep their auras.");
		return;
	}
	std::array<std::array<uint8_t, 8>, 4> expected {}, bytes {};
	for (size_t i = 0; i < 4; ++i) {
		const uint64_t original = exe + kAuraSlots[i].rva;
		std::memcpy(expected[i].data(), &original, 8);
		std::memcpy(bytes[i].data(), &wrappers[i], 8);
		const D2RL::Mutations::BytePatchRequest patch {
			.structSize   = D2RL::Mutations::BytePatchRequestSize,
			.rva          = kRvaSrvDoTable + kAuraSlots[i].slot * 8ull,
			.expected     = expected[i].data(),
			.expectedSize = 8,
			.bytes        = bytes[i].data(),
			.size         = 8,
		};
		D2RL::Mutations::OperationHandle op = D2RL::Mutations::InvalidOperationHandle;
		if (mut->stageBytePatch(ctx, tx, &patch, &op) != D2RL::Mutations::Result::Success) {
			mut->cancelTransaction(ctx, tx);
			char msg[180] {};
			std::snprintf(msg, sizeof(msg), "arena: aura gate: server skill slot %u is not the stock function (another plugin?); held pets keep their auras.", kAuraSlots[i].slot);
			ctx->LogWarn(msg);
			return;
		}
		g_origAuraDo[i].store(reinterpret_cast<SrvDoFn>(original), std::memory_order_release);
	}
	D2RL::Mutations::CommitResult result { .structSize = D2RL::Mutations::CommitResultSize };
	if (mut->commit(ctx, tx, &result) != D2RL::Mutations::Result::Success || result.result != D2RL::Mutations::Result::Success) {
		ctx->LogWarn("arena: aura gate: commit failed; held pets keep their auras.");
		return;
	}
	Log("arena: aura gate installed (server skill slots 65, 66, 81, 82): held pets' auras do nothing.");
}
void InstallAutomapHook(const D2RL::PluginContext* ctx, const D2RL::MutationService* mut) {
	static constexpr uint8_t kMarker[] = { 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x49, 0x8B, 0xD8, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xF1 };
	static constexpr uint8_t kSight[]  = { 0x48, 0x83, 0xEC, 0x38, 0x89, 0x4C, 0x24, 0x28, 0x48, 0x8B, 0x4C, 0x24, 0x60 }; // COLLISION_HasLineOfSight
	if (std::memcmp(reinterpret_cast<const void*>(ctx->exeBase + 0x33a960), kSight, sizeof(kSight)) == 0) {
		g_lineOfSight = reinterpret_cast<LineOfSightFn>(ctx->exeBase + 0x33a960);
	} else {
		ctx->LogWarn("arena: automap \"in sight\" off: COLLISION_HasLineOfSight bytes differ (the opponent then shows).");
	}
	D2RL::Mutations::TransactionHandle tx = D2RL::Mutations::InvalidTransactionHandle;
	if (mut->beginTransaction(ctx, &tx) != D2RL::Mutations::Result::Success) {
		return;
	}
	const D2RL::Mutations::InlineHookRequest hook {
		.structSize   = D2RL::Mutations::InlineHookRequestSize,
		.rva          = 0xd78f0,
		.expected     = kMarker,
		.expectedSize = static_cast<uint32_t>(sizeof(kMarker)),
		.target       = reinterpret_cast<void*>(&ArenaUnitMarker),
	};
	D2RL::Mutations::OperationHandle op = D2RL::Mutations::InvalidOperationHandle;
	D2RL::Mutations::CommitResult    result { .structSize = D2RL::Mutations::CommitResultSize };
	void*                            original = nullptr;
	if (mut->stageInlineHook(ctx, tx, &hook, &op) != D2RL::Mutations::Result::Success) {
		mut->cancelTransaction(ctx, tx);
		ctx->LogWarn("arena: automap rule off: AUTOMAP_GetUnitMarkerAndName hook could not be staged.");
		return;
	}
	if (mut->commit(ctx, tx, &result) != D2RL::Mutations::Result::Success || result.result != D2RL::Mutations::Result::Success
	    || mut->getInlineHookOriginal(ctx, tx, op, &original) != D2RL::Mutations::Result::Success || original == nullptr) {
		ctx->LogWarn("arena: automap rule off: hook commit failed.");
		return;
	}
	g_origMarker.store(reinterpret_cast<MarkerFn>(original), std::memory_order_release);
	ctx->LogInfo("arena: automap rule on.");
}

void InstallItemStakes(const D2RL::PluginContext* ctx, const D2RL::MutationService* mut) {
	struct Fn {
		uint64_t       rva;
		const uint8_t* bytes;
		size_t         n;
	};
	static constexpr uint8_t kCapture[]  = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48 };
	static constexpr uint8_t kStored[]   = { 0x33, 0xC0, 0x89, 0x51, 0x04, 0x89, 0x01, 0x89, 0x41, 0x0C, 0x48, 0x8B };
	static constexpr uint8_t kParent[]   = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9 };
	// 2.4.13 (hook conflict #9): INVENTORY_GetFreePosition 0x1403865B0 is hooked by cube-lock (first 18 bytes); arena calls
	// through that hook (its cube-row rule is wanted) and checks only the body at +0x12 (read in Ghidra 2026-11-09):
	// mov rax,[rip+0x2644CFF] (cookie) / xor rax,rsp / mov [rbp-0x10],rax - the same anchor cube-lock's quick_move checks.
	static constexpr uint8_t kFreePosTail[] = { 0x48, 0x8B, 0x05, 0xFF, 0x4C, 0x64, 0x02, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x45, 0xF0 };
	static constexpr uint8_t kCommit[]   = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48 };
	static constexpr uint8_t kPlace[]    = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };
	static constexpr uint8_t kResync[]   = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48 };
	static constexpr uint8_t kRecord[]   = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89 };
	// ExecuteExchange: PUSH RBP / PUSH RBX / PUSH RDI / LEA RBP,[RSP-0x5A40] / MOV EAX,0x5B40 = 17 bytes, no RIP-relative
	// (the next instruction is the stack-probe CALL rel32)
	static constexpr uint8_t kExchange[] = { 0x40, 0x55, 0x53, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xC0, 0xA5, 0xFF, 0xFF, 0xB8, 0x40, 0x5B, 0x00, 0x00 };
	const Fn fns[] = { { 0x382d20, kCapture, sizeof(kCapture) }, { 0x382e10, kStored, sizeof(kStored) }, { 0x38ac50, kParent, sizeof(kParent) },
		               { 0x3865c2, kFreePosTail, sizeof(kFreePosTail) }, { 0x472590, kCommit, sizeof(kCommit) }, { 0x471500, kPlace, sizeof(kPlace) },
		               { 0x52c100, kResync, sizeof(kResync) }, { 0x34a410, kRecord, sizeof(kRecord) } };
	for (const Fn& f : fns) {
		if (!ctx->CheckExpectedBytes(f.rva, f.bytes, static_cast<uint32_t>(f.n))) {
			char msg[128] {};
			std::snprintf(msg, sizeof(msg), "arena: item stakes off: unexpected bytes at RVA 0x%llx.", static_cast<unsigned long long>(f.rva));
			ctx->LogWarn(msg);
			return;
		}
	}
	const uintptr_t exe = ctx->exeBase;
	g_captureLoc  = reinterpret_cast<CaptureLocFn>(exe + 0x382d20);
	g_makeStored  = reinterpret_cast<MakeStoredFn>(exe + 0x382e10);
	g_parentInv   = reinterpret_cast<ParentInvFn>(exe + 0x38ac50);
	g_freePos     = reinterpret_cast<FreePosFn>(exe + 0x3865b0);
	g_commitMove  = reinterpret_cast<CommitMoveFn>(exe + 0x472590);
	g_placeItem   = reinterpret_cast<PlaceItemFn>(exe + 0x471500);
	g_resyncTrade = reinterpret_cast<ResyncTradeFn>(exe + 0x52c100);
	g_invRecord   = reinterpret_cast<InvRecordFn>(exe + 0x34a410);
	D2RL::Mutations::TransactionHandle tx = D2RL::Mutations::InvalidTransactionHandle;
	if (mut->beginTransaction(ctx, &tx) != D2RL::Mutations::Result::Success) {
		ctx->LogWarn("arena: item stakes off: transaction failed.");
		return;
	}
	const D2RL::Mutations::InlineHookRequest hook {
		.structSize   = D2RL::Mutations::InlineHookRequestSize,
		.rva          = 0x52c5d0,
		.expected     = kExchange,
		.expectedSize = static_cast<uint32_t>(sizeof(kExchange)),
		.target       = reinterpret_cast<void*>(&ArenaExecExchange),
	};
	D2RL::Mutations::OperationHandle op = D2RL::Mutations::InvalidOperationHandle;
	D2RL::Mutations::CommitResult    result { .structSize = D2RL::Mutations::CommitResultSize };
	void*                            original = nullptr;
	if (mut->stageInlineHook(ctx, tx, &hook, &op) != D2RL::Mutations::Result::Success) {
		mut->cancelTransaction(ctx, tx);
		ctx->LogWarn("arena: item stakes off: ExecuteExchange hook could not be staged (another plugin?).");
		return;
	}
	if (mut->commit(ctx, tx, &result) != D2RL::Mutations::Result::Success || result.result != D2RL::Mutations::Result::Success
	    || mut->getInlineHookOriginal(ctx, tx, op, &original) != D2RL::Mutations::Result::Success || original == nullptr) {
		ctx->LogWarn("arena: item stakes off: hook commit failed.");
		return;
	}
	g_origExchange.store(reinterpret_cast<ExecExchangeFn>(original), std::memory_order_release);
	g_itemStakes = true;
	ctx->LogInfo("arena: item stakes on (player menu Duel = a wager through the trade window).");
}

// Every second (host): orphan stakes back, winnings into gold when there is room.
void ProcessWagers(void* game) {
	if (!g_pvp.wagerEnabled) {
		return;
	}
	bool changed = false;
	int  n       = 0;
	for (void* c = Field<void*>(game, kGameClients); c != nullptr && n < 64; c = Field<void*>(c, kClientNext), ++n) {
		if (Field<uint32_t>(c, kClientUnitType) != kUnitPlayer) {
			continue;
		}
		const uint32_t guid = Field<uint32_t>(c, kClientUnitGuid);
		void*          u    = g_serverUnit(game, kUnitPlayer, guid);
		if (u == nullptr) {
			continue;
		}
		if (g_readStat(u, g_pvp.stakeStat, 0) > 0) {
			bool held = false;
			{
				std::lock_guard lock(g_pvpLock);
				const Duel* d = DuelOfLocked(guid);
				held          = d != nullptr && d->stake > 0 && !d->paid;
			}
			if (!held) {
				RefundStake(game, guid, "no running duel");
				changed = true;
			}
		}
		if (const int32_t w = g_readStat(u, g_pvp.winningsStat, 0); w > 0) {
			const int64_t rest = GiveGold(u, w);
			if (rest != w) {
				g_setStat(u, g_pvp.winningsStat, static_cast<int32_t>(rest), 0);
				Log("arena: wager: %lld gold of stored winnings moved to player %u (%lld left).", static_cast<long long>(w - rest), guid,
				    static_cast<long long>(rest));
				{ // c = this player's client (the loop)
					SendLine(c, guid, std::to_string(w - rest) + " gold of your held winnings was added"
					                      + (rest > 0 ? " (" + std::to_string(rest) + " still held)." : std::string(".")));
				}
				changed = true;
			}
		}
	}
	if (changed) {
		SaveNow(game);
	}
}
// PLRSAVE_SaveAllPlayers 0x14041E0D0 (game, player): maybe_GAME_DisconnectClient 0x140405EF0 passes the leaving player
// (call at 0x140405F5D), its other three callers pass 0 (read 2026-11-07). A leaver with a stake is settled here, before
// their file is written: during the countdown or the fight it is a loss, before that a refund.
void __fastcall ArenaSaveAll(void* game, void* player, void* a3, void* a4) {
	bool itemWager = false; // 1.4.0: an item-only stake has no stake stat
	if (g_pvp.wagerEnabled && game != nullptr && player != nullptr && Field<uint32_t>(player, 0x00) == kUnitPlayer) {
		std::lock_guard lock(g_pvpLock);
		const Duel* d = DuelOfLocked(Field<uint32_t>(player, 0x08));
		itemWager     = d != nullptr && d->wager && !d->paid;
	}
	if (g_pvp.wagerEnabled && game != nullptr && player != nullptr && Field<uint32_t>(player, 0x00) == kUnitPlayer
	    && (g_readStat(player, g_pvp.stakeStat, 0) > 0 || itemWager)) {
		const uint32_t guid = Field<uint32_t>(player, 0x08);
		uint32_t       other = 0;
		bool           lose  = false;
		{
			std::lock_guard lock(g_pvpLock);
			if (Duel* d = DuelOfLocked(guid); d != nullptr && d->wager && !d->paid) {
				if (d->state == kDuelCountdown || d->state == kDuelFighting) {
					d->paid = true;
					lose    = true;
					other   = d->a == guid ? d->b : d->a;
				}
			}
		}
		if (lose) {
			Log("arena: wager: player %u leaves the game during the duel: the stake goes to %u.", guid, other);
			PayPot(game, other, guid, false);
			PayItems(game, other, guid, false); // before the leaver's file is written
		} else {
			RefundStake(game, guid, "leaving the game");
		}
	}
	if (const SaveAllFn orig = g_origSaveAll.load(std::memory_order_acquire); orig != nullptr) {
		orig(game, player, a3, a4);
	}
}

// levels.txt Act of a level (vanilla id ranges, excel_txt; the arenas are act 2 rows).
auto ActOfLevel(int32_t level) -> int32_t {
	if (IsPvpLevel(level)) {
		return 1;
	}
	return level < 40 ? 0 : level < 75 ? 1 : level < 103 ? 2 : level < 109 ? 3 : 4;
}
void StartDuel(void* game, void* ua, void* ub, int32_t level, int32_t frame, bool walkIn = false, uint32_t stakeA = 0, uint32_t stakeB = 0, bool items = false,
               const DuelRules* rules = nullptr) {
	const uint32_t a = Field<uint32_t>(ua, 0x08), b = Field<uint32_t>(ub, 0x08);
	Duel           d { a, b, NameOf(ua), NameOf(ub), level, kDuelMoving, frame + static_cast<int32_t>(g_pvp.duelMoveTimeout * kFramesPerSecond) };
	d.walkIn   = walkIn;
	d.crossA   = ActOfLevel(LevelOf(ua)) != ActOfLevel(level);
	d.crossB   = ActOfLevel(LevelOf(ub)) != ActOfLevel(level);
	d.crossAct = d.crossA || d.crossB;
	d.stake    = stakeA + stakeB; // the pot (1.1.0: each side's own stake; > 0 = a wager duel)
	d.wager    = d.stake > 0 || items;
	d.rules       = rules != nullptr ? *rules : DefaultRules();
	d.customRules = rules != nullptr;
	const uint32_t stake = d.stake;
	{
		std::lock_guard lock(g_pvpLock);
		g_invites.erase(std::remove_if(g_invites.begin(), g_invites.end(), [&](const Invite& i) { return i.from == a || i.to == a || i.from == b || i.to == b; }),
		                g_invites.end());
		g_returns.erase(std::remove_if(g_returns.begin(), g_returns.end(), [&](const PendingReturn& r) { return r.guid == a || r.guid == b; }), g_returns.end());
		g_duels.push_back(d);
		g_duelBusy = true;
		g_duelLoaded.erase(a);
		g_duelLoaded.erase(b);
	}
	if (stake > 0) { // the duel is in g_duels now: from here on a stake on either is held, not an orphan
		for (const auto& [u, own] : { std::pair { ua, stakeA }, std::pair { ub, stakeB } }) {
			if (own > 0) {
				g_setStat(u, kStatGold, static_cast<int32_t>(GoldOf(u) - own), 0);
				g_setStat(u, g_pvp.stakeStat, static_cast<int32_t>(g_readStat(u, g_pvp.stakeStat, 0) + own), 0);
			}
		}
		SaveNow(game);
		Log("arena: wager: %u gold taken from %u and %u from %u.", stakeA, a, stakeB, b);
	}
	char line[200] {};
	const std::string terms = DuelTermsText(d.rules);
	if (stake > 0 && stakeA == stakeB) {
		std::snprintf(line, sizeof(line), "Duel: %s vs %s, %s, for %u gold each.", d.nameA.c_str(), d.nameB.c_str(), terms.c_str(), stakeA);
	} else if (stake > 0) {
		std::snprintf(line, sizeof(line), "Duel: %s (%u gold) vs %s (%u gold), %s; the winner takes %u gold.", d.nameA.c_str(), stakeA, d.nameB.c_str(), stakeB,
		              terms.c_str(), stake);
	} else {
		std::snprintf(line, sizeof(line), "Duel: %s vs %s, %s.", d.nameA.c_str(), d.nameB.c_str(), terms.c_str());
	}
	DuelTell(game, a, "", 0, false, false); // clear the invite notices
	DuelTell(game, b, "", 0, false, false);
	if (d.customRules && d.rules.mapShow != 1) { // 1.8.0: each duellist's client hides the other on its automap (1.8.3: or beyond a range)
		for (const auto& [to, other] : { std::pair { a, b }, std::pair { b, a } }) {
			uint8_t msg[5] { static_cast<uint8_t>(d.rules.mapShow == 0 ? 1 : d.rules.mapShow == 6 ? 0xFF : MapRange(d.rules)) }; // 0xFF = in sight
			std::memcpy(msg + 1, &other, 4);
			SendToPlayer(to, kMsgMapHide, msg, sizeof(msg));
		}
	}
	for (uint32_t g : { a, b }) {
		if (void* u = g_serverUnit(game, kUnitPlayer, g); u != nullptr) {
			if (void* c = g_client(u); c != nullptr) {
				SendLine(c, g, line);
			}
		}
	}
	if (!walkIn) {
		// 2.3.0: AssignStartSpots (spawns + start_spawns, as far apart as the spots allow)
		const std::vector<Spot> at = AssignStartSpots(level, { 0, 1 });
		EnterArenaAt(game, ua, level, at.size() == 2 ? &at[0] : nullptr);
		EnterArenaAt(game, ub, level, at.size() == 2 ? &at[1] : nullptr);
	}
	const int32_t checkA[2] { level, d.crossA ? 1 : 0 }, checkB[2] { level, d.crossB ? 1 : 0 };
	SendToPlayer(a, kMsgDuelLoadCheck, checkA, sizeof(checkA));
	SendToPlayer(b, kMsgDuelLoadCheck, checkB, sizeof(checkB));
	Log("arena: duel %u vs %u started in level %d (%s).", a, b, level, walkIn ? "walk-in" : "moving");
}

// ---- 1.1.0 wager window, host side ----
// Sends one side's view. open: 0 closed, 1 open, 2 closed because the duel starts.
void SendWagerState(const WagerSession& w, int me, uint8_t open, const std::string& note = std::string()) {
	const WagerSide& my = w.side[me];
	const WagerSide& th = w.side[me ^ 1];
	WagerStateMsg    m {};
	m.open       = open;
	m.myGold     = my.gold;
	m.theirGold  = th.gold;
	m.myReady    = my.ready ? 1 : 0;
	m.theirReady = th.ready ? 1 : 0;
	m.myItems    = static_cast<uint8_t>(std::min<size_t>(my.items.size(), 255));
	m.theirItems = static_cast<uint8_t>(std::min<size_t>(th.items.size(), 255));
	m.points      = static_cast<uint16_t>(std::clamp(g_pvp.duelPoints, 0, 65535));
	m.canSet      = me == 0 ? 1 : 0;
	m.rules       = w.rules;
	m.agreeWaitMs = static_cast<uint16_t>(std::clamp((w.agreeFrame - g_wagerFrame) * 1000 / kFramesPerSecond, 0, 60000));
	std::string payload(reinterpret_cast<const char*>(&m), sizeof(m));
	for (const std::string* n : { &my.name, &th.name }) {
		const std::string cut = n->substr(0, 32);
		payload += static_cast<char>(cut.size());
		payload += cut;
	}
	payload += (note.empty() ? my.note : note).substr(0, 160);
	SendToPlayer(my.guid, kMsgWagerState, payload.data(), static_cast<uint32_t>(payload.size()));
}
void SendWagerBoth(const WagerSession& w, uint8_t open = 1) {
	SendWagerState(w, 0, open);
	SendWagerState(w, 1, open);
}
void OpenWager(void* game, void* ua, void* ub) {
	WagerSession w;
	w.rules        = DefaultRules();
	w.lastFrame    = Field<int32_t>(game, kGameFrame);
	g_wagerFrame   = w.lastFrame;
	w.agreeFrame   = w.lastFrame;
	w.side[0].guid = Field<uint32_t>(ua, 0x08), w.side[0].name = NameOf(ua);
	w.side[1].guid = Field<uint32_t>(ub, 0x08), w.side[1].name = NameOf(ub);
	{
		std::lock_guard lock(g_pvpLock);
		g_wagerSessions.push_back(w);
		g_duelBusy = true;
	}
	DuelTell(game, w.side[0].guid, "", 0, false, false); // clear the invite notices
	DuelTell(game, w.side[1].guid, "", 0, false, false);
	SendWagerBoth(w);
	Log("arena: wager window opened for %u and %u.", w.side[0].guid, w.side[1].guid);
}
// One side's offer is checked against what it carries and the limit; the note says what is wrong.
auto WagerNote(void* game, const WagerSide& s) -> std::string {
	void* u = g_serverUnit(game, kUnitPlayer, s.guid);
	if (g_pvp.wagerMax > 0 && s.gold > g_pvp.wagerMax) {
		return "The highest wager is " + std::to_string(g_pvp.wagerMax) + " gold.";
	}
	if (u != nullptr && GoldOf(u) < s.gold) {
		return "You don't carry " + std::to_string(s.gold) + " gold.";
	}
	return std::string();
}
void CloseWagerFor(const WagerSession& w, const std::string& why) {
	for (int i = 0; i < 2; ++i) {
		SendWagerState(w, i, 0, why);
	}
}
// 1.6.0: mark the pair, labels on, and the challenger's client sends the stock trade request (used after the rules page
// and by the old direct kDuelTrade command).
void BeginTradeWager(void* game, void* me, void* other, const DuelRules& rules, int32_t frame) {
	const uint32_t mg = Field<uint32_t>(me, 0x08), og = Field<uint32_t>(other, 0x08);
	{
		std::lock_guard lock(g_pvpLock);
		g_tradeWagers.erase(std::remove_if(g_tradeWagers.begin(), g_tradeWagers.end(),
		                                   [&](const TradeWager& w) { return w.a == mg || w.b == mg || w.a == og || w.b == og; }),
		                    g_tradeWagers.end());
		TradeWager tw { mg, og, frame + static_cast<int32_t>(3600 * kFramesPerSecond), rules, false, frame }; // 2.4.11: the 2 min close is on the wall clock
		tw.startedMs = NowMs();
		g_getCoords(me, tw.posA);
		g_getCoords(other, tw.posB);
		g_tradeWagers.push_back(tw);
		g_duelBusy = true;
	}
	// 2.4.6 (user, screenshot 2026-11-09 06:46: instruction texts made sense with the console commands, not with the menus
	// and the trade window): no "sending the stake trade" / "put your stake in" notices any more; clear the invite notices.
	DuelTell(game, og, "", 0, false, false);
	DuelTell(game, mg, "", 0, false, false);
	const uint8_t on = 1;
	SendToPlayer(mg, kMsgWagerFlag, &on, 1);
	SendToPlayer(og, kMsgWagerFlag, &on, 1);
	SendToPlayer(mg, kMsgStartTrade, &og, sizeof(og));
	Log("arena: %u starts a wager trade with %u.", mg, og);
}

// ---- 1.9.0 team duels, host side ----
// kMsgLobbyState payload; then per member u8 team (255 = none), u8 flags (1 ready, 2 leads, 4 you), u8 name length +
// name; then the note text.
#pragma pack(push, 1)
struct LobbyStateMsg {
	uint8_t   open;        // 0 closed, 1 open, 2 closed because the duel starts
	uint8_t   myTeam;      // kNoTeam = not on a team
	uint8_t   leader;      // this player sets the rules
	uint8_t   myReady;
	uint8_t   sizesOk;     // the teams make an allowed duel
	uint8_t   count;       // members that follow
	uint16_t  agreeWaitMs; // the ready button works after this (2 s after the last change)
	DuelRules rules;
	uint8_t   mix;         // 1.9.1: mixed teams
	uint8_t   revealing;   // 1.9.1: teams drawn, the duel starts in a moment (buttons off)
};
#pragma pack(pop)
void SendLobbyState(const Lobby& l, uint32_t to, uint8_t open, const std::string& note = std::string()) {
	LobbyStateMsg m {};
	int           size[2] {}, draw = 0;
	for (const LobbyMember& x : l.members) {
		if (x.team < 2) {
			++size[x.team];
		}
		draw += x.team == kInDraw ? 1 : 0;
		if (x.guid == to) {
			m.myTeam  = x.team;
			m.myReady = x.ready ? 1 : 0;
		}
	}
	m.open        = open;
	m.leader      = !l.members.empty() && l.members[0].guid == to ? 1 : 0;
	m.sizesOk     = (l.mix ? draw >= 2 && draw <= 2 * kLobbyTeamMax : TeamSizesOk(size[0], size[1])) ? 1 : 0;
	m.mix         = l.mix ? 1 : 0;
	m.revealing   = l.revealFrame != 0 ? 1 : 0;
	m.count       = static_cast<uint8_t>(std::min<size_t>(l.members.size(), 16));
	m.agreeWaitMs = static_cast<uint16_t>(std::clamp((l.agreeFrame - g_wagerFrame) * 1000 / kFramesPerSecond, 0, 60000));
	m.rules       = l.rules;
	std::string payload(reinterpret_cast<const char*>(&m), sizeof(m));
	for (size_t i = 0; i < m.count; ++i) {
		const LobbyMember& x = l.members[i];
		payload += static_cast<char>(x.team);
		payload += static_cast<char>((x.ready ? 1 : 0) | (i == 0 ? 2 : 0) | (x.guid == to ? 4 : 0));
		const std::string cut = x.name.substr(0, 32);
		payload += static_cast<char>(cut.size());
		payload += cut;
	}
	payload += note.substr(0, 200);
	SendToPlayer(to, kMsgLobbyState, payload.data(), static_cast<uint32_t>(payload.size()));
}
void SendLobbyAll(const Lobby& l, const std::string& note = std::string()) {
	for (const LobbyMember& x : l.members) {
		SendLobbyState(l, x.guid, 1, note);
	}
}
auto NearLobby(void* unit) -> bool {
	if (unit == nullptr || LevelOf(unit) != g_config.entranceLevel) {
		return false;
	}
	int32_t xy[2] {};
	g_getCoords(unit, xy);
	for (const auto& [x, y] : LobbySpots()) { // 1.9.3: near any of the obelisks
		const int64_t dx = xy[0] - x, dy = xy[1] - y;
		if (dx * dx + dy * dy <= static_cast<int64_t>(g_config.lobbyRange) * g_config.lobbyRange) {
			return true;
		}
	}
	return false;
}
void QueueLobbyOpen(uint32_t guid) { // from the obelisk's operate call: run by the tick, not inside it
	{
		std::lock_guard lock(g_wagerCmdLock);
		g_wagerCmds.push_back({ guid, kLobbyOpen, 0 });
	}
	QueueTick();
}
// Everyone on a team into a free arena: team 1 on one spawn spot, team 2 on another (Teleport finds free ground around it).
void StartTeamDuel(void* game, const std::vector<void*>& teamA, const std::vector<void*>& teamB, int32_t level, int32_t frame, const DuelRules& rules) {
	Duel d { Field<uint32_t>(teamA[0], 0x08), Field<uint32_t>(teamB[0], 0x08), NameOf(teamA[0]), NameOf(teamB[0]), level, kDuelMoving,
		     frame + static_cast<int32_t>(g_pvp.duelMoveTimeout * kFramesPerSecond) };
	d.rules        = rules;
	d.rules.stakes = 0; // user 2026-11-08: no stakes in team duels for now
	d.customRules  = true;
	std::string list[2];
	for (int t = 0; t < 2; ++t) {
		for (void* u : t == 0 ? teamA : teamB) {
			const uint32_t g = Field<uint32_t>(u, 0x08);
			d.team[g]        = static_cast<uint8_t>(t);
			d.names[g]       = NameOf(u);
			if (ActOfLevel(LevelOf(u)) != ActOfLevel(level)) {
				d.cross.insert(g);
			}
			list[t] += (list[t].empty() ? "" : ", ") + d.names[g];
		}
	}
	d.crossAct    = !d.cross.empty();
	d.teamName[0] = "Team One"; // 2.0.0 (user 2026-11-08): always Team One (left) and Team Two (right)
	d.teamName[1] = "Team Two";
	{
		std::lock_guard lock(g_pvpLock);
		g_invites.erase(std::remove_if(g_invites.begin(), g_invites.end(), [&](const Invite& i) { return d.team.count(i.from) != 0 || d.team.count(i.to) != 0; }),
		                g_invites.end());
		g_returns.erase(std::remove_if(g_returns.begin(), g_returns.end(), [&](const PendingReturn& r) { return d.team.count(r.guid) != 0; }), g_returns.end());
		for (const auto& [g, side] : d.team) {
			g_duelLoaded.erase(g);
		}
		g_duels.push_back(d);
		g_duelBusy = true;
	}
	const std::string line = "Team duel: " + list[0] + " vs " + list[1] + ", " + DuelTermsText(d.rules) + ".";
	for (const auto& [g, side] : d.team) {
		DuelTell(game, g, "", 0, false, false);
		if (void* u = g_serverUnit(game, kUnitPlayer, g); u != nullptr) {
			if (void* c = g_client(u); c != nullptr) {
				SendLine(c, g, line);
			}
		}
		if (d.rules.mapShow != 1) { // each player's client hides the other team on its automap (or beyond the range / out of sight)
			std::string msg(1, static_cast<char>(d.rules.mapShow == 0 ? 1 : d.rules.mapShow == 6 ? 0xFF : MapRange(d.rules)));
			for (const auto& [o, oside] : d.team) {
				if (oside != side) {
					msg.append(reinterpret_cast<const char*>(&o), 4);
				}
			}
			SendToPlayer(g, kMsgMapHide, msg.data(), static_cast<uint32_t>(msg.size()));
		}
		{ // 1.9.3: teammates get the party look on the automap
			std::string mates(1, static_cast<char>(1));
			for (const auto& [o, oside] : d.team) {
				if (oside == side && o != g) {
					mates.append(reinterpret_cast<const char*>(&o), 4);
				}
			}
			if (mates.size() > 1) {
				SendToPlayer(g, kMsgTeamMates, mates.data(), static_cast<uint32_t>(mates.size()));
			}
		}
	}
	// 2.3.0 (user): everyone on their own spot (no more teams starting together), see AssignStartSpots
	std::vector<void*>   all;
	std::vector<uint8_t> sides;
	for (int t = 0; t < 2; ++t) {
		for (void* u : t == 0 ? teamA : teamB) {
			all.push_back(u);
			sides.push_back(static_cast<uint8_t>(t));
		}
	}
	const std::vector<Spot> at = AssignStartSpots(level, sides);
	for (size_t i = 0; i < all.size(); ++i) {
		EnterArenaAt(game, all[i], level, at.size() == all.size() ? &at[i] : nullptr);
	}
	for (const auto& [g, side] : d.team) {
		const int32_t check[2] { level, d.cross.count(g) != 0 ? 1 : 0 };
		SendToPlayer(g, kMsgDuelLoadCheck, check, sizeof(check));
	}
	Log("arena: team duel %s vs %s started in level %d (%s).", list[0].c_str(), list[1].c_str(), level, DuelTermsText(d.rules).c_str());
}
// All on a team ready: the lobby closes and the duel starts, or everyone is told why not (lobby kept, ready taken back).
void LobbyStart(void* game, int32_t frame) {
	Lobby l;
	{
		std::lock_guard lock(g_pvpLock);
		l       = g_lobby;
		g_lobby = Lobby {}; // out of the lobby now, so BusyReason doesn't see it
	}
	std::vector<void*> team[2];
	std::string        bad;
	for (const LobbyMember& m : l.members) {
		if (m.team >= 2 || !bad.empty()) {
			continue;
		}
		void* u = g_serverUnit(game, kUnitPlayer, m.guid);
		if (u == nullptr) {
			bad = m.name + " left the game.";
		} else if (const std::string why = BusyReason(game, u); !why.empty()) {
			bad = m.name + " is " + why + ".";
		} else {
			team[m.team].push_back(u);
		}
	}
	for (void* a : team[0]) { // opponents in one party are never made hostile (SyncHostility)
		for (void* b : team[1]) {
			const uint32_t pa = static_cast<uint32_t>(g_partyId(a) & 0xFFFF), pb = static_cast<uint32_t>(g_partyId(b) & 0xFFFF);
			if (bad.empty() && pa != 0xFFFF && pa == pb) {
				bad = NameOf(a) + " and " + NameOf(b) + " are in one party: leave it before a team duel.";
			}
		}
	}
	const int na = static_cast<int>(team[0].size()), nb = static_cast<int>(team[1].size());
	if (bad.empty() && !(l.mix ? MixSizesOk(na, nb) : TeamSizesOk(na, nb))) {
		bad = "The teams don't make an allowed duel.";
	}
	const int32_t level = bad.empty() ? FreeArena(game, na + nb) : 0;
	if (bad.empty() && level == 0) {
		bad = "All arenas are in use. Try again later.";
	}
	if (!bad.empty()) {
		for (LobbyMember& m : l.members) {
			m.ready = false;
			if (l.mix && m.team < 2) {
				m.team = kInDraw; // mixed: drawn again next time
			}
		}
		l.revealFrame = 0;
		{
			std::lock_guard lock(g_pvpLock);
			g_lobby = l;
		}
		SendLobbyAll(l, bad);
		for (const LobbyMember& m : l.members) {
			DuelTell(game, m.guid, bad, 6000, false);
		}
		Log("arena: team duel not started: %s", bad.c_str());
		return;
	}
	for (const LobbyMember& m : l.members) {
		SendLobbyState(l, m.guid, m.team < 2 ? 2 : 0, m.team < 2 ? std::string() : "The team duel started.");
	}
	StartTeamDuel(game, team[0], team[1], level, frame, l.rules);
}
// A lobby command (game thread): open (clicked the obelisk), join a team, ready, a rule (the leader), close.
void LobbyCmd(void* game, const WagerCmd& cmd, int32_t frame) {
	void* u = g_serverUnit(game, kUnitPlayer, cmd.guid);
	if (u == nullptr) {
		return;
	}
	std::string refuse;
	if (cmd.kind == kLobbyOpen) {
		bool member = false;
		{
			std::lock_guard lock(g_pvpLock);
			member = InLobbyLocked(cmd.guid);
		}
		if (!member) {
			const std::string why = BusyReason(game, u);
			refuse                = !why.empty() ? "You are " + why + "." : !NearLobby(u) ? "Come closer to the obelisk." : std::string();
		}
	}
	if (!refuse.empty()) {
		DuelTell(game, cmd.guid, refuse, 4000, false);
		return;
	}
	Lobby       l;
	bool        changed = false, start = false, closed = false;
	std::string note;
	{
		std::lock_guard lock(g_pvpLock);
		Lobby&     lb = g_lobby;
		const auto me = std::find_if(lb.members.begin(), lb.members.end(), [&](const LobbyMember& m) { return m.guid == cmd.guid; });
		const auto unready = [&] {
			for (LobbyMember& m : lb.members) {
				m.ready = false;
				if (lb.mix && m.team < 2) {
					m.team = kInDraw; // a drawn team is undone (someone left during the reveal)
				}
			}
			lb.agreeFrame  = frame + kWagerAgreeSeconds * kFramesPerSecond;
			lb.revealFrame = 0;
		};
		const bool locked = lb.revealFrame != 0 && cmd.kind != kLobbyClose && cmd.kind != kLobbyOpen; // teams drawn: only leaving
		if (cmd.kind == kLobbyOpen) {
			if (me == lb.members.end() && lb.revealFrame != 0) {
				note = "The teams are being drawn - wait a moment.";
			} else if (me == lb.members.end()) {
				if (lb.members.size() >= 16) {
					note = "The lobby is full.";
				} else {
					if (lb.members.empty()) {
						lb.rules        = DefaultRules();
						lb.rules.stakes = 0;
						lb.agreeFrame   = frame;
					}
					lb.members.push_back({ cmd.guid, NameOf(u) });
					changed = true;
				}
			}
		} else if (me != lb.members.end() && !locked) {
			if (cmd.kind == kLobbyJoin && lb.mix && cmd.value < 2) { // 1.9.1: either button joins the draw
				const auto inDraw = std::count_if(lb.members.begin(), lb.members.end(), [](const LobbyMember& m) { return m.team == kInDraw; });
				if (me->team == kInDraw) {
					// already in
				} else if (inDraw >= 2 * kLobbyTeamMax) {
					note = "The draw is full.";
				} else {
					me->team = kInDraw;
					unready();
					changed = true;
				}
			} else if (cmd.kind == kLobbyRule && me == lb.members.begin() && (cmd.value & 0xFF) == 7) { // 1.9.1: row 7 = picked / mixed teams
				lb.mix = !lb.mix;
				for (LobbyMember& m : lb.members) {
					m.team = m.team == kNoTeam ? kNoTeam : lb.mix ? kInDraw : kNoTeam;
				}
				unready();
				changed = true;
			} else if (cmd.kind == kLobbyJoin && cmd.value < 2 && me->team != cmd.value) {
				const auto onTeam = std::count_if(lb.members.begin(), lb.members.end(), [&](const LobbyMember& m) { return m.team == cmd.value; });
				if (onTeam >= kLobbyTeamMax) {
					note = "That team is full.";
				} else {
					me->team = static_cast<uint8_t>(cmd.value);
					unready();
					changed = true;
				}
			} else if (cmd.kind == kLobbyRule && me == lb.members.begin() && (cmd.value & 0xFF) != 7) { // row 7 = stakes: none (1.9.1: the team mode)
				ApplyRule(lb.rules, static_cast<int>(cmd.value & 0xFF), (cmd.value >> 8) != 0);
				lb.rules.stakes = 0;
				unready();
				changed = true;
			} else if (cmd.kind == kLobbyReady && me->team != kNoTeam && frame - lb.agreeFrame >= 0) {
				int size[2] {}, draw = 0;
				for (const LobbyMember& m : lb.members) {
					if (m.team < 2) {
						++size[m.team];
					}
					draw += m.team == kInDraw ? 1 : 0;
				}
				if (lb.mix ? draw >= 2 && draw <= 2 * kLobbyTeamMax : TeamSizesOk(size[0], size[1])) {
					me->ready     = !me->ready;
					changed       = true;
					const bool all = std::all_of(lb.members.begin(), lb.members.end(), [](const LobbyMember& m) { return m.team == kNoTeam || m.ready; });
					if (all && lb.mix) { // 1.9.1: draw the teams (shuffled, half each), show them, start after kRevealSeconds
						std::vector<LobbyMember*> pool;
						for (LobbyMember& m : lb.members) {
							if (m.team == kInDraw) {
								pool.push_back(&m);
							}
						}
						std::shuffle(pool.begin(), pool.end(), Rng());
						for (size_t i = 0; i < pool.size(); ++i) {
							pool[i]->team = i < pool.size() / 2 ? 0 : 1;
						}
						lb.revealFrame = frame + kRevealSeconds * kFramesPerSecond;
						note           = "Teams are drawn - the duel starts in " + std::to_string(kRevealSeconds) + " seconds.";
					} else {
						start = all;
					}
				}
			} else if (cmd.kind == kLobbyClose) {
				lb.members.erase(me);
				unready();
				changed = closed = true;
			}
		}
		l = lb;
	}
	if (closed) {
		Lobby gone;
		SendLobbyState(gone, cmd.guid, 0);
	}
	if (start) {
		Log("arena: team duel lobby: everyone is ready.");
		LobbyStart(game, frame);
		return;
	}
	if (changed) {
		SendLobbyAll(l, note);
	} else if (!note.empty() || cmd.kind == kLobbyOpen) {
		SendLobbyState(l, cmd.guid, 1, note);
	}
	if (cmd.kind == kLobbyOpen && changed) {
		Log("arena: player %u opened the team duel lobby (%zu in it).", cmd.guid, l.members.size());
	}
}
// Members who left the game, the town or walked away from the obelisk are out (game thread, every tick).
void LobbyTick(void* game, int32_t frame) {
	std::vector<uint32_t> out;
	Lobby                 l;
	bool                  revealDone = false;
	{
		std::lock_guard lock(g_pvpLock);
		if (g_lobby.members.empty()) {
			return;
		}
		g_duelBusy = true;
		revealDone = g_lobby.revealFrame != 0 && frame - g_lobby.revealFrame >= 0;
		for (auto it = g_lobby.members.begin(); it != g_lobby.members.end();) {
			void* u = g_serverUnit(game, kUnitPlayer, it->guid);
			if (u == nullptr || !NearLobby(u)) {
				out.push_back(u != nullptr ? it->guid : 0);
				it = g_lobby.members.erase(it);
			} else {
				++it;
			}
		}
		if (out.empty() && revealDone) {
			g_lobby.revealFrame = 0;
		}
		if (out.empty()) {
			l = g_lobby;
		} else {
			for (LobbyMember& m : g_lobby.members) {
				m.ready = false;
				if (g_lobby.mix && m.team < 2) {
					m.team = kInDraw;
				}
			}
			g_lobby.agreeFrame  = frame + kWagerAgreeSeconds * kFramesPerSecond;
			g_lobby.revealFrame = 0;
			l                   = g_lobby;
		}
	}
	if (out.empty()) {
		if (revealDone) {
			LobbyStart(game, frame);
		}
		return;
	}
	const Lobby none;
	for (uint32_t g : out) {
		if (g != 0) {
			SendLobbyState(none, g, 0, "You walked away from the obelisk.");
		}
	}
	SendLobbyAll(l, "A player left the lobby.");
}

void ProcessWagerCmds(void* game, int32_t frame) {
	g_wagerFrame = frame;
	std::vector<WagerCmd> cmds;
	{
		std::lock_guard lock(g_wagerCmdLock);
		cmds.swap(g_wagerCmds);
	}
	for (const WagerCmd& cmd : cmds) {
		if (cmd.kind == kStakeTradeClosed) { // 2.4.10: picked up by ProcessTradeStarts (same tick, runs after this)
			std::lock_guard lock(g_pvpLock);
			for (TradeWager& t : g_tradeWagers) {
				if ((t.a == cmd.guid || t.b == cmd.guid) && t.closedBy == 0) {
					t.closedBy = cmd.guid;
				}
			}
			if (g_pvp.diagnostics) {
				Log("arena: player %u reports the stake trade closed.", cmd.guid);
			}
			continue;
		}
		if (cmd.kind >= kLobbyOpen) { // 1.9.0
			LobbyCmd(game, cmd, frame);
			continue;
		}
		WagerSession w;
		int          me = -1;
		{
			std::lock_guard lock(g_pvpLock);
			if (WagerSession* p = WagerOfLocked(cmd.guid); p != nullptr) {
				w  = *p;
				me = p->side[0].guid == cmd.guid ? 0 : 1;
			}
		}
		if (me < 0) {
			continue;
		}
		WagerSide& my = w.side[me];
		WagerSide& th = w.side[me ^ 1];
		bool       start = false, closed = false;
		w.lastFrame      = frame;
		if (cmd.kind == kWagerClose) {
			closed = true;
		} else if (cmd.kind == kWagerRule) {
			// 1.6.0: the challenger (side 0) sets the rules; a change takes both accepts back and waits 2 s, as the stake did
			if (me == 0) {
				ApplyRule(w.rules, static_cast<int>(cmd.value & 0xFF), (cmd.value >> 8) != 0);
				my.ready = th.ready = false;
				w.agreeFrame        = frame + kWagerAgreeSeconds * kFramesPerSecond;
			}
		} else if (cmd.kind == kWagerOffer) {
			// 1.3.0 (user): one stake, "gold each", set only by the challenger (side 0); any change takes both accepts back
			// and accepting waits kWagerAgreeSeconds
			if (me == 0 && cmd.value != my.gold) {
				my.gold = th.gold = cmd.value;
				my.ready = th.ready = false;
				w.agreeFrame        = frame + kWagerAgreeSeconds * kFramesPerSecond;
			}
		} else if (cmd.kind == kWagerReady) {
			if (frame - w.agreeFrame < 0) {
				// too soon after a change: ignored (the button is off on the client too)
			} else if (const std::string why = WagerNote(game, my); !my.ready && !why.empty()) {
				DuelTell(game, my.guid, why, 5000, false); // no text in the window (user): chat line + notice
			} else {
				my.ready = !my.ready;
			}
			start = my.ready && th.ready;
		}
		{
			std::lock_guard lock(g_pvpLock);
			g_wagerSessions.erase(std::remove_if(g_wagerSessions.begin(), g_wagerSessions.end(),
			                                     [&](const WagerSession& x) { return x.side[0].guid == w.side[0].guid && x.side[1].guid == w.side[1].guid; }),
			                      g_wagerSessions.end());
			if (!closed && !start) {
				g_wagerSessions.push_back(w);
			}
		}
		if (closed) {
			SendWagerState(w, me, 0, "");
			SendWagerState(w, me ^ 1, 0, my.name + " closed the " + WagerWindowName(w) + "."); // 2.4.2 wording
			DuelTell(game, th.guid, my.name + " closed the " + WagerWindowName(w) + ".", 5000, false);
			Log("arena: wager window of %u and %u closed by %u.", w.side[0].guid, w.side[1].guid, cmd.guid);
			continue;
		}
		if (!start) {
			SendWagerBoth(w);
			continue;
		}
		// both accepted the same offers: check again (out of the session now, so BusyReason doesn't see the window)
		void*       ua  = g_serverUnit(game, kUnitPlayer, w.side[0].guid);
		void*       ub  = g_serverUnit(game, kUnitPlayer, w.side[1].guid);
		std::string bad;
		if (ua == nullptr || ub == nullptr) {
			bad = "The other player left the game.";
		} else if (std::string why = BusyReason(game, ua); !why.empty()) {
			bad = w.side[0].name + " is " + why + ".";
		} else if (why = BusyReason(game, ub); !why.empty()) {
			bad = w.side[1].name + " is " + why + ".";
		} else if (!WagerNote(game, w.side[0]).empty() || !WagerNote(game, w.side[1]).empty()) {
			bad = "Both players must carry their stakes.";
		}
		const int32_t level = bad.empty() ? FreeArena(game) : 0;
		if (bad.empty() && level == 0) {
			bad = "All arenas are in use. Try again later.";
		}
		if (!bad.empty()) {
			w.side[0].ready = w.side[1].ready = false;
			if (ua != nullptr && ub != nullptr) {
				std::lock_guard lock(g_pvpLock);
				g_wagerSessions.push_back(w);
			}
			for (int i = 0; i < 2; ++i) {
				SendWagerState(w, i, ua != nullptr && ub != nullptr ? 1 : 0, bad);
				DuelTell(game, w.side[i].guid, bad, 5000, false);
			}
			continue;
		}
		SendWagerBoth(w, 2);
		Log("arena: duel rules of %u and %u accepted (%s, stakes %s).", w.side[0].guid, w.side[1].guid, DuelTermsText(w.rules).c_str(),
		    w.rules.stakes != 0 ? "trade" : "none");
		if (w.rules.stakes != 0) {
			BeginTradeWager(game, ua, ub, w.rules, frame); // 1.6.0: rules first, then the stock trade window for the stakes
		} else {
			StartDuel(game, ua, ub, level, frame, false, 0, 0, false, &w.rules);
		}
	}
	// a player left the game: the window closes for the other; nobody did anything for 2 minutes: it closes for both
	std::vector<std::pair<WagerSession, int>> gone;
	std::vector<WagerSession>                 idle, apart;
	{
		std::lock_guard lock(g_pvpLock);
		for (auto it = g_wagerSessions.begin(); it != g_wagerSessions.end();) {
			if (frame - it->lastFrame > kWagerIdleSeconds * kFramesPerSecond) {
				idle.push_back(*it);
				it = g_wagerSessions.erase(it);
				continue;
			}
			// 1.3.1 (user: like trade): apart by the trade's distance (maybe_PLRTRADE_D2R_CanPlayersTrade: < 0x33) or in
			// another level -> closed for both
			void* u0 = g_serverUnit(game, kUnitPlayer, it->side[0].guid);
			void* u1 = g_serverUnit(game, kUnitPlayer, it->side[1].guid);
			if (u0 != nullptr && u1 != nullptr) {
				int32_t a[2] {}, b[2] {};
				g_getCoords(u0, a);
				g_getCoords(u1, b);
				const int64_t dx = a[0] - b[0], dy = a[1] - b[1];
				if (LevelOf(u0) != LevelOf(u1) || dx * dx + dy * dy >= static_cast<int64_t>(kWagerMaxDistance) * kWagerMaxDistance) {
					apart.push_back(*it);
					it = g_wagerSessions.erase(it);
					continue;
				}
			}
			const bool g0 = g_serverUnit(game, kUnitPlayer, it->side[0].guid) == nullptr;
			const bool g1 = g_serverUnit(game, kUnitPlayer, it->side[1].guid) == nullptr;
			if (g0 || g1) {
				gone.push_back({ *it, g0 ? 1 : 0 });
				it = g_wagerSessions.erase(it);
			} else {
				++it;
			}
		}
		if (!g_wagerSessions.empty()) {
			g_duelBusy = true; // keep the tick running
		}
	}
	for (const auto& [w, stay] : gone) {
		SendWagerState(w, stay, 0, w.side[stay ^ 1].name + " left the game.");
	}
	for (const WagerSession& w : apart) {
		for (int i = 0; i < 2; ++i) {
			SendWagerState(w, i, 0);
			DuelTell(game, w.side[i].guid, "The " + WagerWindowName(w) + " closed: you walked away from each other.", 5000, false);
		}
		Log("arena: wager window of %u and %u closed (apart).", w.side[0].guid, w.side[1].guid);
	}
	for (const WagerSession& w : idle) {
		for (int i = 0; i < 2; ++i) {
			SendWagerState(w, i, 0);
			DuelTell(game, w.side[i].guid, "The " + WagerWindowName(w) + " closed: nothing happened for 2 minutes.", 5000, false);
		}
		Log("arena: wager window of %u and %u closed (idle).", w.side[0].guid, w.side[1].guid);
	}
}

// 1.2.0: the challenged player's Accept / Decline popup (ms 0 closes it).
void SendDuelInvite(uint32_t to, uint32_t ms, bool panel, uint32_t stake, const std::string& from) {
	std::string payload(9, '\0');
	std::memcpy(payload.data(), &ms, 4);
	payload[4] = panel ? 1 : 0;
	std::memcpy(payload.data() + 5, &stake, 4);
	payload += from.substr(0, 32);
	SendToPlayer(to, kMsgDuelInvite, payload.data(), static_cast<uint32_t>(payload.size()));
}

// Tick: accepted wager trades -> duels; expired marks dropped.
void ProcessTradeStarts(void* game, int32_t frame) {
	std::vector<TradeStart> starts;
	std::vector<uint32_t>   expiredMarks;
	{
		std::lock_guard lock(g_pvpLock);
		starts.swap(g_tradeStarts);
		for (const TradeWager& w : g_tradeWagers) {
			if (frame - w.expires >= 0) {
				expiredMarks.push_back(w.a), expiredMarks.push_back(w.b);
			}
		}
		g_tradeWagers.erase(std::remove_if(g_tradeWagers.begin(), g_tradeWagers.end(), [&](const TradeWager& w) { return frame - w.expires >= 0; }),
		                    g_tradeWagers.end());
		if (!g_tradeWagers.empty()) {
			g_duelBusy = true;
		}
	}
	for (uint32_t g : expiredMarks) {
		const uint8_t zero = 0;
		SendToPlayer(g, kMsgWagerFlag, &zero, 1);
	}
	// 2.4.3 (user, screenshot 21:23: walking away during the stake trade told nobody; "X is sending the stake trade" stayed
	// ~30 s): a marked pair that walks apart (the trade distance 0x33 / another level / left the game), or whose stock trade
	// was open and is closed again without the accept (client +0x6B8 trade id back to 0), is cancelled for both.
	struct Off {
		uint32_t    a, b;
		std::string why;
	};
	std::vector<Off> off;
	{
		std::lock_guard lock(g_pvpLock);
		for (TradeWager& w : g_tradeWagers) {
			void* ua = g_serverUnit(game, kUnitPlayer, w.a);
			void* ub = g_serverUnit(game, kUnitPlayer, w.b);
			if (ua == nullptr || ub == nullptr) {
				off.push_back({ w.a, w.b, std::string(ua == nullptr ? "The challenger" : "The other player") + " left the game: the wager is cancelled." });
				continue;
			}
			int32_t pa[2] {}, pb[2] {};
			g_getCoords(ua, pa);
			g_getCoords(ub, pb);
			const int64_t dx = pa[0] - pb[0], dy = pa[1] - pb[1];
			if (LevelOf(ua) != LevelOf(ub) || dx * dx + dy * dy >= static_cast<int64_t>(kWagerMaxDistance) * kWagerMaxDistance) {
				off.push_back({ w.a, w.b, "You walked away from each other: the wager is cancelled." });
				continue;
			}
			// 2.4.10 (IV14 log 2026-11-09: the server trade state at client +0x6B8 stayed 0/0/-1 for both even with the trade
			// window open, so a close was never seen and every cancel waited for the timer): the clients report their own
			// trade popup / window closing (kStakeTradeClosed); a player a few steps from where they stood at the mark has
			// walked away; the timer runs on the wall clock.
			const auto moved = [](const int32_t* now, const int32_t* then) {
				const int64_t mx = now[0] - then[0], my = now[1] - then[1];
				return mx * mx + my * my > static_cast<int64_t>(kStakeTradeWalkAway) * kStakeTradeWalkAway;
			};
			if (const uint64_t age = (NowMs() - w.startedMs) / 1000; g_pvp.diagnostics && age > 0 && age % 30 == 0 && w.lastAgeLogged != age) {
				w.lastAgeLogged = age; // 2.4.11 diagnostic
				Log("arena: stake trade %u/%u still pending after %llu s.", w.a, w.b, static_cast<unsigned long long>(age));
			}
			// 2.4.15 (user 2026-11-14: the client walked away, the chat blamed the host): walking away closes both popups, so
			// both report a close and the host's local report comes first; the walk-away check now runs before the close report.
			if (const int64_t sx = w.posA[0] - w.posB[0], sy = w.posA[1] - w.posB[1];
			    std::sqrt(static_cast<double>(dx * dx + dy * dy)) > std::sqrt(static_cast<double>(sx * sx + sy * sy)) + kStakeTradeWalkAway
			    && (moved(pa, w.posA) || moved(pb, w.posB))) { // apart by a few more steps than at the mark
				off.push_back({ w.a, w.b, NameOf(moved(pa, w.posA) ? ua : ub) + " walked away from the stake trade: the wager is cancelled." });
			} else if (w.closedBy != 0) {
				// 2.4.16 (user 2026-11-14): neutral - both games report a close and the first report is not the one who cancelled
				off.push_back({ w.a, w.b, "The stake trade was cancelled: the wager is cancelled." });
			} else if (NowMs() - w.startedMs > static_cast<uint64_t>(kStakeTradeOpenSeconds) * 1000) {
				// 2.4.11 (user: after 2:30 the window turned into a normal trade): closed after 2 minutes, trade window too
				off.push_back({ w.a, w.b, "The stake trade wasn't finished in 2 minutes: the wager is cancelled." });
			} else if (InLobbyLocked(w.a) || InLobbyLocked(w.b) || (WagerOfLocked(w.a) != nullptr) || (WagerOfLocked(w.b) != nullptr)) {
				const uint32_t who = InLobbyLocked(w.a) || WagerOfLocked(w.a) != nullptr ? w.a : w.b; // 2.4.4: moved on
				off.push_back({ w.a, w.b, NameOf(who == w.a ? ua : ub) + " left the stake trade: the wager is cancelled." });
			}
		}
		g_tradeWagers.erase(std::remove_if(g_tradeWagers.begin(), g_tradeWagers.end(), [&](const TradeWager& w) {
			                    return std::any_of(off.begin(), off.end(), [&](const Off& o) { return o.a == w.a && o.b == w.b; });
		                    }),
		                    g_tradeWagers.end());
	}
	for (const Off& o : off) {
		const uint8_t zero = 0;
		for (uint32_t g : { o.a, o.b }) {
			SendToPlayer(g, kMsgWagerFlag, &zero, 1);
			SendToPlayer(g, kMsgCloseTrade, &zero, 1); // 2.4.11: a stock popup / window left open must not become a normal trade
			DuelTell(game, g, o.why, 5000, false);
		}
		Log("arena: wager trade of %u and %u cancelled: %s", o.a, o.b, o.why.c_str());
	}
	for (const TradeStart& ts : starts) {
		void* ua = g_serverUnit(game, kUnitPlayer, ts.a);
		void* ub = g_serverUnit(game, kUnitPlayer, ts.b);
		std::string bad;
		if (ua == nullptr || ub == nullptr) {
			bad = "The other player left the game.";
		} else if (std::string why = BusyReason(game, ua); !why.empty()) {
			bad = NameOf(ua) + " is " + why + ".";
		} else if (why = BusyReason(game, ub); !why.empty()) {
			bad = NameOf(ub) + " is " + why + ".";
		}
		const int32_t level = bad.empty() ? FreeArena(game) : 0;
		if (bad.empty() && level == 0) {
			bad = "All arenas are in use. Try again later.";
		}
		if (!bad.empty()) {
			for (uint32_t g : { ts.a, ts.b }) {
				DuelTell(game, g, bad + " The wager duel is off.", 6000, false);
			}
			continue;
		}
		{
			std::lock_guard lock(g_pvpLock);
			g_stakedItems[ts.a] = ts.itemsA;
			g_stakedItems[ts.b] = ts.itemsB;
		}
		StartDuel(game, ua, ub, level, frame, false, ts.goldA, ts.goldB, !ts.itemsA.empty() || !ts.itemsB.empty(), &ts.rules);
		if (!ts.itemsA.empty() || !ts.itemsB.empty()) {
			char line[160] {};
			std::snprintf(line, sizeof(line), "Staked items: %s %zu, %s %zu. The winner takes them.", NameOf(ua).c_str(), ts.itemsA.size(), NameOf(ub).c_str(),
			              ts.itemsB.size());
			for (uint32_t g : { ts.a, ts.b }) {
				if (void* u = g_serverUnit(game, kUnitPlayer, g); u != nullptr) {
					if (void* c = g_client(u); c != nullptr) {
						SendLine(c, g, line);
					}
				}
			}
		}
	}
}

void ProcessDuelCmds(void* game, int32_t frame) {
	std::vector<DuelCmd> cmds;
	{
		std::lock_guard lock(g_duelCmdLock);
		cmds.swap(g_duelCmds);
	}
	const uint32_t inviteMs = static_cast<uint32_t>(g_pvp.duelInviteSeconds * 1000.0 + 0.5);
	for (const DuelCmd& cmd : cmds) {
		void* me = g_serverUnit(game, kUnitPlayer, cmd.guid);
		if (me == nullptr) {
			continue;
		}
		const std::string myName = NameOf(me);
		if (!g_pvp.duelEnabled) {
			DuelTell(game, cmd.guid, "Duels are turned off.", 5000, false);
			continue;
		}
		if (cmd.kind == kDuelPlainTrade) { // 1.4.0: menu Trade: a normal trade, never a leftover wager mark
			std::vector<uint32_t> off;
			{
				std::lock_guard lock(g_pvpLock);
				for (const TradeWager& w : g_tradeWagers) {
					if (w.a == cmd.guid || w.b == cmd.guid) {
						off.push_back(w.a), off.push_back(w.b);
					}
				}
				g_tradeWagers.erase(std::remove_if(g_tradeWagers.begin(), g_tradeWagers.end(), [&](const TradeWager& w) { return w.a == cmd.guid || w.b == cmd.guid; }),
				                    g_tradeWagers.end());
			}
			const uint8_t zero = 0;
			for (uint32_t g : off) {
				SendToPlayer(g, kMsgWagerFlag, &zero, 1);
			}
			continue;
		}
		if (cmd.kind == kDuelTrade) { // 1.4.0: player menu Duel -> a wager through the stock trade window
			void* other = FindPlayerByName(game, cmd.name);
			if (other == nullptr || other == me) {
				continue;
			}
			const std::string oName = NameOf(other);
			std::string       why   = BusyReason(game, me);
			if (!why.empty()) {
				DuelTell(game, cmd.guid, "You can't challenge now: you are " + why + ".", 5000, false);
				continue;
			}
			if (why = BusyReason(game, other); !why.empty()) {
				DuelTell(game, cmd.guid, oName + " is " + why + ".", 5000, false);
				continue;
			}
			const uint32_t pa = static_cast<uint32_t>(g_partyId(me) & 0xFFFF), pb = static_cast<uint32_t>(g_partyId(other) & 0xFFFF);
			if (pa != 0xFFFF && pa == pb) {
				DuelTell(game, cmd.guid, "Leave the party with " + oName + " first.", 5000, false);
				continue;
			}
			if (FreeArena(game) == 0) {
				DuelTell(game, cmd.guid, "All arenas are in use. Try again later.", 5000, false);
				continue;
			}
			BeginTradeWager(game, me, other, DefaultRules(), frame);
			continue;
		}
		if (cmd.kind == kDuelChallenge || cmd.kind == kDuelWager) {
			const bool     panel = cmd.kind == kDuelWager; // 1.1.0: the stakes are set in the wager window after the accept
			std::string    who   = Trim(cmd.name);
			const uint32_t stake = panel ? 0 : SplitStake(cmd.name, who);
			void*          other = FindPlayerByName(game, who);
			if (other == nullptr || other == me) {
				DuelTell(game, cmd.guid, other == me ? "You can't challenge yourself." : "No player called \"" + who + "\" in this game.", 5000, false);
				continue;
			}
			const uint32_t    og    = Field<uint32_t>(other, 0x08);
			const std::string oName = NameOf(other);
			std::string       why   = BusyReason(game, me);
			if (!why.empty()) {
				DuelTell(game, cmd.guid, "You can't challenge now: you are " + why + ".", 5000, false);
				continue;
			}
			if (why = BusyReason(game, other); !why.empty()) {
				DuelTell(game, cmd.guid, oName + " is " + why + ".", 5000, false);
				continue;
			}
			const uint32_t pa = static_cast<uint32_t>(g_partyId(me) & 0xFFFF), pb = static_cast<uint32_t>(g_partyId(other) & 0xFFFF);
			if (pa != 0xFFFF && pa == pb) {
				DuelTell(game, cmd.guid, "Leave the party with " + oName + " first.", 5000, false);
				continue;
			}
			if (panel && !g_pvp.wagerEnabled) {
				DuelTell(game, cmd.guid, "Gold wagers are turned off.", 5000, false);
				continue;
			}
			if (stake > 0) {
				if (!g_pvp.wagerEnabled) {
					DuelTell(game, cmd.guid, "Gold wagers are turned off.", 5000, false);
					continue;
				}
				if (g_pvp.wagerMax > 0 && stake > g_pvp.wagerMax) {
					DuelTell(game, cmd.guid, "The highest wager is " + std::to_string(g_pvp.wagerMax) + " gold.", 5000, false);
					continue;
				}
				if (GoldOf(me) < stake) {
					DuelTell(game, cmd.guid, "You don't carry " + std::to_string(stake) + " gold.", 5000, false);
					continue;
				}
				if (GoldOf(other) < stake) {
					DuelTell(game, cmd.guid, oName + " doesn't carry " + std::to_string(stake) + " gold.", 5000, false);
					continue;
				}
			}
			{
				std::lock_guard lock(g_pvpLock);
				g_invites.erase(std::remove_if(g_invites.begin(), g_invites.end(), [&](const Invite& i) { return i.from == cmd.guid; }), g_invites.end());
				g_invites.push_back({ cmd.guid, og, frame + static_cast<int32_t>(g_pvp.duelInviteSeconds * kFramesPerSecond + 0.5), stake, panel });
				g_duelBusy = true;
			}
			// 2.4.6 (user: console-style instruction texts don't fit the menus): the invite window (kMsgDuelInvite) is the
			// only prompt - no "arena accept / arena decline" notice or chat line; the challenger gets a short notice only.
			SendDuelInvite(og, inviteMs, panel, stake, myName);
			DuelTell(game, cmd.guid, "", 0, false, false); // 2.4.7 (user): no "Challenge sent" notice either
			Log("arena: %u challenges %u to a duel.", cmd.guid, og);
		} else if (cmd.kind == kDuelAccept || cmd.kind == kDuelDecline) {
			uint32_t from = 0, stake = 0;
			bool     panel = false;
			{
				std::lock_guard lock(g_pvpLock);
				for (auto it = g_invites.rbegin(); it != g_invites.rend(); ++it) {
					if (it->to == cmd.guid) {
						from  = it->from;
						stake = it->stake;
						panel = it->panel;
						break;
					}
				}
				if (from != 0 && cmd.kind == kDuelDecline) {
					g_invites.erase(std::remove_if(g_invites.begin(), g_invites.end(), [&](const Invite& i) { return i.from == from && i.to == cmd.guid; }),
					                g_invites.end());
				}
			}
			void* challenger = from != 0 ? g_serverUnit(game, kUnitPlayer, from) : nullptr;
			if (challenger == nullptr) {
				DuelTell(game, cmd.guid, "No open duel challenge.", 5000, false);
				continue;
			}
			const std::string cName = NameOf(challenger);
			if (cmd.kind == kDuelDecline) {
				DuelTell(game, cmd.guid, "You declined the duel.", 4000, false, false);
				DuelTell(game, from, myName + " declined the duel.", 5000, false);
				continue;
			}
			std::string why = BusyReason(game, me);
			if (!why.empty()) {
				DuelTell(game, cmd.guid, "You can't start the duel now: you are " + why + ".", 5000, false);
				continue;
			}
			if (why = BusyReason(game, challenger); !why.empty()) {
				DuelTell(game, cmd.guid, cName + " is " + why + ". Try again in a moment.", 5000, false);
				continue;
			}
			if (panel) {
				{
					std::lock_guard lock(g_pvpLock); // the accepted invite is used up
					g_invites.erase(std::remove_if(g_invites.begin(), g_invites.end(), [&](const Invite& i) { return i.from == from && i.to == cmd.guid; }),
					                g_invites.end());
				}
				OpenWager(game, challenger, me);
				continue;
			}
			if (stake > 0 && (!g_pvp.wagerEnabled || GoldOf(me) < stake || GoldOf(challenger) < stake)) {
				const std::string bad = !g_pvp.wagerEnabled ? "Gold wagers are turned off." : "Both players must carry the " + std::to_string(stake) + " gold.";
				DuelTell(game, cmd.guid, bad, 5000, false);
				DuelTell(game, from, bad, 5000, false);
				continue;
			}
			const int32_t level = FreeArena(game);
			if (level == 0) {
				DuelTell(game, cmd.guid, "All arenas are in use. Try again later.", 5000, false);
				DuelTell(game, from, "All arenas are in use. Try again later.", 5000, false);
				continue;
			}
			{
				std::lock_guard lock(g_pvpLock); // the accepted invite is used up
				g_invites.erase(std::remove_if(g_invites.begin(), g_invites.end(), [&](const Invite& i) { return i.from == from && i.to == cmd.guid; }),
				                g_invites.end());
			}
			StartDuel(game, challenger, me, level, frame, false, stake, stake);
		} else if (cmd.kind == kDuelCancel) {
			uint32_t to = 0;
			{
				std::lock_guard lock(g_pvpLock);
				for (const Invite& i : g_invites) {
					if (i.from == cmd.guid) {
						to = i.to;
					}
				}
				g_invites.erase(std::remove_if(g_invites.begin(), g_invites.end(), [&](const Invite& i) { return i.from == cmd.guid; }), g_invites.end());
			}
			if (to == 0) {
				bool        inWager = false;
				std::string winName = "Duel Rules window";
				{
					std::lock_guard lock(g_pvpLock);
					if (const WagerSession* ws = WagerOfLocked(cmd.guid); ws != nullptr) {
						inWager = true;
						winName = WagerWindowName(*ws); // 2.4.2
					}
				}
				if (inWager) { // 1.2.2: the way out of a wager window that didn't open on screen
					{
						std::lock_guard lock(g_wagerCmdLock);
						g_wagerCmds.push_back({ cmd.guid, kWagerClose, 0 });
					}
					DuelTell(game, cmd.guid, std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(winName[0])))) + winName.substr(1) + " closed.", 4000, false, false);
					continue;
				}
				DuelTell(game, cmd.guid, "No challenge to cancel.", 4000, false);
				continue;
			}
			DuelTell(game, cmd.guid, "Challenge cancelled.", 4000, false, false);
			DuelTell(game, to, myName + " cancelled the duel challenge.", 5000, false);
			SendDuelInvite(to, 0, false, 0, "");
		}
	}
}

// Walk-in duels (0.7.6, user 2026-11-07): a second player coming into an arena turns the stay into a normal duel at the
// configured points. Only after both clients have loaded (ProcessDuels) are they moved: whoever already stands on a
// spawn spot stays, nobody shares a spot.
auto SpawnSpotOf(void* unit, const std::vector<Spot>& spots, int32_t ox, int32_t oy) -> int {
	int32_t xy[2] {};
	g_getCoords(unit, xy);
	for (size_t i = 0; i < spots.size(); ++i) {
		if (std::abs(ox + spots[i].x - xy[0]) <= 3 && std::abs(oy + spots[i].y - xy[1]) <= 3) {
			return static_cast<int>(i);
		}
	}
	return -1;
}
void PlaceWalkIn(void* game, void* ua, void* ub, int32_t level) {
	int32_t    lv = 0, ox = 0, oy = 0;
	const auto it = g_pvp.spawns.find(level);
	if (ArenaOf(ua, &lv, &ox, &oy) == nullptr || lv != level || it == g_pvp.spawns.end() || it->second.size() < 2) {
		return;
	}
	const std::vector<Spot>& spots = it->second;
	const auto other = [&](int taken) {
		std::uniform_int_distribution<int> pick(0, static_cast<int>(spots.size()) - 1);
		int i = pick(Rng());
		while (i == taken) {
			i = pick(Rng());
		}
		return i;
	};
	const int onA = SpawnSpotOf(ua, spots, ox, oy);
	const int onB = SpawnSpotOf(ub, spots, ox, oy);
	int       sa  = onA;
	int       sb  = onB == onA ? -1 : onB; // same spot: the second one moves
	if (sa < 0) {
		sa = other(sb);
	}
	if (sb < 0) {
		sb = other(sa);
	}
	if (sa != onA) {
		(void)Teleport(game, ua, level, ox + spots[sa].x, oy + spots[sa].y);
		g_changeMode2(game, ua, nullptr, kPlrModeNeutral, 0, 0, 1);
	}
	if (sb != onB) {
		(void)Teleport(game, ub, level, ox + spots[sb].x, oy + spots[sb].y);
		g_changeMode2(game, ub, nullptr, kPlrModeNeutral, 0, 0, 1);
	}
	Log("arena: walk-in duel in level %d: spawn spots %d (was %d) and %d (was %d).", level, sa, onA, sb, onB);
}
// Entrance trapdoor: a walk-in would make a duel, so refuse what can't be one. True = refused.
auto RefuseWalkIn(void* game, void* player, int32_t level) -> bool {
	if (!g_pvp.duelEnabled || !g_pvp.duelWalkIn) {
		return false;
	}
	const uint32_t party = static_cast<uint32_t>(g_partyId(player) & 0xFFFF);
	int            count = 0, n = 0;
	for (void* c = Field<void*>(game, kGameClients); c != nullptr && n < 64; c = Field<void*>(c, kClientNext), ++n) {
		if (Field<uint32_t>(c, kClientUnitType) != kUnitPlayer) {
			continue;
		}
		void* u = g_serverUnit(game, kUnitPlayer, Field<uint32_t>(c, kClientUnitGuid));
		if (u == nullptr || u == player || LevelOf(u) != level) {
			continue;
		}
		++count;
		if (party != 0xFFFF && party == static_cast<uint32_t>(g_partyId(u) & 0xFFFF)) {
			Refuse(game, player, "Leave the party with " + NameOf(u) + " first.");
			return true;
		}
	}
	if (DuelJoinable(level)) { // 1.0.0: join the free for all while there is room
		if (count + 1 > g_pvp.duelMaxPlayers) {
			Refuse(game, player, "This duel is full (" + std::to_string(g_pvp.duelMaxPlayers) + " players).");
			return true;
		}
		return false;
	}
	if (count >= 2) {
		Refuse(game, player, g_pvp.duelBusyText);
		return true;
	}
	return false;
}
// 1.0.0: everyone in the level who isn't in its joinable duel yet joins it (came in by the trapdoor; a debug teleport
// beyond walk_in_extras stays outside the duel). Frozen until placed and protected (ProcessJoins).
void JoinWalkIns(void* game, int32_t level, const std::vector<void*>& units, int32_t frame) {
	for (void* u : units) {
		const uint32_t    guid = Field<uint32_t>(u, 0x08);
		const std::string name = NameOf(u);
		int32_t           score = 0;
		std::vector<uint32_t> others;
		{
			std::lock_guard lock(g_pvpLock);
			const auto it = std::find_if(g_duels.begin(), g_duels.end(), [&](const Duel& d) { return d.level == level; });
			if (it == g_duels.end() || IsMember(*it, guid) || static_cast<int32_t>(Members(*it).size()) >= g_pvp.duelMaxPlayers
			    || std::any_of(g_returns.begin(), g_returns.end(), [&](const PendingReturn& r) { return r.duelEnd && r.guid == guid; })) {
				continue;
			}
			others = Members(*it);
			score  = it->state == kDuelFighting ? g_pvp.duelJoinScore : 0; // user: joining during the countdown starts at 0
			it->joined.push_back({ guid, name });
			g_joinWait.push_back({ guid, level, frame + static_cast<int32_t>(10.0 * kFramesPerSecond) });
			g_joinWait.back().score = score;
			PlayerScore& js         = g_scores[guid];
			js                      = PlayerScore {};
			js.name                 = name;
			js.score                = score;
			g_duelLoaded.erase(guid);
		}
		SetFrozen(u, guid, true);
		const int32_t check[2] { level, 0 };
		SendToPlayer(guid, kMsgDuelLoadCheck, check, sizeof(check));
		if (void* c = g_client(u); c != nullptr) {
			SendLine(c, guid, "You join the duel with " + std::to_string(score) + " points. First to " + std::to_string(g_pvp.duelPoints) + " wins.");
		}
		for (uint32_t g : others) {
			if (void* o = g_serverUnit(game, kUnitPlayer, g); o != nullptr) {
				if (void* c = g_client(o); c != nullptr) {
					SendLine(c, g, name + " joins the duel (" + std::to_string(score) + " points).");
				}
			}
		}
		Log("arena: player %u joins the walk-in duel in level %d with %d points.", guid, level, score);
	}
}
// Spawn spot for a joined player: the free one farthest from the nearest other duelist.
void PlaceJoiner(void* game, void* unit, int32_t level, const std::vector<uint32_t>& members) {
	int32_t    lv = 0, ox = 0, oy = 0;
	const auto it = g_pvp.spawns.find(level);
	if (ArenaOf(unit, &lv, &ox, &oy) == nullptr || lv != level || it == g_pvp.spawns.end() || it->second.empty()) {
		return;
	}
	std::vector<std::array<int32_t, 2>> others;
	for (uint32_t g : members) {
		if (void* o = g_serverUnit(game, kUnitPlayer, g); o != nullptr && o != unit && LevelOf(o) == level) {
			int32_t xy[2] {};
			g_getCoords(o, xy);
			others.push_back({ xy[0], xy[1] });
		}
	}
	int     best = -1;
	int64_t bestD = -1;
	for (size_t i = 0; i < it->second.size(); ++i) {
		const int32_t x = ox + it->second[i].x, y = oy + it->second[i].y;
		int64_t       nearest = INT64_MAX;
		for (const auto& o : others) {
			nearest = std::min<int64_t>(nearest, static_cast<int64_t>(x - o[0]) * (x - o[0]) + static_cast<int64_t>(y - o[1]) * (y - o[1]));
		}
		if (nearest > bestD) {
			bestD = nearest;
			best  = static_cast<int>(i);
		}
	}
	if (best >= 0) {
		(void)Teleport(game, unit, level, ox + it->second[best].x, oy + it->second[best].y);
		g_changeMode2(game, unit, nullptr, kPlrModeNeutral, 0, 0, 1);
		Log("arena: joined player %u placed on spawn spot %d in level %d.", Field<uint32_t>(unit, 0x08), best, level);
	}
}
void ProcessWalkIns(void* game, int32_t frame) {
	if (!g_pvp.duelEnabled || !g_pvp.duelWalkIn) {
		return;
	}
	std::map<int32_t, std::vector<void*>> inLevel;
	int                                   n = 0;
	for (void* c = Field<void*>(game, kGameClients); c != nullptr && n < 64; c = Field<void*>(c, kClientNext), ++n) {
		if (Field<uint32_t>(c, kClientUnitType) == kUnitPlayer) {
			if (void* u = g_serverUnit(game, kUnitPlayer, Field<uint32_t>(c, kClientUnitGuid)); u != nullptr && IsPvpLevel(LevelOf(u))) {
				inLevel[LevelOf(u)].push_back(u);
			}
		}
	}
	for (const auto& [level, units] : inLevel) {
		if (DuelJoinable(level)) {
			JoinWalkIns(game, level, units, frame);
			continue;
		}
		if (units.size() != 2 || ArenaHasDuel(level) || IsDownPlayer(units[0]) || IsDownPlayer(units[1])) {
			continue;
		}
		{
			std::lock_guard lock(g_pvpLock); // on their way out after a duel: not a new one
			const uint32_t g0 = Field<uint32_t>(units[0], 0x08), g1 = Field<uint32_t>(units[1], 0x08);
			if (std::any_of(g_returns.begin(), g_returns.end(), [&](const PendingReturn& r) { return r.duelEnd && (r.guid == g0 || r.guid == g1); })) {
				continue;
			}
		}
		const uint32_t pa = static_cast<uint32_t>(g_partyId(units[0]) & 0xFFFF), pb = static_cast<uint32_t>(g_partyId(units[1]) & 0xFFFF);
		if (pa != 0xFFFF && pa == pb) {
			continue; // same party: not a fight (the trapdoor refuses this; they can join a party only after arriving)
		}
		StartDuel(game, units[0], units[1], level, frame, true);
	}
}

// 1.0.0 helpers for joined players (game thread).
auto DuelClientLoaded(uint32_t guid) -> bool; // below
auto JoinWaiting(uint32_t guid) -> bool {
	std::lock_guard lock(g_pvpLock);
	return std::any_of(g_joinWait.begin(), g_joinWait.end(), [&](const JoinWait& w) { return w.guid == guid; });
}
// A duelist who left a free for all that goes on: unfrozen, told, the others told.
void LeaveDuel(void* game, const Duel& d, uint32_t guid, const std::string& name, void* unit) {
	{
		std::lock_guard lock(g_pvpLock);
		g_joinWait.erase(std::remove_if(g_joinWait.begin(), g_joinWait.end(), [&](const JoinWait& w) { return w.guid == guid; }), g_joinWait.end());
	}
	if (unit != nullptr) {
		SetFrozen(unit, guid, false);
		SendDuelCountdown(guid, 0);
	}
	for (uint32_t g : Members(d)) {
		if (g == guid) {
			continue;
		}
		if (void* o = g_serverUnit(game, kUnitPlayer, g); o != nullptr) {
			if (void* c = g_client(o); c != nullptr) {
				SendLine(c, g, name + " left the duel.");
			}
		}
	}
	Log("arena: player %u left the free for all in level %d.", guid, d.level);
}
// Joined players: placed once their client loaded (or after 10 s), then frozen for join_protect_seconds and at least
// until the duel's countdown is over.
void ProcessJoins(void* game, const Duel& d, int32_t frame) {
	std::vector<JoinWait> waits;
	{
		std::lock_guard lock(g_pvpLock);
		for (const JoinWait& w : g_joinWait) {
			if (w.level == d.level && IsMember(d, w.guid)) {
				waits.push_back(w);
			}
		}
	}
	const std::vector<uint32_t> members = Members(d);
	for (JoinWait& w : waits) {
		void* u = g_serverUnit(game, kUnitPlayer, w.guid);
		if (u == nullptr || LevelOf(u) != d.level) {
			continue; // the leave step above handles it next tick
		}
		bool done = false;
		if (!w.placed && (DuelClientLoaded(w.guid) || frame - w.loadDue >= 0)) {
			PlaceJoiner(game, u, d.level, members);
			w.placed     = true;
			w.protectDue = frame + static_cast<int32_t>(g_pvp.duelJoinProtect * kFramesPerSecond + 0.5);
			if (d.state == kDuelCountdown && d.due - w.protectDue > 0) {
				w.protectDue = d.due;
			}
			SendDuelCountdown(w.guid, static_cast<uint32_t>(std::max(0, w.protectDue - frame) * 1000 / kFramesPerSecond));
		} else if (w.placed && frame - w.protectDue >= 0 && d.state == kDuelFighting) {
			SetFrozen(u, w.guid, false);
			done = true;
			Log("arena: joined player %u fights.", w.guid);
		}
		std::lock_guard lock(g_pvpLock);
		const auto it = std::find_if(g_joinWait.begin(), g_joinWait.end(), [&](const JoinWait& x) { return x.guid == w.guid; });
		if (it != g_joinWait.end()) {
			if (done) {
				g_joinWait.erase(it);
			} else {
				*it = w;
			}
		}
	}
}

constexpr int32_t kDuelLoserWarpFrames = 3;
// Ends a duel: winner 0 = cancelled. Respawns a downed player, unfreezes, sends both (if still here) to town.
void FinishDuel(void* game, const Duel& d) {
	if (d.stake > 0 && !d.paid) { // cancelled (didn't arrive, both left): stakes back to whoever is still here
		RefundStake(game, d.a, "duel cancelled");
		RefundStake(game, d.b, "duel cancelled");
		SaveNow(game);
	}
	for (uint32_t g : Members(d)) {
		void* u = g_serverUnit(game, kUnitPlayer, g);
		bool  down = false;
		{
			std::lock_guard lock(g_pvpLock);
			g_stakedItems.erase(g); // 1.4.0: paid already, or a cancelled duel (items never moved)
			g_lockTold.erase(g);    // 1.5.0 (game thread, same as the gate)
			g_joinWait.erase(std::remove_if(g_joinWait.begin(), g_joinWait.end(), [&](const JoinWait& w) { return w.guid == g; }), g_joinWait.end());
			const auto it = std::find_if(g_down.begin(), g_down.end(), [&](const Down& x) { return x.guid == g; });
			if (it != g_down.end()) {
				g_down.erase(it);
				g_downCount = static_cast<int32_t>(g_down.size());
				down        = true;
			}
		}
		if (u == nullptr) {
			continue;
		}
		if (down) {
			Respawn(game, g, false); // no spawn spot: they are leaving
		}
		SetFrozen(u, g, false);
		if (!down) { // 2.3.0 (user): everyone healed at the end, as at the start (the downed get it from Respawn above)
			if (g_pvp.cleanStart) {
				CleanStart(game, u);
			} else {
				g_removeStates(u);
				RestoreFull(u);
				for (const int32_t stat : { 6, 8, 10 }) {
					g_updateAttr(u, stat, static_cast<uint32_t>(g_readStat(u, stat, 0)), u, 0);
				}
			}
		}
		SendDuelCountdown(g, 0);
		if (d.suddenDeath) {
			SendSudden(g, kSuddenOff);
		}
		if (d.endFrame != 0) {
			AloneNotice(g, "", 0); // 1.6.0: the "Time left" notice
		}
		if (d.customRules && d.rules.mapShow != 1) {
			const uint8_t off = 0; // 1.8.0: the automap shows everyone again
			SendToPlayer(g, kMsgMapHide, &off, 1);
		}
		if (!d.team.empty()) {
			const uint8_t off = 0; // 1.9.3: teammate party look off
			SendToPlayer(g, kMsgTeamMates, &off, 1);
		}
		if (LevelOf(u) == d.level) {
			if (down) {
				// A respawn and a warp to another act in one tick crashed the client (2026-11-07, loser going back to act 4:
				// CUnit.cpp:380/427 + SCmd.cpp:1254 "Could not get act!" in the 0x15 handler). Leave a few frames between them.
				std::lock_guard lock(g_pvpLock);
				g_returns.push_back({ g, d.level, Field<int32_t>(game, kGameFrame) + kDuelLoserWarpFrames, true, true });
			} else {
				WarpHome(game, u);
			}
		}
	}
	Log("arena: duel %u vs %u (+%zu joined) in level %d finished (winner %u).", d.a, d.b, d.joined.size(), d.level, d.winner);
}

// A client that can't answer (no hello: no plugin there) counts as loaded: the server side is all that can be seen.
auto DuelClientLoaded(uint32_t guid) -> bool {
	std::lock_guard lock(g_pvpLock);
	return g_duelLoaded.count(guid) != 0 || (guid != g_localGuid.load() && g_peers.count(guid) == 0);
}
// True when this player's client saw its loading screen close (ack 2).
auto DuelScreenClosed(uint32_t guid) -> bool {
	std::lock_guard lock(g_pvpLock);
	const auto it = g_duelLoaded.find(guid);
	return it != g_duelLoaded.end() && it->second == 2;
}

// 1.6.0: the time is up. Highest score wins; a tie -> sudden death (the next kill wins, user 2026-11-08).
void PayItems(void* game, uint32_t winner, uint32_t loser, bool save);
void EndDuelOnTime(void* game, Duel& d, int32_t frame) {
	const std::vector<uint32_t> members = Members(d);
	if (!d.team.empty()) { // 1.9.0: the higher team total wins, a tie goes to sudden death
		for (uint32_t g : members) {
			AloneNotice(g, "", 0);
		}
		if (d.teamScore[0] == d.teamScore[1]) {
			d.suddenDeath = true;
			Broadcast(game, "Time! The teams are tied: sudden death - no more respawns, the last team standing wins.");
			for (uint32_t g : members) {
				SendSudden(g, kSuddenLastTeam); // 1.9.7: a short banner (kSuddenBannerMs), then off
			}
			Log("arena: team duel in level %d: time up, tied at %d: sudden death.", d.level, d.teamScore[0]);
			return;
		}
		const uint8_t wt = d.teamScore[0] > d.teamScore[1] ? 0 : 1;
		d.winner         = wt == 0 ? d.a : d.b;
		d.state          = kDuelEnding;
		d.due            = frame + static_cast<int32_t>(g_pvp.duelEndSeconds * kFramesPerSecond + 0.5);
		Broadcast(game, d.teamName[wt] + " wins the team duel on time (" + std::to_string(d.teamScore[wt]) + " : " + std::to_string(d.teamScore[wt ^ 1]) + ").");
		for (const auto& [g, side] : d.team) {
			void* u = g_serverUnit(game, kUnitPlayer, g);
			if (side != wt && u != nullptr && !IsDownPlayer(u)) {
				SetFrozen(u, g, true);
			}
			SendDuelResult(g, side == wt ? 1 : 2);
		}
		Log("arena: team duel in level %d: time up, team %d wins %d : %d.", d.level, wt + 1, d.teamScore[wt], d.teamScore[wt ^ 1]);
		return;
	}
	uint32_t                    best    = 0;
	int32_t                     top     = INT32_MIN;
	int                         tied    = 0;
	{
		std::lock_guard lock(g_pvpLock);
		for (uint32_t g : members) {
			const auto    it = g_scores.find(g);
			const int32_t sc = it == g_scores.end() ? 0 : d.rules.type == kTypeKills ? it->second.kills : it->second.score; // 2.3.0
			if (sc > top) {
				top = sc, best = g, tied = 1;
			} else if (sc == top) {
				++tied;
			}
		}
	}
	for (uint32_t g : members) {
		AloneNotice(g, "", 0);
	}
	if (tied > 1) {
		d.suddenDeath = true;
		Broadcast(game, "Time! The scores are tied: sudden death - the next kill wins.");
		for (uint32_t g : members) {
			SendSudden(g, kSuddenNextKill); // 1.9.7: a short banner, then off
		}
		Log("arena: duel %u vs %u: time up, tied at %d: sudden death.", d.a, d.b, top);
		return;
	}
	d.winner = best;
	d.state  = kDuelEnding;
	d.due    = frame + static_cast<int32_t>(g_pvp.duelEndSeconds * kFramesPerSecond + 0.5);
	const std::string wName = MemberName(d, best);
	Broadcast(game, wName + " wins the duel on time (" + std::to_string(top) + " points).");
	for (uint32_t g : members) {
		void* u = g_serverUnit(game, kUnitPlayer, g);
		if (g != best && u != nullptr && !IsDownPlayer(u)) {
			SetFrozen(u, g, true); // nobody fights on through the end pause
		}
		SendDuelResult(g, g == best ? 1 : 2);
	}
	if (d.wager && !d.paid) {
		d.paid = true;
		for (uint32_t g : members) {
			if (g != best) {
				PayPot(game, best, g, false);
				PayItems(game, best, g, true);
			}
		}
	}
	Log("arena: duel %u vs %u: time up, %u wins with %d.", d.a, d.b, best, top);
}

// 1.9.0: a team duel's step (game thread). False = finished (drop it).
auto ProcessTeamDuel(void* game, Duel& d, int32_t frame) -> bool {
	const auto unitOf = [&](uint32_t g) { return g_serverUnit(game, kUnitPlayer, g); };
	if (d.state == kDuelMoving) {
		bool allIn = true, gone = false;
		for (const auto& [g, side] : d.team) {
			void* u = unitOf(g);
			gone |= u == nullptr;
			allIn &= u != nullptr && LevelOf(u) == d.level;
		}
		if (gone || (!allIn && frame - d.due >= 0)) {
			for (const auto& [g, side] : d.team) {
				DuelTell(game, g, "The team duel was cancelled (a player didn't arrive).", 5000, false);
			}
			d.winner = 0;
			FinishDuel(game, d);
			return false;
		}
		if (!allIn) {
			return true;
		}
		if (!d.arrived) { // frozen on arrival; the countdown waits for every client (as a 1 v 1)
			d.arrived = true;
			for (const auto& [g, side] : d.team) {
				SetFrozen(unitOf(g), g, true);
			}
		}
		bool loaded = std::all_of(d.team.begin(), d.team.end(), [](const auto& m) { return DuelClientLoaded(m.first); });
		if (loaded && !d.cross.empty()) {
			if (!d.extraStarted) {
				const bool   screens = g_pvp.duelScreenCheck && std::all_of(d.cross.begin(), d.cross.end(), [](uint32_t g) { return DuelScreenClosed(g); });
				const double extra   = screens ? g_pvp.duelScreenExtra : g_pvp.duelCrossActExtra;
				d.extraStarted       = true;
				d.extraDue           = frame + static_cast<int32_t>(extra * kFramesPerSecond + 0.5);
				Log("arena: team duel in level %d: %s: %.1f s before the countdown.", d.level, screens ? "after the loading screens" : "fixed wait", extra);
			}
			loaded = frame - d.extraDue >= 0;
		}
		if (!loaded && frame - d.due < 0) {
			if (!d.waitNoticed) {
				d.waitNoticed        = true;
				const uint32_t waitMs = static_cast<uint32_t>(g_pvp.duelMoveTimeout * 1000.0 + 0.5);
				for (const auto& [g, side] : d.team) {
					DuelTell(game, g, "Waiting for all players to finish loading.", waitMs, false, false);
				}
				Log("arena: team duel in level %d: all arrived, waiting for their clients to load.", d.level);
			}
			return true;
		}
		const uint32_t cdMs = static_cast<uint32_t>(g_pvp.duelCountdown * 1000.0 + 0.5);
		for (const auto& [g, side] : d.team) {
			if (d.waitNoticed) {
				DuelTell(game, g, "", 0, false, false);
			}
			SendDuelCountdown(g, cdMs);
		}
		d.state = kDuelCountdown;
		d.due   = frame + static_cast<int32_t>(g_pvp.duelCountdown * kFramesPerSecond + 0.5);
		Log("arena: team duel in level %d: countdown.", d.level);
		return true;
	}
	if (d.state == kDuelCountdown || d.state == kDuelFighting) {
		std::vector<uint32_t> left; // left the arena or the game: out of the duel
		for (const auto& [g, side] : d.team) {
			void* u = unitOf(g);
			if (u == nullptr || LevelOf(u) != d.level) {
				left.push_back(g);
			}
		}
		for (uint32_t g : left) {
			const std::string name = MemberName(d, g);
			d.team.erase(g);
			LeaveDuel(game, d, g, name, unitOf(g));
			if (d.rules.mapShow != 1 && unitOf(g) != nullptr) {
				const uint8_t off = 0; // their automap shows everyone again
				SendToPlayer(g, kMsgMapHide, &off, 1);
			}
			if (unitOf(g) != nullptr) {
				const uint8_t off = 0; // 1.9.3: no teammates any more
				SendToPlayer(g, kMsgTeamMates, &off, 1);
			}
		}
		int count[2] {};
		for (const auto& [g, side] : d.team) {
			++count[side];
		}
		if (count[0] == 0 || count[1] == 0) {
			if (count[0] == 0 && count[1] == 0) {
				d.winner = 0;
				FinishDuel(game, d);
				return false;
			}
			const uint8_t wt = count[0] != 0 ? 0 : 1;
			d.winner         = wt == 0 ? d.a : d.b;
			Broadcast(game, d.teamName[wt] + " wins the team duel (the other team left).");
			for (const auto& [g, side] : d.team) {
				SetFrozen(unitOf(g), g, false);
				SendDuelCountdown(g, 0);
				SendDuelResult(g, 1);
			}
			d.state = kDuelEnding;
			d.due   = frame + static_cast<int32_t>(g_pvp.duelEndSeconds * kFramesPerSecond + 0.5);
			return true;
		}
		if (d.state == kDuelCountdown && frame - d.due >= 0) {
			for (const auto& [g, side] : d.team) {
				SetFrozen(unitOf(g), g, false);
			}
			d.state = kDuelFighting;
			if (d.rules.minutes > 0) {
				d.endFrame = frame + d.rules.minutes * 60 * kFramesPerSecond;
				for (const auto& [g, side] : d.team) {
					AloneNotice(g, "Time left", static_cast<uint32_t>(d.rules.minutes) * 60000u);
				}
			}
			Log("arena: team duel in level %d: fight.", d.level);
		}
		if (d.state == kDuelFighting && d.endFrame != 0 && !d.suddenDeath && frame - d.endFrame >= 0) {
			EndDuelOnTime(game, d, frame);
		}
		return true;
	}
	if (frame - d.due >= 0) { // kDuelEnding
		FinishDuel(game, d);
		return false;
	}
	return true;
}

// Duel state machine + invite expiry, every tick (game thread).
void ProcessDuels(void* game, int32_t frame) {
	std::vector<Duel>   duels;
	std::vector<Invite> expired, vanished;
	{
		std::lock_guard lock(g_pvpLock);
		duels = g_duels;
		for (auto it = g_invites.begin(); it != g_invites.end();) {
			if (g_serverUnit(game, kUnitPlayer, it->from) == nullptr || g_serverUnit(game, kUnitPlayer, it->to) == nullptr) {
				vanished.push_back(*it); // a player left the game: the other one's notice must not keep counting
				it = g_invites.erase(it);
			} else if (frame - it->expires >= 0) {
				expired.push_back(*it);
				it = g_invites.erase(it);
			} else {
				++it;
			}
		}
	}
	for (const Invite& i : expired) {
		DuelTell(game, i.from, "Your duel challenge was not answered.", 5000, false);
		DuelTell(game, i.to, "", 0, false, false);
		SendDuelInvite(i.to, 0, false, 0, "");
	}
	for (const Invite& i : vanished) {
		for (uint32_t g : { i.from, i.to }) {
			if (g_serverUnit(game, kUnitPlayer, g) != nullptr) {
				DuelTell(game, g, "The duel challenge is over: the other player left the game.", 5000, false);
				SendDuelInvite(g, 0, false, 0, "");
			}
		}
	}
	if (duels.empty()) {
		std::lock_guard lock(g_pvpLock);
		g_duelBusy = !g_invites.empty() || !g_duels.empty() || !g_wagerSessions.empty() || !g_lobby.members.empty();
		return;
	}
	const uint32_t   cdMs = static_cast<uint32_t>(g_pvp.duelCountdown * 1000.0 + 0.5);
	std::vector<Duel> keep;
	for (Duel d : duels) {
		if (!d.team.empty()) { // 1.9.0
			if (ProcessTeamDuel(game, d, frame)) {
				keep.push_back(d);
			}
			continue;
		}
		void*      ua  = g_serverUnit(game, kUnitPlayer, d.a);
		void*      ub  = g_serverUnit(game, kUnitPlayer, d.b);
		bool       inA = ua != nullptr && LevelOf(ua) == d.level;
		bool       inB = ub != nullptr && LevelOf(ub) == d.level;
		if (d.state == kDuelMoving) {
			if (inA && inB) {
				// LEVEL_ChangeAct puts the player in the new room at once, while the client still loads (packets 0x05 .. 0x04).
				// So freeze on arrival and start counting when both clients say they are done (or at the move timeout).
				if (!d.arrived) {
					d.arrived = true;
					SetFrozen(ua, d.a, true);
					SetFrozen(ub, d.b, true);
				}
				bool loaded = DuelClientLoaded(d.a) && DuelClientLoaded(d.b);
				if (loaded && d.crossAct) {
					// The clients' load signal (packet 0x04) comes before the HD assets are in, and nothing better is known
					// (FX5 2026-11-07: a player arrived with 3 s of the countdown left). Same-act duels don't reload.
					if (!d.extraStarted) {
						// 0.8.3: every act-changing client saw its loading screen close -> a short fade-out wait; otherwise the
						// fixed wait (check off, no plugin there, or the screen was never seen).
						const bool   screens = g_pvp.duelScreenCheck && (!d.crossA || DuelScreenClosed(d.a)) && (!d.crossB || DuelScreenClosed(d.b));
						const double extra   = screens ? g_pvp.duelScreenExtra : g_pvp.duelCrossActExtra;
						d.extraStarted       = true;
						d.extraDue           = frame + static_cast<int32_t>(extra * kFramesPerSecond + 0.5);
						Log("arena: duel %u vs %u: %s: %.1f s before the countdown.", d.a, d.b, screens ? "after the loading screen" : "fixed wait", extra);
					}
					loaded = frame - d.extraDue >= 0;
				}
				if (!loaded && frame - d.due < 0) {
					if (!d.waitNoticed) {
						d.waitNoticed = true;
						Log("arena: duel %u vs %u: both in level %d, waiting for their clients to load.", d.a, d.b, d.level);
						const uint32_t waitMs = static_cast<uint32_t>(g_pvp.duelMoveTimeout * 1000.0 + 0.5);
						DuelTell(game, d.a, "Waiting for both players to finish loading.", waitMs, false, false);
						DuelTell(game, d.b, "Waiting for both players to finish loading.", waitMs, false, false);
					}
					keep.push_back(d);
					continue;
				}
				if (d.waitNoticed) {
					DuelTell(game, d.a, "", 0, false, false);
					DuelTell(game, d.b, "", 0, false, false);
				}
				if (d.walkIn) {
					PlaceWalkIn(game, ua, ub, d.level);
				}
				d.state = kDuelCountdown;
				d.due   = frame + static_cast<int32_t>(g_pvp.duelCountdown * kFramesPerSecond + 0.5);
				SendDuelCountdown(d.a, cdMs);
				SendDuelCountdown(d.b, cdMs);
				Log("arena: duel %u vs %u: both in level %d, countdown.", d.a, d.b, d.level);
			} else if (ua == nullptr || ub == nullptr || frame - d.due >= 0) {
				for (uint32_t g : { d.a, d.b }) {
					DuelTell(game, g, "The duel was cancelled (a player didn't arrive).", 5000, false);
				}
				d.winner = 0;
				FinishDuel(game, d);
				continue;
			}
			keep.push_back(d);
			continue;
		}
		if (d.state == kDuelCountdown || d.state == kDuelFighting) {
			// 1.0.0: a joined player who left is out; a first-two player who left hands the slot to a joined one
			for (auto it = d.joined.begin(); it != d.joined.end();) {
				void* u = g_serverUnit(game, kUnitPlayer, it->first);
				if (u != nullptr && LevelOf(u) == d.level) {
					++it;
					continue;
				}
				LeaveDuel(game, d, it->first, it->second, u);
				it = d.joined.erase(it);
			}
			while ((!inA || !inB) && !d.joined.empty()) {
				const auto j = d.joined.front();
				d.joined.erase(d.joined.begin());
				const uint32_t    gone = inA ? d.b : d.a;
				const std::string goneName = inA ? d.nameB : d.nameA;
				if (inA) {
					d.b = j.first, d.nameB = j.second, ub = g_serverUnit(game, kUnitPlayer, d.b);
				} else {
					d.a = j.first, d.nameA = j.second, ua = g_serverUnit(game, kUnitPlayer, d.a);
				}
				LeaveDuel(game, d, gone, goneName, g_serverUnit(game, kUnitPlayer, gone));
				inA = ua != nullptr && LevelOf(ua) == d.level;
				inB = ub != nullptr && LevelOf(ub) == d.level;
			}
			ProcessJoins(game, d, frame);
			if (!inA || !inB) { // left the arena or the game: forfeit
				if (!inA && !inB) {
					d.winner = 0;
					FinishDuel(game, d);
					continue;
				}
				d.winner                 = inA ? d.a : d.b;
				const std::string& wName = inA ? d.nameA : d.nameB;
				const std::string& lName = inA ? d.nameB : d.nameA;
				Broadcast(game, wName + " wins the duel against " + lName + " (forfeit).");
				SetFrozen(inA ? ua : ub, d.winner, false);
				SendDuelCountdown(d.winner, 0);
				SendDuelResult(d.winner, 1);
				if (d.wager && !d.paid) { // left the arena (leaving the game was settled in the save hook)
					d.paid = true;
					PayPot(game, d.winner, inA ? d.b : d.a, false);
					PayItems(game, d.winner, inA ? d.b : d.a, true);
				}
				d.state = kDuelEnding;
				d.due   = frame + static_cast<int32_t>(g_pvp.duelEndSeconds * kFramesPerSecond + 0.5);
				keep.push_back(d);
				continue;
			}
			if (d.state == kDuelCountdown && frame - d.due >= 0) {
				for (const auto& [g, u] : { std::pair { d.a, ua }, std::pair { d.b, ub } }) {
					if (!JoinWaiting(g)) {
						SetFrozen(u, g, false);
					}
				}
				d.state = kDuelFighting;
				if (d.rules.minutes > 0) { // 1.6.0: the time limit runs from "Fight!"
					d.endFrame = frame + d.rules.minutes * 60 * kFramesPerSecond;
					for (uint32_t g : Members(d)) {
						AloneNotice(g, "Time left", static_cast<uint32_t>(d.rules.minutes) * 60000u);
					}
				}
				Log("arena: duel %u vs %u: fight.", d.a, d.b);
			}
			if (d.state == kDuelFighting && d.endFrame != 0 && !d.suddenDeath && frame - d.endFrame >= 0) {
				EndDuelOnTime(game, d, frame);
			}
			keep.push_back(d);
			continue;
		}
		// kDuelEnding (set here on a forfeit or by GoDown on the winning kill)
		if (frame - d.due >= 0) {
			FinishDuel(game, d);
			continue;
		}
		keep.push_back(d);
	}
	std::lock_guard lock(g_pvpLock);
	// GoDown runs on this thread too, so nothing changed g_duels since the copy: write the result back
	g_duels    = keep;
	g_duelBusy = !g_invites.empty() || !g_duels.empty() || !g_wagerSessions.empty() || !g_lobby.members.empty();
}

// One player in an arena with nobody to fight (0.7.5, user 2026-11-07): there is no menu, stairs or town portal there,
// so a notice counts down alone_seconds, then the player goes to town through the last-player return (no second
// countdown: told + due now). An opponent arriving, a duel or a pending last-player return ends or skips the wait.
void AloneNotice(uint32_t guid, const std::string& text, uint32_t ms) {
	std::string payload(5, '\0');
	std::memcpy(payload.data(), &ms, 4);
	payload[4] = 2;
	payload += text.substr(0, 210);
	SendToPlayer(guid, kMsgNotice, payload.data(), static_cast<uint32_t>(payload.size()));
}
void ProcessAlone(void* game, int32_t frame) {
	if (g_pvp.aloneSeconds <= 0.0) {
		return;
	}
	std::map<int32_t, int32_t>  counts;
	std::map<int32_t, uint32_t> single;
	int                         n = 0;
	for (void* c = Field<void*>(game, kGameClients); c != nullptr && n < 64; c = Field<void*>(c, kClientNext), ++n) {
		if (Field<uint32_t>(c, kClientUnitType) != kUnitPlayer) {
			continue;
		}
		const uint32_t g = Field<uint32_t>(c, kClientUnitGuid);
		if (void* u = g_serverUnit(game, kUnitPlayer, g); u != nullptr) {
			if (const int32_t level = LevelOf(u); IsPvpLevel(level)) {
				++counts[level];
				single[level] = g;
			}
		}
	}
	std::vector<uint32_t> started, ended;
	{
		std::lock_guard lock(g_pvpLock);
		for (auto it = g_alone.begin(); it != g_alone.end();) {
			const int32_t  level = it->first;
			const uint32_t guid  = it->second.guid;
			const auto     cnt   = counts.find(level);
			if (cnt == counts.end() || cnt->second != 1 || single[level] != guid || DuelHereLocked(guid, level)) {
				ended.push_back(guid);
				it = g_alone.erase(it);
				continue;
			}
			const bool down = std::any_of(g_down.begin(), g_down.end(), [&](const Down& d) { return d.guid == guid; });
			if (frame - it->second.due >= 0 && !down) {
				g_returns.push_back({ guid, level, frame, true });
				Log("arena: player %u had no opponent in level %d for %.0f s: return to town.", guid, level, g_pvp.aloneSeconds);
				ended.push_back(guid);
				it = g_alone.erase(it);
				continue;
			}
			++it;
		}
		for (const auto& [level, count] : counts) {
			const uint32_t guid = single[level];
			if (count != 1 || g_alone.count(level) != 0 || DuelHereLocked(guid, level)
			    || std::any_of(g_returns.begin(), g_returns.end(), [&](const PendingReturn& r) { return r.guid == guid; })) {
				continue;
			}
			g_alone[level] = { guid, frame + static_cast<int32_t>(g_pvp.aloneSeconds * kFramesPerSecond + 0.5) };
			started.push_back(guid);
		}
	}
	for (uint32_t g : ended) {
		AloneNotice(g, "", 0);
	}
	for (uint32_t g : started) {
		AloneNotice(g, g_pvp.aloneText, static_cast<uint32_t>(g_pvp.aloneSeconds * 1000.0 + 0.5));
		Log("arena: player %u is alone in an arena: back to town in %.0f s without an opponent.", g, g_pvp.aloneSeconds);
	}
}

// Console / Controls keys on any player: the hosting process queues it, a joined client sends it to the host.
auto SendDuelCmd(uint8_t kind, const std::string& name) -> std::string {
	void* me = g_localPlayer != nullptr ? g_localPlayer(g_localIndex()) : nullptr;
	if (me == nullptr) {
		return "not in a game.";
	}
	bool host = false;
	{
		std::lock_guard lock(g_pvpLock);
		host = g_game != nullptr;
	}
	if (host) {
		g_localGuid = Field<uint32_t>(me, 0x08);
		{
			std::lock_guard lock(g_duelCmdLock);
			g_duelCmds.push_back({ Field<uint32_t>(me, 0x08), kind, name });
		}
		QueueTick();
		return "";
	}
	if (g_net == nullptr) {
		return "no connection to the host (network service missing).";
	}
	std::string payload(1, static_cast<char>(kind));
	payload += name.substr(0, 32);
	if (g_net->sendToHost(g_ctx, g_channel, kMsgDuelCmd, payload.data(), static_cast<uint32_t>(payload.size())) != D2RL::Network::Result::Success) {
		return "could not reach the host.";
	}
	return "";
}


void __cdecl Tick(const D2RL::PluginContext*, void* queuedEpoch) noexcept {
	if (reinterpret_cast<uintptr_t>(queuedEpoch) != g_tickEpoch.load()) {
		return; // from before a session change and not dropped (the session value can repeat); never two tick chains
	}
	g_tickQueued = 0;
	void* game = nullptr;
	{
		std::lock_guard lock(g_pvpLock);
		game = g_game;
	}
	if (game == nullptr) {
		return;
	}
	if (const uint32_t guid = g_leaveGuid.exchange(0); guid != 0) {
		void*         u     = g_serverUnit(game, kUnitPlayer, guid);
		const int32_t level = u != nullptr ? LevelOf(u) : 0;
		if (u != nullptr && g_config.arenas.count(level) != 0 && !IsDownPlayer(u)) {
			LeaveArena(game, u, level);
		} else {
			Log("arena: `arena leave` for player %u ignored (not in an arena level, or down).", guid);
		}
	}
	const int32_t         frame = Field<int32_t>(game, kGameFrame);
	std::vector<uint32_t> due;
	bool                  waiting = false;
	{
		std::lock_guard lock(g_pvpLock);
		for (auto it = g_down.begin(); it != g_down.end();) {
			if (const Duel* dl = DuelOfLocked(it->guid); dl != nullptr && dl->state == kDuelEnding) {
				waiting = true, ++it; // 0.8.4: the loser stays down through the end pause (FinishDuel respawns them)
				continue;
			}
			if (frame - it->due >= 0) {
				due.push_back(it->guid);
				it = g_down.erase(it);
			} else {
				waiting = true, ++it;
			}
		}
		g_downCount = static_cast<int32_t>(g_down.size());
	}
	for (uint32_t guid : due) {
		Respawn(game, guid);
	}
	std::vector<uint32_t> clean;
	{
		std::lock_guard lock(g_pvpLock);
		clean.swap(g_cleanQueue);
	}
	std::sort(clean.begin(), clean.end());
	clean.erase(std::unique(clean.begin(), clean.end()), clean.end());
	for (uint32_t guid : clean) {
		void* u = g_pvp.cleanStart ? g_serverUnit(game, kUnitPlayer, guid) : nullptr;
		if (u != nullptr && !IsDown(guid) && IsPvpLevel(LevelOf(u))) {
			CleanStart(game, u);
			Log("arena: clean start for player %u (hostility began).", guid);
		}
	}
	if (waiting && frame % 5 == 0) {
		std::vector<uint32_t> downs;
		{
			std::lock_guard lock(g_pvpLock);
			for (const Down& d : g_down) {
				downs.push_back(d.guid);
			}
		}
		for (uint32_t guid : downs) {
			if (void* u = g_serverUnit(game, kUnitPlayer, guid); u != nullptr) {
				g_resync(u);
			}
		}
	}
	if (frame % kFramesPerSecond == 0) {
		SyncHostility(game); // party changes, players leaving the game
		ProcessWagers(game);
	}
	CheckHeldPets(game);
	ProcessBodies(game, frame);
	ProcessReturns(game, frame);
	ProcessDuelCmds(game, frame);
	ProcessWagerCmds(game, frame);
	LobbyTick(game, frame);
	ProcessTradeStarts(game, frame);
	ProcessAlone(game, frame); // before the duel steps: its end-of-wait clear must not wipe their "waiting" notice
	ProcessWalkIns(game, frame);
	ProcessDuels(game, frame);
	if (g_wantEntrances.exchange(false)) {
		EnsureEntrances(game);
		EnsureLobby(game);
	}
	if (const uint64_t here = g_lobbyHere.exchange(0); here != 0) {
		const std::pair<int32_t, int32_t> spot { static_cast<int32_t>(here >> 32), static_cast<int32_t>(here & 0xFFFFFFFF) };
		const size_t idx = static_cast<size_t>(std::clamp(g_lobbyHereIndex.load(), 0, static_cast<int>(kLobbySpotsMax) - 1));
		{
			std::lock_guard lock(g_lobbySpotLock);
			if (idx < g_config.lobbySpots.size()) {
				g_config.lobbySpots[idx] = spot;
			} else {
				g_config.lobbySpots.push_back(spot); // a gap (n past the end) just adds the next one
			}
		}
		EnsureLobby(game);
	}
	if (waiting || g_anyInArena || g_duelBusy || g_heldCount.load() != 0) {
		QueueTick();
	}
}

auto IsArenaPlayer(void* unit) -> bool {
	return unit != nullptr && Field<uint32_t>(unit, 0x00) == kUnitPlayer && IsPvpLevel(LevelOf(unit));
}
auto IsDown(uint32_t guid) -> bool {
	if (g_downCount.load(std::memory_order_relaxed) == 0) {
		return false;
	}
	std::lock_guard lock(g_pvpLock);
	return std::any_of(g_down.begin(), g_down.end(), [&](const Down& d) { return d.guid == guid; });
}

// An arena player "dies" (game thread): full life, frozen in place (stopped, state 0x36, untargetable, movement and
// skill packets dropped), scored, countdown; Respawn() after respawn_seconds. No death mode at all: the life never
// reaches 0 (ArenaExecEvents / ArenaCalcTotal clamp the lethal blow), so the client never starts its own death
// (P2 2026-11-01: with the DEATH animation the client stayed dead after the server-side respawn).
auto ResolveKiller(void* game, void* unit, void* attacker) -> void*; // below, next to g_lastHitBy
// 2.2.0 (user 2026-11-08): kill bonus - the killer gets kill_bonus_life % of the missing life and kill_bonus_mana % of the
// missing mana, sent to the client as Respawn() does (the client keeps its own value otherwise).
void KillBonus(void* killer) {
	const int32_t pct[2] = { g_pvp.killBonusLife, g_pvp.killBonusMana };
	const int32_t cur[2] = { 6, 8 }, max[2] = { 7, 9 }; // life / mana (8-bit fixed point)
	for (int i = 0; i < 2; ++i) {
		const int32_t c = g_readStat(killer, cur[i], 0), m = g_readStat(killer, max[i], 0);
		if (m > c && pct[i] > 0) {
			const int32_t v = c + static_cast<int32_t>(static_cast<int64_t>(m - c) * pct[i] / 100);
			g_setStat(killer, cur[i], v, 0);
			g_updateAttr(killer, cur[i], static_cast<uint32_t>(v), killer, 0);
		}
	}
}
void GoDown(void* game, void* unit, void* attacker) {
	NoteGame(game);
	attacker = ResolveKiller(game, unit, attacker); // a pet / merc kill counts for its owner (also for the kill bonus, 2.2.1)
	g_removeStates(unit); // poison, chill, freeze, curses (P2 2026-11-01: poison carried over into the respawn)
	RestoreFull(unit);
	const uint32_t guid  = Field<uint32_t>(unit, 0x08);
	const int32_t  frame = Field<int32_t>(game, kGameFrame);
	std::string    line, duelWin;
	uint32_t       payWinner = 0;
	std::vector<uint32_t> ffaLosers; // 1.0.0: the other players of a free for all when it is won (1.9.0: the losing team)
	std::vector<uint32_t> teamWinners; // 1.9.0: the winning team, but the killer
	bool                  eliminated = false; // 1.9.0: fell in a team sudden death, stays down
	bool                  bonus      = false; // 2.2.0: the killer gets the kill bonus
	{
		std::lock_guard lock(g_pvpLock);
		if (std::any_of(g_down.begin(), g_down.end(), [&](const Down& d) { return d.guid == guid; })) {
			return; // already down (a second lethal hit during the countdown)
		}
		g_down.push_back({ guid, frame + static_cast<int32_t>(g_pvp.respawnSeconds * kFramesPerSecond + 0.5) });
		g_downCount = static_cast<int32_t>(g_down.size());
		PlayerScore& v = g_scores[guid];
		v.name         = NameOf(unit);
		++v.deaths;
		const bool byPlayer = attacker != nullptr && Field<uint32_t>(attacker, 0x00) == kUnitPlayer;
		char       buf[200] {};
		Duel*      duel     = DuelOfLocked(guid);
		const bool duelKill = duel != nullptr && duel->state == kDuelFighting && byPlayer && attacker != unit
		                   && IsMember(*duel, Field<uint32_t>(attacker, 0x08))
		                   && (duel->team.empty() || TeamRelationLocked(guid, Field<uint32_t>(attacker, 0x08)) == 2);
		const bool ffa      = duel != nullptr && !duel->joined.empty();
		if (ffa && !duelKill && duel->state == kDuelFighting) { // 1.0.0: nobody in the duel caused it: it only costs the point
			v.score += g_pvp.playerDeath;
			std::snprintf(buf, sizeof(buf), "%s died (%s %d)", v.name.c_str(), v.name.c_str(), v.score);
		} else if (duel != nullptr && !duelKill) { // duel: monster deaths and suicides don't count (user 2026-11-01)
			--v.deaths;
			std::snprintf(buf, sizeof(buf), "%s died (not counted in the duel)", v.name.c_str());
		} else if (attacker == unit) {
			v.score += g_pvp.suicide;
			std::snprintf(buf, sizeof(buf), "%s killed themself (%s %d)", v.name.c_str(), v.name.c_str(), v.score);
		} else if (byPlayer) {
			const uint32_t kg   = Field<uint32_t>(attacker, 0x08);
			PlayerScore&   k    = g_scores[kg];
			k.name              = NameOf(attacker);
			const int32_t vBase = v.score;
			++k.kills;
			if (duelKill) { // 1.6.0: the duel's rules
				const DuelRules& r = duel->rules;
				if (r.type == kTypeKills) {
					k.score += 1; // the score is the kill count
				} else {
					k.score += r.killWorth;
					v.score -= r.deathPenalty;
				}
				if (!duel->team.empty()) { // 1.9.0: the teams' totals decide
					const uint8_t kt = duel->team[kg], vt = duel->team[guid];
					if (r.type == kTypeKills) {
						duel->teamScore[kt] += 1;
					} else {
						duel->teamScore[kt] += r.killWorth;
						duel->teamScore[vt] -= r.deathPenalty;
					}
				}
			} else {
				k.score += g_pvp.playerKill + vBase * g_pvp.playerKillPercent / 100;
				v.score += g_pvp.playerDeath + vBase * g_pvp.playerDeathPercent / 100;
			}
			if (duelKill && duel->rules.type == kTypeKills) { // 2.3.0: a Kills duel talks in kills
				std::snprintf(buf, sizeof(buf), "%s killed %s (%s %d kills, %s %d kills)", k.name.c_str(), v.name.c_str(), k.name.c_str(), k.kills, v.name.c_str(), v.kills);
			} else {
				std::snprintf(buf, sizeof(buf), "%s killed %s (%s %d, %s %d)", k.name.c_str(), v.name.c_str(), k.name.c_str(), k.score, v.name.c_str(), v.score);
			}
			// 2.2.1 (user): pet and merc kills give their owner the bonus too; only the killer (owner) gets it, never teammates
			// 2.3.1 (user): every walk-in duel (1 v 1 or Battle Royale) uses walk_in_kill_bonus (on by default); menu / team duels
			// use their rule row
			bonus = duel != nullptr ? (duel->walkIn ? g_pvp.walkInKillBonus : duel->rules.killBonus != 0) : g_pvp.killBonus; // applied below
			const bool    teamDuel = duelKill && !duel->team.empty();
			const uint8_t kTeam    = teamDuel ? duel->team[kg] : 0;
			// 1.9.0 (user 2026-11-08): a team duel's sudden death is last one standing - the fallen stay down, the team
			// with nobody left standing loses
			bool lastDown = false;
			if (teamDuel && duel->suddenDeath) {
				const uint8_t vt = duel->team[guid];
				lastDown         = std::all_of(duel->team.begin(), duel->team.end(), [&](const auto& m) {
                    return m.second != vt || std::any_of(g_down.begin(), g_down.end(), [&](const Down& x) { return x.guid == m.first; });
                });
				if (!lastDown) {
					eliminated = true;
					for (Down& x : g_down) {
						if (x.guid == guid) {
							x.due = frame + 3600 * kFramesPerSecond; // no respawn: FinishDuel brings them back
						}
					}
				}
			}
			const bool won = duelKill && (teamDuel && duel->suddenDeath ? lastDown
			                                                            : duel->suddenDeath || (duel->rules.type != kTypeTimed && (teamDuel ? duel->teamScore[kTeam] : duel->rules.type == kTypeKills ? k.kills : k.score) >= duel->rules.target));
			if (won) {
				if (duel->wager && !duel->paid) {
					duel->paid = true;
					payWinner  = kg;
				}
				duel->winner = kg;
				duel->state  = kDuelEnding;
				duel->due    = frame + static_cast<int32_t>(g_pvp.duelEndSeconds * kFramesPerSecond + 0.5);
				char w[200] {};
				if (teamDuel) {
					if (duel->suddenDeath) {
						std::snprintf(w, sizeof(w), "%s wins the team duel (last ones standing).", duel->teamName[kTeam].c_str());
					} else {
						std::snprintf(w, sizeof(w), "%s wins the team duel (%d : %d).", duel->teamName[kTeam].c_str(), duel->teamScore[kTeam], duel->teamScore[kTeam ^ 1]);
					}
					for (const auto& [g, side] : duel->team) {
						if (g != kg && g != guid) {
							(side == kTeam ? teamWinners : ffaLosers).push_back(g);
						}
					}
				} else if (ffa) {
					std::snprintf(w, sizeof(w), "%s wins the duel.", k.name.c_str());
					for (uint32_t g : Members(*duel)) {
						if (g != kg && g != guid) {
							ffaLosers.push_back(g);
						}
					}
				} else {
					const bool kills = duel->rules.type == kTypeKills; // 2.3.0
					std::snprintf(w, sizeof(w), "%s wins the duel against %s (%d : %d%s).", k.name.c_str(), v.name.c_str(), kills ? k.kills : k.score,
					              kills ? v.kills : v.score, kills ? " kills" : "");
				}
				duelWin = w;
			}
		} else {
			v.score += g_pvp.monsterDeath;
			std::snprintf(buf, sizeof(buf), "%s was killed (%s %d)", v.name.c_str(), v.name.c_str(), v.score);
		}
		line = buf;
	}
	g_changeMode2(game, unit, nullptr, kPlrModeNeutral, 0, 0, 1); // stop walking / casting where the blow landed
	Freeze(unit, true);
	g_resync(unit);
	HoldOwnerPets(game, unit);
	g_deathsHandled.fetch_add(1, std::memory_order_relaxed);
	if (bonus && duelWin.empty() && attacker != nullptr && attacker != unit && !IsDownPlayer(attacker)) {
		KillBonus(attacker); // not for the duel-ending kill (the fight is over)
	}
	if (g_pvp.killMessages) {
		Broadcast(game, line);
	}
	Log("arena: %s", line.c_str());
	if (!duelWin.empty()) {
		Broadcast(game, duelWin);
		Log("arena: %s", duelWin.c_str());
	}
	if (payWinner != 0) {
		PayPot(game, payWinner, guid, false);
		PayItems(game, payWinner, guid, true);
	}
	const double downSeconds = duelWin.empty() ? g_pvp.respawnSeconds : g_pvp.duelEndSeconds; // a lost duel: down until the warp
	TellClient(guid, kMsgCountdown, eliminated ? 0u : static_cast<uint32_t>(downSeconds * 1000.0 + 0.5));
	if (void* client = g_client(unit); client != nullptr) { // chat line to the downed player only (works without the panel)
		char down[120] {};
		if (eliminated) {
			std::snprintf(down, sizeof(down), "You are out: sudden death, no respawn - the last team standing wins.");
			SendSudden(guid, kSuddenOut);
		} else {
			std::snprintf(down, sizeof(down), duelWin.empty() ? "You are down - respawning in %.0f seconds." : "You lost the duel - back to town in %.0f seconds.",
			              downSeconds);
		}
		SendLine(client, guid, down);
	}
	if (!duelWin.empty()) {
		SendDuelResult(guid, 2);
		if (attacker != nullptr) {
			SendDuelResult(Field<uint32_t>(attacker, 0x08), 1);
		}
		for (uint32_t g : teamWinners) {
			SendDuelResult(g, 1);
		}
		for (uint32_t g : ffaLosers) { // still standing: they lost too, and nobody fights on through the end pause
			if (void* o = g_serverUnit(game, kUnitPlayer, g); o != nullptr) {
				if (!IsDownPlayer(o)) {
					SetFrozen(o, g, true);
				}
				SendDuelResult(g, 2);
			}
		}
	}
	QueueTick();
}

// gPlrModeChart[DEATH].pfnStartID (game thread): only deaths the clamp did not catch (e.g. a poison tick). The stock
// StartID (the DEATH mode itself) is skipped; the client may still have seen life 0 there.
void __fastcall ArenaDeath(void* game, void* unit, int32_t mode, void* attacker) {
	if (!IsArenaPlayer(unit)) {
		g_origDeath(game, unit, mode, attacker);
		return;
	}
	GoDown(game, unit, attacker);
}

// ---- lethal-blow clamp (game thread) ----
// D2DamageStrc (notes/damage.md): +0x04 wResultFlags (2 = will die, 0x400 = forced kill, D2R), +0x134 dwDmgTotal
// (life damage, 8-bit fixed point). SUNITDMG_ExecuteEvents 0x14044CE80 sets life = life - total, 0 below 256.
constexpr size_t   kDmgResultFlags = 0x04;
constexpr size_t   kDmgTotal       = 0x134;
constexpr uint16_t kResultWillDie  = 0x2;
constexpr uint16_t kResultForced   = 0x400;
thread_local bool  t_calcLethal    = false;   // set by ArenaCalcTotal inside the ExecuteEvents call of a missile hit
thread_local void* t_execDefender  = nullptr; // defender of the running ExecuteEvents call
std::map<uint32_t, uint32_t> g_lastHitBy;     // victim guid -> last player attacker guid (poison kill credit; game thread)

// Who gets the kill (0.7.7, user 2026-11-07: no suicides, reflected damage belongs to whoever caused it).
// In a running duel only the opponent can have caused it, directly or through a pet, curse, aura or item stat. Otherwise
// a pet or merc counts for its owner, and an unknown source (or the victim itself) for the last player who hit them.
// Thorns and "attacker takes damage" already pass the stat's owner as attacker (SKILLS_ApplyThornsDamage,
// D2RCORE_SKILLITEM_ApplyProcDamage); the Ruff thorns patch is not needed here, GoDown runs from our ExecuteEvents hook.
auto ResolveKiller(void* game, void* unit, void* attacker) -> void* {
	const uint32_t guid = Field<uint32_t>(unit, 0x08);
	{
		std::lock_guard lock(g_pvpLock);
		if (const Duel* d = DuelOfLocked(guid); d != nullptr && d->state == kDuelFighting && d->joined.empty() && d->team.empty()) {
			if (void* other = g_serverUnit(game, kUnitPlayer, d->a == guid ? d->b : d->a); other != nullptr) {
				return other;
			}
		}
	}
	if (attacker != nullptr && Field<uint32_t>(attacker, 0x00) == kUnitMonster && g_minionOwner != nullptr) {
		if (void* owner = g_minionOwner(attacker); owner != nullptr) {
			attacker = owner;
		}
	}
	if (attacker == nullptr || attacker == unit || Field<uint32_t>(attacker, 0x00) != kUnitPlayer) {
		if (const auto it = g_lastHitBy.find(guid); it != g_lastHitBy.end()) {
			if (void* by = g_serverUnit(game, kUnitPlayer, it->second); by != nullptr) {
				return by;
			}
		}
	}
	return attacker;
}
// Iron Maiden (D2GAME_EventFunc04_6FD0E840) passes the unit that was hit as attacker, not the caster: the caster is the
// owner of the curse's stat list on the victim. Its result flags (txt 0x4001 | 0x20) equal thorns' 0x4021, so a cursed
// player dying to someone's thorns also counts for the curse's caster (only possible with 3+ players, walk_in = false).
constexpr int32_t  kStateIronMaiden = 55;   // excel_txt states.txt
constexpr uint16_t kResultReflected = 0x20; // flag 0x20 of reflected damage
auto IronMaidenCaster(void* game, void* victim) -> void* {
	void* list = g_stateStatList(victim, kStateIronMaiden);
	return list != nullptr ? g_serverUnit(game, g_listOwnerType(list), g_listOwnerGuid(list)) : nullptr;
}

// Leaves the player at least 1 life (256); returns true when the blow would have killed.
auto ClampLethal(void* unit, uint8_t* dmg) -> bool {
	auto&         total  = *reinterpret_cast<int32_t*>(dmg + kDmgTotal);
	auto&         result = *reinterpret_cast<uint16_t*>(dmg + kDmgResultFlags);
	bool          lethal = (result & kResultForced) != 0;
	const int32_t life   = g_readStat(unit, 6, 0);
	result               = static_cast<uint16_t>(result & ~kResultForced);
	if (total > 0 && life - total < 256) {
		total  = std::max(0, life - 256);
		lethal = true;
	}
	return lethal;
}

using CalcTotalFn  = void(__fastcall*)(void* game, void* attacker, void* defender, uint8_t* damage);
using ExecEventsFn = void(__fastcall*)(void* game, void* attacker, void* defender, uint64_t bMissile, uint8_t* damage);
std::atomic<CalcTotalFn>  g_origCalcTotal { nullptr };
std::atomic<ExecEventsFn> g_origExec { nullptr };

// SUNITDMG_CalculateTotalDamage 0x14044DF10 (melee: from SUNITDMG_AllocCombat; missiles: inside ExecuteEvents).
void __fastcall ArenaCalcTotal(void* game, void* attacker, void* defender, uint8_t* damage) {
	const CalcTotalFn orig = g_origCalcTotal.load(std::memory_order_acquire);
	if (orig != nullptr) {
		orig(game, attacker, defender, damage);
	}
	// Only the missile path (CalculateTotalDamage inside ExecuteEvents). Melee calls it from SUNITDMG_AllocCombat at the
	// start of the swing: clamping there made the later hit non-lethal (P2 2026-11-01: "sometimes 1 more hit").
	if (damage != nullptr && defender != nullptr && defender == t_execDefender && ClampLethal(defender, damage)) {
		t_calcLethal = true;
	}
}

// SUNITDMG_ExecuteEvents 0x14044CE80: applies the life loss, then the caller (SUNITDMG_ExecuteMissileDamage) turns
// result flag 2 into PLAYERMODE_Change(DEATH) - unless the unit has state 0x36 (it returns early then).
void __fastcall ArenaExecEvents(void* game, void* attacker, void* defender, uint64_t bMissile, uint8_t* damage) {
	const ExecEventsFn orig = g_origExec.load(std::memory_order_acquire);
	if (orig == nullptr) {
		return;
	}
	if (damage == nullptr || !IsArenaPlayer(defender)) {
		orig(game, attacker, defender, bMissile, damage);
		return;
	}
	auto& result = *reinterpret_cast<uint16_t*>(damage + kDmgResultFlags);
	if (IsDown(Field<uint32_t>(defender, 0x08))) {
		result = static_cast<uint16_t>(result & ~kResultWillDie); // no damage while down
		return;
	}
	if (attacker != nullptr && attacker != defender && Field<uint32_t>(attacker, 0x00) == kUnitPlayer) {
		g_lastHitBy[Field<uint32_t>(defender, 0x08)] = Field<uint32_t>(attacker, 0x08);
	}
	const bool  trace    = g_pvp.traceDamage;
	const int32_t lifeIn = trace ? g_readStat(defender, 6, 0) : 0;
	if (trace) {
		Log("arena: trace: in  attacker %u (type %u) -> defender %u, missile %d, hit flags 0x%X, result 0x%X, phys %d, total %d, life %d; Iron Maiden on attacker %d, on defender %d.",
		    attacker != nullptr ? Field<uint32_t>(attacker, 0x08) : 0, attacker != nullptr ? Field<uint32_t>(attacker, 0x00) : 99, Field<uint32_t>(defender, 0x08),
		    static_cast<int32_t>(bMissile), *reinterpret_cast<uint32_t*>(damage), static_cast<uint32_t>(result), *reinterpret_cast<int32_t*>(damage + 0x18),
		    *reinterpret_cast<int32_t*>(damage + kDmgTotal), lifeIn,
		    attacker != nullptr && g_stateStatList(attacker, kStateIronMaiden) != nullptr ? 1 : 0, g_stateStatList(defender, kStateIronMaiden) != nullptr ? 1 : 0);
	}
	bool        lethal   = static_cast<int32_t>(bMissile) == 0 && ClampLethal(defender, damage); // melee: total already known
	const bool  outer    = t_calcLethal;
	void* const outerDef = t_execDefender;
	t_calcLethal         = false;
	t_execDefender       = defender;
	orig(game, attacker, defender, bMissile, damage);
	lethal         = lethal || t_calcLethal || g_readStat(defender, 6, 0) < 256;
	if (trace) {
		Log("arena: trace: out defender %u, result 0x%X, phys %d, total %d, life %d -> %d.", Field<uint32_t>(defender, 0x08), static_cast<uint32_t>(result),
		    *reinterpret_cast<int32_t*>(damage + 0x18), *reinterpret_cast<int32_t*>(damage + kDmgTotal), lifeIn, g_readStat(defender, 6, 0));
	}
	t_calcLethal   = outer;
	t_execDefender = outerDef;
	if (lethal) {
		result = static_cast<uint16_t>(result & ~kResultWillDie);
		void* by = (result & kResultReflected) != 0 ? IronMaidenCaster(game, defender) : nullptr;
		if (by == nullptr && attacker == defender && g_targetUnit != nullptr) { // 2.4.23: null = credit to the attacker
			by = g_targetUnit(game, defender); // self-damage (Sacrifice: ExecuteEvents(unit, unit)): the target, or its owner in GoDown
		}
		GoDown(game, defender, by != nullptr && by != defender ? by : attacker);
	}
}

// Player event table [3] = D2GAME_EVENTS_StatRegen_6FC80F80 0x14042E600 (every frame): stock floors life at 0x100, so
// poison / open wounds / burn never kill a player. In the arena a tick that would take the last life point downs the
// player instead (user 2026-11-01); the kill goes to the last player who hit them.
PlrEventFn g_origRegen = nullptr;
void __fastcall ArenaStatRegen(void* game, void* unit, uint64_t a3, uint64_t a4, uint64_t a5) {
	if (IsArenaPlayer(unit)) {
		const int32_t regen = g_readStat(unit, 0x4A, 0);
		if (regen < 0 && g_readStat(unit, 6, 0) + regen < 256 && !IsDown(Field<uint32_t>(unit, 0x08))) {
			void* by = nullptr;
			if (const auto it = g_lastHitBy.find(Field<uint32_t>(unit, 0x08)); it != g_lastHitBy.end()) {
				by = g_serverUnit(game, kUnitPlayer, it->second);
			}
			GoDown(game, unit, by);
		}
	}
	g_origRegen(game, unit, a3, a4, a5);
}

// ---- potions in the arena (game thread) ----
// gpPSpellPrepareTable slots 3 (healing / mana / rejuvenation), 4 (antidote / thawing), 6 (stamina): every potion use
// (belt, inventory, shift-click to the merc, the deferred replay) asks here first; false = the item stays, nothing is
// sent (combat-rules.md 2). Per kind: a limit per arena visit (reset on entering an arena) and a cooldown.
using PrepareFn = bool(__fastcall*)(void* game, void* unit, void* item);
std::array<PrepareFn, 7> g_origPrepare {};
struct PotionUse {
	std::array<int32_t, 4> used {};
	std::array<int32_t, 4> lastFrame { INT32_MIN / 2, INT32_MIN / 2, INT32_MIN / 2, INT32_MIN / 2 };
};
std::map<uint32_t, PotionUse> g_potions; // player guid -> uses in the current arena visit (game thread)
void ResetPotions(uint32_t guid) {
	g_potions.erase(guid);
}
constexpr int32_t kItemTypeHealing = 77, kItemTypeMana = 78, kItemTypeRejuv = 79; // itemtypes.txt rows (no Expansion row)
constexpr const char* kPotionNames[] = { "healing", "mana", "rejuvenation", "other" };

void TellPlayer(void* player, const std::string& text) {
	if (void* client = g_client(player); client != nullptr) {
		SendLine(client, Field<uint32_t>(player, 0x08), text);
	}
	if (g_config.refusalSound > 0) {
		g_attachSound(player, g_config.refusalSound, nullptr);
	}
}

template <int Slot>
bool __fastcall PotionGate(void* game, void* unit, void* item) {
	if (!g_origPrepare[Slot](game, unit, item)) {
		return false; // stock refusal (e.g. full life)
	}
	if (unit == nullptr || !IsPvpLevel(LevelOf(unit))) {
		return true;
	}
	if (Field<uint32_t>(unit, 0x00) != kUnitPlayer) {
		return g_pvp.mercPotions; // a mercenary (shift-click) in the arena
	}
	int kind = 3;
	if (Slot == 3) {
		kind = g_itemType(item, kItemTypeHealing) != 0 ? 0 : g_itemType(item, kItemTypeMana) != 0 ? 1 : g_itemType(item, kItemTypeRejuv) != 0 ? 2 : 3;
	}
	PotionUse&    u     = g_potions[Field<uint32_t>(unit, 0x08)];
	const int32_t frame = Field<int32_t>(game, kGameFrame);
	int32_t       limit = g_pvp.potionLimit[kind];
	double        cdSec = g_pvp.potionCooldown;
	{
		std::lock_guard lock(g_pvpLock); // 1.6.0: a duel from the rules page uses its own potion rules
		if (const Duel* d = DuelOfLocked(Field<uint32_t>(unit, 0x08)); d != nullptr && d->customRules) {
			limit = d->rules.potionLimit;
			cdSec = d->rules.potionCd;
		}
	}
	char          text[128] {};
	if (limit > 0 && u.used[kind] >= limit) {
		std::snprintf(text, sizeof(text), "No %s potions left in this fight (%d/%d).", kPotionNames[kind], u.used[kind], limit);
		TellPlayer(unit, text);
		return false;
	}
	const int32_t cd = static_cast<int32_t>(cdSec * kFramesPerSecond + 0.5);
	if (cd > 0 && frame - u.lastFrame[kind] < cd) {
		std::snprintf(text, sizeof(text), "Wait %.1f s before the next %s potion.", static_cast<double>(cd - (frame - u.lastFrame[kind])) / kFramesPerSecond, kPotionNames[kind]);
		TellPlayer(unit, text);
		return false;
	}
	++u.used[kind];
	u.lastFrame[kind] = frame;
	return true;
}

// ---- movement / skill packets of a downed player (game thread) ----
// gpPacketCallbackTable 0x141D2A790 (8-byte slots): 0x01-0x12 walk/run/skills, 0x42 D2R directional (controller)
// move. handler(game, unit, packet, size); 0 = handled (3 would kick the client).
using PacketFn = uint64_t(__fastcall*)(void* game, void* unit, uint8_t* packet, uint32_t size);
std::array<PacketFn, 0x43> g_origPacket {};
// 1.7.0 duel rule "Teleport cooldown" (notes/arena-skill-cooldown.md, RE session 2026-11-08): the stock per-skill
// cooldown (state 185 / stat 359, checked by SKILLS_CheckSkillDelay) refuses casts by itself once started. Started
// here: a new cast shows as a changed cast id (player +0x12C); the skill is UNITS_GetUsedSkill; the server setter is
// maybe_D2GAME_SetLocalCooldown 0x140439500 (game, unit, frames, skill); the client is told with kMsgSkillCd and calls
// CLIENT_SetLocalCooldown 0x140217B90 (unit, frames, skill) so its icon and own gate match.
using UsedSkillFn   = void*(__fastcall*)(void* unit);                                       // UNITS_GetUsedSkill 0x14034BA40
using SkillIdFn     = int32_t(__fastcall*)(void* skill);                                    // SKILLS_GetSkillIdFromSkill 0x14033E080
using SrvSetCdFn    = void(__fastcall*)(void* game, void* unit, int32_t frames, int32_t skill); // maybe_D2GAME_SetLocalCooldown
using CltSetCdFn    = void(__fastcall*)(void* unit, int32_t frames, int32_t skill);         // CLIENT_SetLocalCooldown
UsedSkillFn g_usedSkill = nullptr;
SkillIdFn   g_skillId   = nullptr;
SrvSetCdFn  g_srvSetCd  = nullptr;
CltSetCdFn  g_cltSetCd  = nullptr;
constexpr int32_t kSkillTeleport = 54; // skills.txt *Id
auto DuelTeleCd(uint32_t guid) -> int32_t {
	if (!g_duelBusy.load(std::memory_order_relaxed)) {
		return 0;
	}
	std::lock_guard lock(g_pvpLock);
	const Duel* d = DuelOfLocked(guid);
	return d != nullptr && d->customRules ? d->rules.teleCd : 0;
}
template <int N>
uint64_t __fastcall PacketGate(void* game, void* unit, uint8_t* packet, uint32_t size) {
	if (unit != nullptr && (g_downCount.load(std::memory_order_relaxed) != 0 || g_frozenCount.load(std::memory_order_relaxed) != 0)
	    && Field<uint32_t>(unit, 0x00) == kUnitPlayer && (IsDown(Field<uint32_t>(unit, 0x08)) || IsFrozen(Field<uint32_t>(unit, 0x08)))) {
		g_packetDrops[N].fetch_add(1, std::memory_order_relaxed);
		g_resync(unit); // the client ran ahead locally: snap it back (P2 2026-11-01: only the downed player saw the move)
		return 0;
	}
	if (g_srvSetCd != nullptr && unit != nullptr && Field<uint32_t>(unit, 0x00) == kUnitPlayer) {
		if (const int32_t cdSec = DuelTeleCd(Field<uint32_t>(unit, 0x08)); cdSec > 0) {
			const uint32_t castBefore = Field<uint32_t>(unit, 0x12C);
			const uint64_t r          = g_origPacket[N](game, unit, packet, size);
			if (Field<uint32_t>(unit, 0x12C) != castBefore) {
				void* skill = g_usedSkill(unit);
				if (skill != nullptr && g_skillId(skill) == kSkillTeleport) {
					const int32_t frames = cdSec * kFramesPerSecond;
					g_srvSetCd(game, unit, frames, kSkillTeleport);
					const uint32_t msg[2] { static_cast<uint32_t>(kSkillTeleport), static_cast<uint32_t>(frames) };
					SendToPlayer(Field<uint32_t>(unit, 0x08), kMsgSkillCd, msg, sizeof(msg));
				}
			}
			return r;
		}
	}
	return g_origPacket[N](game, unit, packet, size);
}
template <int... N>
constexpr auto MakePacketGates(std::integer_sequence<int, N...>) -> std::array<PacketFn, sizeof...(N)> {
	return { &PacketGate<N>... };
}

// ---- 1.5.0 wager trade labels (client) ----
// The exe's GetNamespacedStringById 0x143E2B730 is a thunk `JMP [0x143E2AE10]` into D2RCore (ushort id -> char*).
// While a wager trade is pending (kMsgWagerFlag), the trade strings read as wager strings. The window title comes
// from the layout (@NPCMenuTrade), so it is also set on the open TradeLayout's Title (TextBoxWidget_SetText).
using StringByIdFn = const char*(__fastcall*)(uint32_t id);
using SetTextFn    = void(__fastcall*)(void* textbox, const void* str); // TextBoxWidget_SetText 0x14086DD50: {const char*, size_t}*
std::atomic<StringByIdFn> g_origStringById { nullptr };
SetTextFn                 g_setTextNative = nullptr;
// 1.5.1: the check button's tooltip (FocusableWidget tooltipString +0x88, a game string resolved at layout load):
// STRING_AssignPointerLength 0x140080CF0 (string*, const char*, size_t), as TextBoxWidget_SetText uses it
using AssignStrFn = void(__fastcall*)(void* str, const char* data, size_t len);
AssignStrFn               g_assignStr = nullptr;
bool                      g_tradeTitleSet = false; // UI thread
auto WagerLabel(uint32_t id) -> const char* {
	switch (id & 0xFFFF) {
	case 0xD44: return "duel wager";                          // NPCMenuTrade (window title)
	case 0xFC8: return "wager";                               // strTrade
	case 0xFC9: return "accept wager";                        // strTradeAccept
	case 0xFCA: return "agree to a wager duel";               // strTradeAgreeTo (the request box)
	case 0xFCB: return "asking other player for a wager duel"; // strWaitingForOtherPlayer
	case 0x1021: return "Wager Gold";                         // strGoldTradepup
	case 0x1023: return "Accept Wager";                       // strAcceptTrade (check button tooltip)
	default: return nullptr;
	}
}
auto WagerLabelsOn() -> bool {
	const uint64_t until = g_wagerLabelsUntil.load(std::memory_order_relaxed);
	return until != 0 && NowMs() < until;
}
auto __fastcall ArenaStringById(uint32_t id) -> const char* {
	if (WagerLabelsOn()) {
		if (const char* t = WagerLabel(id); t != nullptr) {
			return t;
		}
	}
	return g_origStringById.load(std::memory_order_acquire)(id);
}
// ---- 1.5.0 item lock during duels (server; notes/arena-item-lock.md, RE session 2026-11-08) ----
// While a player is in a duel, gpPacketCallbackTable slots that move / equip / drop / socket / cube / stack the
// sender's items are refused; 0x26 (all item use) passes only for potions (item id dword at +0x0A, itemtypes row 9
// poti). Weapon swap 0x50 stays allowed. After a refusal the client's item stays "pending" (greyed) until the server
// resends it: PLRTRADE_D2R_ResendInventoryItems 0x140528F40 (game, player), throttled, never with a cursor item (its
// ToCursor resend could duplicate the cursor item client side).
constexpr uint8_t kLockedSlots[] = { 0x14, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
	                                 0x27, 0x28, 0x29, 0x2A, 0x2E, 0x45, 0x46, 0x4E, 0x51, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A,
	                                 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x63, 0x65 };
using ResendInvFn = void(__fastcall*)(void* game, void* player);
ResendInvFn                          g_resendInv = nullptr;
std::array<PacketFn, 0x70>           g_origItemPacket {};
std::array<std::atomic<uint32_t>, 0x70> g_itemDrops {};
std::map<uint32_t, uint64_t>         g_lockResend; // game thread: guid -> NowMs of the last resend
bool                                 g_itemLock = false;
auto InDuelForLock(uint32_t guid) -> bool {
	if (!g_duelBusy.load(std::memory_order_relaxed)) {
		return false;
	}
	std::lock_guard lock(g_pvpLock);
	return DuelOfLocked(guid) != nullptr;
}
template <int N>
uint64_t __fastcall ItemGate(void* game, void* unit, uint8_t* packet, uint32_t size) {
	if (unit != nullptr && Field<uint32_t>(unit, 0x00) == kUnitPlayer && InDuelForLock(Field<uint32_t>(unit, 0x08))) {
		if (N == 0x26 && packet != nullptr && size >= 0x0E) {
			uint32_t id = 0;
			std::memcpy(&id, packet + 0x0A, 4);
			if (void* item = g_serverUnit(game, 4, id); item != nullptr && g_itemType(item, 9) != 0) {
				return g_origItemPacket[N](game, unit, packet, size); // a potion: drinking stays allowed
			}
		}
		g_itemDrops[N].fetch_add(1, std::memory_order_relaxed);
		const uint32_t guid = Field<uint32_t>(unit, 0x08);
		if (g_lockTold.insert(guid).second) {
			TellPlayer(unit, "Items are locked during a duel (potions still work).");
		}
		const uint64_t now  = NowMs();
		uint64_t&      last = g_lockResend[guid];
		void*          inv  = g_getInventory(unit, __FILE__, __LINE__);
		if (now - last >= 250 && (inv == nullptr || g_cursorItem(inv) == nullptr)) {
			last = now;
			g_resendInv(game, unit); // clears the client's pending (greyed) mark
		}
		return 0;
	}
	return g_origItemPacket[N](game, unit, packet, size);
}
template <int... N>
constexpr auto MakeItemGates(std::integer_sequence<int, N...>) -> std::array<PacketFn, sizeof...(N)> {
	return { &ItemGate<N>... };
}

void InstallLabelsAndLock(const D2RL::PluginContext* ctx, const D2RL::MutationService* mut, bool lock) {
	const uintptr_t exe = ctx->exeBase;
	{ // 1.7.0 Teleport cooldown rule
		static constexpr uint8_t kUsed[]   = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9 };
		static constexpr uint8_t kSkId[]   = { 0x48, 0x85, 0xC9, 0x75, 0x03, 0x33, 0xC0, 0xC3, 0x48, 0x8B, 0x01, 0x0F };
		static constexpr uint8_t kSrvCd[]  = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40 };
		static constexpr uint8_t kCltCd[]  = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20 };
		// skill-ext inline-hooks both setters: then their first byte is a jump (E9 / FF 25); calling through it is fine
		const auto okOrHooked = [&](uint64_t rva, const uint8_t* b, size_t n) {
			const uint8_t first = *reinterpret_cast<const uint8_t*>(exe + rva);
			return ctx->CheckExpectedBytes(rva, b, static_cast<uint32_t>(n)) || first == 0xE9 || first == 0xFF;
		};
		if (ctx->CheckExpectedBytes(0x34ba40, kUsed, sizeof(kUsed)) && ctx->CheckExpectedBytes(0x33e080, kSkId, sizeof(kSkId))
		    && okOrHooked(0x439500, kSrvCd, sizeof(kSrvCd)) && okOrHooked(0x217b90, kCltCd, sizeof(kCltCd))) {
			g_usedSkill = reinterpret_cast<UsedSkillFn>(exe + 0x34ba40);
			g_skillId   = reinterpret_cast<SkillIdFn>(exe + 0x33e080);
			g_srvSetCd  = reinterpret_cast<SrvSetCdFn>(exe + 0x439500);
			g_cltSetCd  = reinterpret_cast<CltSetCdFn>(exe + 0x217b90);
		} else {
			ctx->LogWarn("arena: Teleport cooldown rule off: unexpected bytes at the skill cooldown functions.");
		}
	}
	static constexpr uint8_t kThunk[]   = { 0xFF, 0x25, 0xDA, 0xF6, 0xFF, 0xFF }; // GetNamespacedStringById: JMP [0x143E2AE10]
	static constexpr uint8_t kSetText[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x42, 0x08, 0x48, 0x8B }; // TextBoxWidget_SetText
	static constexpr uint8_t kResend[]  = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x56, 0x57 };
	struct SlotPatch {
		uint64_t rva;
		uint64_t original;
		uint64_t wrapper;
	};
	std::vector<SlotPatch> patches;
	if (ctx->CheckExpectedBytes(0x3e2b730, kThunk, sizeof(kThunk))) {
		const uint64_t cur = *reinterpret_cast<const uint64_t*>(exe + 0x3e2ae10);
		if (cur != 0) {
			g_origStringById = reinterpret_cast<StringByIdFn>(cur);
			patches.push_back({ 0x3e2ae10, cur, reinterpret_cast<uint64_t>(&ArenaStringById) });
		}
	} else {
		ctx->LogWarn("arena: wager trade labels off: unexpected bytes at GetNamespacedStringById.");
	}
	if (ctx->CheckExpectedBytes(0x86dd50, kSetText, sizeof(kSetText))) {
		g_setTextNative = reinterpret_cast<SetTextFn>(exe + 0x86dd50);
	}
	static constexpr uint8_t kAssign[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89 };
	if (ctx->CheckExpectedBytes(0x80cf0, kAssign, sizeof(kAssign))) {
		g_assignStr = reinterpret_cast<AssignStrFn>(exe + 0x80cf0);
	}
	const std::array<PacketFn, 0x70> gates = MakeItemGates(std::make_integer_sequence<int, 0x70> {});
	if (lock && ctx->CheckExpectedBytes(0x528f40, kResend, sizeof(kResend))) {
		g_resendInv = reinterpret_cast<ResendInvFn>(exe + 0x528f40);
		for (uint8_t slot : kLockedSlots) {
			const uint64_t rva = PvpRva::PacketTable + slot * 8ull;
			const uint64_t cur = *reinterpret_cast<const uint64_t*>(exe + rva);
			if (cur == 0) {
				continue;
			}
			g_origItemPacket[slot] = reinterpret_cast<PacketFn>(cur);
			patches.push_back({ rva, cur, reinterpret_cast<uint64_t>(gates[slot]) });
		}
	} else if (lock) {
		ctx->LogWarn("arena: item lock off: unexpected bytes at PLRTRADE_D2R_ResendInventoryItems.");
	}
	if (patches.empty()) {
		return;
	}
	D2RL::Mutations::TransactionHandle tx = D2RL::Mutations::InvalidTransactionHandle;
	if (mut->beginTransaction(ctx, &tx) != D2RL::Mutations::Result::Success) {
		ctx->LogWarn("arena: labels / item lock off: transaction failed.");
		return;
	}
	std::vector<std::array<uint8_t, 8>> expected(patches.size()), bytes(patches.size());
	for (size_t i = 0; i < patches.size(); ++i) {
		std::memcpy(expected[i].data(), &patches[i].original, 8);
		std::memcpy(bytes[i].data(), &patches[i].wrapper, 8);
		const D2RL::Mutations::BytePatchRequest bp {
			.structSize   = D2RL::Mutations::BytePatchRequestSize,
			.rva          = patches[i].rva,
			.expected     = expected[i].data(),
			.expectedSize = 8,
			.bytes        = bytes[i].data(),
			.size         = 8,
		};
		D2RL::Mutations::OperationHandle op = D2RL::Mutations::InvalidOperationHandle;
		if (mut->stageBytePatch(ctx, tx, &bp, &op) != D2RL::Mutations::Result::Success) {
			mut->cancelTransaction(ctx, tx);
			ctx->LogWarn("arena: labels / item lock off: a slot could not be staged (another plugin?).");
			return;
		}
	}
	D2RL::Mutations::CommitResult result { .structSize = D2RL::Mutations::CommitResultSize };
	if (mut->commit(ctx, tx, &result) != D2RL::Mutations::Result::Success || result.result != D2RL::Mutations::Result::Success) {
		ctx->LogWarn("arena: labels / item lock off: commit failed.");
		return;
	}
	g_itemLock = g_resendInv != nullptr;
	char msg[160] {};
	std::snprintf(msg, sizeof(msg), "arena: wager trade labels %s; item lock during duels %s.", g_origStringById.load() != nullptr ? "on" : "off",
	              g_itemLock ? "on" : "off");
	ctx->LogInfo(msg);
}

// After a player's NEUTRAL / WALK / RUN mode start (game thread): the game pointer, and hostility when a player
// changes level. (D2R often calls PLRMODE_StartXY_Neutral directly - e.g. the end-anim handler - so the walk/run
// entries, used by every movement click through PLRMODES_ChangeMode, are the reliable trigger; P1 2026-10-31.)
void NoteLevel(void* game, void* unit);
void __fastcall ArenaNeutral(void* game, void* unit, int32_t mode, int32_t x, int32_t y) {
	g_origNeutral(game, unit, mode, x, y);
	NoteLevel(game, unit);
}
void __fastcall ArenaMove(void* game, void* unit, int32_t mode, int32_t x, int32_t y) {
	g_origMove(game, unit, mode, x, y);
	NoteLevel(game, unit);
}
void NoteLevel(void* game, void* unit) {
	if (unit == nullptr || Field<uint32_t>(unit, 0x00) != kUnitPlayer) {
		return;
	}
	NoteGame(game);
	const uint32_t guid  = Field<uint32_t>(unit, 0x08);
	const int32_t  level = LevelOf(unit);
	if (level == g_config.entranceLevel && !g_config.entrances.empty()) {
		g_wantEntrances = true; // spawn missing entrance trapdoors in the next tick (not inside a mode start)
		QueueTick();
	}
	bool           changed;
	{
		std::lock_guard lock(g_pvpLock);
		const auto      it = g_lastLevel.find(guid);
		changed            = it == g_lastLevel.end() || it->second != level;
	}
	if (changed) {
		Log("arena: player %u is in level %d%s.", guid, level, IsPvpLevel(level) ? " (arena)" : "");
		SyncHostility(game);
		if (g_anyInArena) {
			QueueTick();
		}
	}
}

// Player event table [1] ENDANIM (game thread). The stock handler turns the end of the DEATH animation into the real
// death (corpse with the items + automap marker, pets killed, client DEAD flag, hardcore save, mode 0x11 DEAD which
// opens the client's "You have died" modal). For a downed arena player it does nothing: the player stays in the
// fallen pose until Respawn sets neutral again.
void __fastcall ArenaEndAnim(void* game, void* unit, uint64_t a3, uint64_t a4, uint64_t a5) {
	if (unit != nullptr && Field<uint32_t>(unit, 0x00) == kUnitPlayer && g_getAnimMode(unit) == kPlrModeDeath) {
		const uint32_t guid = Field<uint32_t>(unit, 0x08);
		std::lock_guard lock(g_pvpLock);
		if (std::any_of(g_down.begin(), g_down.end(), [&](const Down& d) { return d.guid == guid; })) {
			return;
		}
	}
	g_origEndAnim(game, unit, a3, a4, a5);
}

// ---- network ----
void __cdecl OnHostMessage(const D2RL::PluginContext*, D2RL::Network::ChannelHandle, D2RL::Network::PeerHandle peer, uint16_t id, const void* data,
                           uint32_t size, void*) noexcept {
	if (id == kMsgHello && data != nullptr && size >= 4) {
		uint32_t guid = 0;
		std::memcpy(&guid, data, 4);
		{
			std::lock_guard lock(g_pvpLock);
			g_peers[guid] = peer;
		}
		Log("arena: hello from player %u's client.", guid);
	} else if (id == kMsgDuelCmd && data != nullptr && size >= 1) {
		uint32_t guid = 0;
		{
			std::lock_guard lock(g_pvpLock);
			for (const auto& [g, pr] : g_peers) {
				if (pr == peer) {
					guid = g;
				}
			}
		}
		if (guid == 0) {
			Log("arena: duel command from a client without hello ignored.");
			return;
		}
		const auto* b = static_cast<const char*>(data);
		{
			std::lock_guard lock(g_duelCmdLock);
			g_duelCmds.push_back({ guid, static_cast<uint8_t>(b[0]), std::string(b + 1, strnlen(b + 1, size - 1)) });
		}
		QueueTick();
	} else if (id == kMsgWagerCmd && data != nullptr && size >= 5) {
		uint32_t guid = 0;
		{
			std::lock_guard lock(g_pvpLock);
			for (const auto& [g, pr] : g_peers) {
				if (pr == peer) {
					guid = g;
				}
			}
		}
		if (guid != 0 && static_cast<const uint8_t*>(data)[0] != kLobbyOpen) {
			uint32_t value = 0;
			std::memcpy(&value, static_cast<const uint8_t*>(data) + 1, 4);
			std::lock_guard lock(g_wagerCmdLock);
			g_wagerCmds.push_back({ guid, static_cast<const uint8_t*>(data)[0], value });
		}
		QueueTick();
	} else if (id == kMsgDuelLoaded) {
		std::lock_guard lock(g_pvpLock);
		for (const auto& [g, pr] : g_peers) {
			if (pr == peer) {
				const uint8_t ack = data != nullptr && size >= 1 ? static_cast<const uint8_t*>(data)[0] : 1;
				g_duelLoaded[g]   = ack;
				Log("arena: player %u's client finished loading the duel level%s.", g, ack == 2 ? " (loading screen closed)" : "");
			}
		}
	}
}
std::atomic<uint64_t> g_duelCdUntil { 0 }; // UI: end of the frozen duel start countdown (ms), 0 = none
std::atomic<uint64_t> g_duelLoadUntil { 0 }; // UI: stop waiting for the load (ms)
std::atomic<uint64_t> g_duelResultUntil { 0 }; // UI: end of the duel result on the HUD (ms), 0 = none
std::atomic<int32_t>  g_duelResultKind { 0 };  // 1 won, 2 lost
std::atomic<int32_t>  g_suddenKind { 0 };      // 1.9.3: sudden death banner (SuddenKind), 0 = none
std::atomic<uint64_t> g_suddenUntil { 0 };     // 1.9.7: kinds 1 / 2 end at this NowMs (0 = stays, kind 3)
std::atomic<bool>     g_duelExpectScreen { false }; // UI: this client changes act for the duel: wait for the loading screen
std::atomic<uint64_t> g_duelScreenUntil { 0 };      // UI: a loading screen not seen by then = none to wait for
bool                  g_duelScreenSeen = false;     // UI thread
bool                  g_duelScreenMissLogged = false;
std::mutex            g_noticeLock;
std::string           g_noticeText;        // UI: notice line under the scoreboard
uint64_t              g_noticeUntil   = 0;
bool                  g_noticeSeconds = false;
std::string           g_noticeBaseText;    // UI: background notice (alone countdown), shown when no other notice runs
uint64_t              g_noticeBaseUntil = 0;
// 1.1.0: what the wager window shows (UI thread reads it in UiPoll)
struct WagerView {
	uint8_t     open = 0;
	uint32_t    myGold = 0, theirGold = 0;
	bool        myReady = false, theirReady = false;
	uint16_t    points = 0;
	std::string me, them, note;
	uint32_t    seq = 0;
	bool        canSet = false;  // 1.3.0: this player sets the stake
	uint64_t    agreeAt = 0;     // NowMs from which the accept button works
	DuelRules   rules;           // 1.6.0
};
std::mutex g_wagerViewLock;
WagerView  g_wagerView;
// 1.9.0: the team duel lobby as the host sent it (UI thread reads it in UiPoll)
struct LobbyView {
	struct Member {
		uint8_t     team, flags; // flags: 1 ready, 2 leads, 4 you
		std::string name;
	};
	uint8_t             open = 0;
	uint8_t             myTeam = 255;
	bool                leader = false, myReady = false, sizesOk = false;
	bool                mix = false, revealing = false; // 1.9.1
	uint64_t            agreeAt = 0;
	DuelRules           rules;
	std::vector<Member> members;
	std::string         note;
	uint32_t            seq = 0;
};
std::mutex g_lobbyViewLock;
LobbyView  g_lobbyView;
// 1.2.0: the duel invite popup (UI thread reads it in UiPoll)
struct InviteView {
	uint64_t    until = 0; // NowMs, 0 = none
	bool        panel = false;
	uint32_t    stake = 0;
	std::string from;
	uint32_t    seq = 0;
};
std::mutex g_inviteLock;
InviteView g_inviteView;
std::atomic<bool>     g_closeTradePending { false }; // 2.4.11: host said the stake trade is off - cancel the stock trade (UI poll)
std::atomic<uint32_t> g_tradeStartGuid { 0 }; // 1.4.0: host said "send the trade request to this player" (UI poll does it)
void ClientHandle(uint16_t id, const void* data, uint32_t size) {
	if (id == kMsgTeamMates && data != nullptr && size >= 1) {
		const auto*     b = static_cast<const uint8_t*>(data);
		std::lock_guard lock(g_mapHideLock);
		g_teamMates.clear();
		for (uint32_t off = 1; b[0] != 0 && off + 4 <= size; off += 4) {
			uint32_t g = 0;
			std::memcpy(&g, b + off, 4);
			g_teamMates.insert(g);
		}
		Log("arena: automap: %zu teammate(s) with the party marker.", g_teamMates.size());
	} else if (id == kMsgMapHide && data != nullptr && size >= 1) {
		const auto*     b = static_cast<const uint8_t*>(data);
		std::lock_guard lock(g_mapHideLock);
		g_mapHidden.clear();
		g_mapRange = b[0] > 1 ? b[0] : 0;
		for (uint32_t off = 1; b[0] != 0 && off + 4 <= size; off += 4) {
			uint32_t g = 0;
			std::memcpy(&g, b + off, 4);
			g_mapHidden.insert(g);
		}
		Log("arena: automap: %zu player(s) hidden%s (range %d).", g_mapHidden.size(), g_mapRange.load() == 0xFF ? " out of sight" : g_mapRange.load() != 0 ? " when far" : "", g_mapRange.load());
	} else if (id == kMsgSkillCd && data != nullptr && size >= 8) {
		uint32_t m[2] {};
		std::memcpy(m, data, 8);
		g_pendingSkillCd = (static_cast<uint64_t>(m[0]) << 32) | m[1];
		StartUiPoll();
	} else if (id == kMsgWagerFlag && data != nullptr && size >= 1) {
		g_wagerLabelsUntil = static_cast<const uint8_t*>(data)[0] != 0 ? NowMs() + 600000 : 0; // 2.4.11: host ends it (2 min close)
		if (static_cast<const uint8_t*>(data)[0] != 0) {
			g_stakeInvCheck = true; // 2.4.14: right frame on the client's stake trade popup
		}
		StartUiPoll();
	} else if (id == kMsgCloseTrade) { // 2.4.11
		g_closeTradePending = true;
		StartUiPoll();
	} else if (id == kMsgStartTrade && data != nullptr && size >= 4) {
		uint32_t g = 0;
		std::memcpy(&g, data, 4);
		g_tradeStartGuid = g;
	} else if (id == kMsgDuelInvite && data != nullptr && size >= 9) {
		uint32_t ms = 0, stake = 0;
		std::memcpy(&ms, data, 4);
		std::memcpy(&stake, static_cast<const uint8_t*>(data) + 5, 4);
		const char*     name = static_cast<const char*>(data) + 9;
		std::lock_guard lock(g_inviteLock);
		g_inviteView.until = ms != 0 ? NowMs() + ms : 0;
		g_inviteView.panel = static_cast<const uint8_t*>(data)[4] != 0;
		g_inviteView.stake = stake;
		g_inviteView.from  = std::string(name, strnlen(name, size - 9));
		++g_inviteView.seq;
	} else if (id == kMsgLobbyState && data != nullptr && size >= sizeof(LobbyStateMsg)) {
		LobbyStateMsg m {};
		std::memcpy(&m, data, sizeof(m));
		const auto* b   = static_cast<const char*>(data);
		size_t      pos = sizeof(m);
		std::vector<LobbyView::Member> members;
		for (int i = 0; i < m.count && pos + 3 <= size; ++i) {
			LobbyView::Member x { static_cast<uint8_t>(b[pos]), static_cast<uint8_t>(b[pos + 1]), std::string() };
			const size_t      len = std::min<size_t>(static_cast<uint8_t>(b[pos + 2]), size - pos - 3);
			x.name.assign(b + pos + 3, len);
			pos += 3 + len;
			members.push_back(std::move(x));
		}
		std::lock_guard lock(g_lobbyViewLock);
		g_lobbyView.open    = m.open;
		g_lobbyView.myTeam  = m.myTeam;
		g_lobbyView.leader  = m.leader != 0;
		g_lobbyView.myReady = m.myReady != 0;
		g_lobbyView.sizesOk = m.sizesOk != 0;
		g_lobbyView.mix       = m.mix != 0;
		g_lobbyView.revealing = m.revealing != 0;
		g_lobbyView.agreeAt = NowMs() + m.agreeWaitMs;
		g_lobbyView.rules   = m.rules;
		g_lobbyView.members = std::move(members);
		g_lobbyView.note    = pos < size ? std::string(b + pos, strnlen(b + pos, size - pos)) : std::string();
		++g_lobbyView.seq;
		if (m.open == 2) {
			g_closeInvPending = true; // as a 1 v 1 duel: the inventory closes before the arena
		}
	} else if (id == kMsgWagerState && data != nullptr && size >= sizeof(WagerStateMsg)) {
		WagerStateMsg m {};
		std::memcpy(&m, data, sizeof(m));
		const auto* b   = static_cast<const char*>(data);
		size_t      pos = sizeof(m);
		std::string names[2];
		for (std::string& n : names) {
			if (pos < size) {
				const size_t len = std::min<size_t>(static_cast<uint8_t>(b[pos]), size - pos - 1);
				n.assign(b + pos + 1, len);
				pos += 1 + len;
			}
		}
		std::lock_guard lock(g_wagerViewLock);
		g_wagerView.open       = m.open;
		g_wagerView.myGold     = m.myGold;
		g_wagerView.theirGold  = m.theirGold;
		g_wagerView.myReady    = m.myReady != 0;
		g_wagerView.theirReady = m.theirReady != 0;
		g_wagerView.points     = m.points;
		g_wagerView.canSet     = m.canSet != 0;
		g_wagerView.rules      = m.rules;
		g_wagerView.agreeAt    = NowMs() + m.agreeWaitMs;
		g_wagerView.me         = names[0];
		g_wagerView.them       = names[1];
		g_wagerView.note       = pos < size ? std::string(b + pos, strnlen(b + pos, size - pos)) : std::string();
		++g_wagerView.seq;
	} else if (id == kMsgNotice && data != nullptr && size >= 5) {
		uint32_t ms = 0;
		std::memcpy(&ms, data, 4);
		const char*     text = static_cast<const char*>(data) + 5;
		const uint8_t   kind = static_cast<const uint8_t*>(data)[4];
		std::lock_guard lock(g_noticeLock);
		if (kind == 2) {
			g_noticeBaseText  = ms != 0 ? std::string(text, strnlen(text, size - 5)) : std::string();
			g_noticeBaseUntil = ms != 0 ? NowMs() + ms : 0;
		} else {
			g_noticeText    = ms != 0 ? std::string(text, strnlen(text, size - 5)) : std::string();
			g_noticeUntil   = ms != 0 ? NowMs() + ms : 0;
			g_noticeSeconds = kind != 0;
		}
	} else if (id == kMsgDuelCountdown && data != nullptr && size >= 4) {
		uint32_t ms = 0;
		std::memcpy(&ms, data, 4);
		g_duelCdUntil = ms != 0 ? NowMs() + ms : 0;
		Log("arena: duel countdown %u ms.", ms);
	} else if (id == kMsgDuelLoadCheck && data != nullptr && size >= 4) {
		g_closeInvPending = true; // 1.8.2 (user): the rules / wager / trade windows leave the inventory open into the arena
		int32_t level = 0;
		std::memcpy(&level, data, 4);
		int32_t expect = 0;
		if (size >= 8) {
			std::memcpy(&expect, static_cast<const uint8_t*>(data) + 4, 4);
		}
		g_duelLoadUntil    = NowMs() + static_cast<uint64_t>(g_pvp.duelMoveTimeout * 1000.0);
		g_duelScreenUntil  = NowMs() + static_cast<uint64_t>(g_pvp.duelScreenWait * 1000.0);
		g_duelExpectScreen = expect != 0;
		g_duelLoadLevel    = level;
	} else if (id == kMsgSuddenDeath && data != nullptr && size >= 1) {
		g_suddenKind  = static_cast<const uint8_t*>(data)[0];
		g_suddenUntil = g_suddenKind.load() == kSuddenOut || g_suddenKind.load() == kSuddenOff ? 0 : NowMs() + kSuddenBannerMs;
		StartUiPoll();
		Log("arena: sudden death banner %d.", g_suddenKind.load());
	} else if (id == kMsgDuelResult && data != nullptr && size >= 5) {
		g_suddenKind = 0; // the result replaces the banner
		uint32_t ms = 0;
		std::memcpy(&ms, static_cast<const uint8_t*>(data) + 1, 4);
		g_duelResultKind  = static_cast<const uint8_t*>(data)[0];
		g_duelResultUntil = ms != 0 ? NowMs() + ms : 0;
		StartUiPoll();
		Log("arena: duel result %d, %u ms.", g_duelResultKind.load(), ms);
	} else if (id == kMsgCountdown && data != nullptr && size >= 4) {
		uint32_t ms = 0;
		std::memcpy(&ms, data, 4);
		g_countdownUntil = NowMs() + ms;
		Log("arena: countdown %u ms received from the host.", ms);
	} else if (id == kMsgRespawned) {
		g_countdownUntil = 0;
	} else if (id == kMsgReturn && data != nullptr && size >= 4) {
		uint32_t ms = 0;
		std::memcpy(&ms, data, 4);
		g_returnUntil = ms != 0 ? NowMs() + ms : 0;
		Log("arena: return-to-town countdown %u ms received from the host.", ms);
	} else if (id == kMsgScore && data != nullptr) {
		const char*     text = static_cast<const char*>(data);
		std::lock_guard lock(g_scoreLock);
		g_netScore.assign(text, strnlen(text, std::min<size_t>(size, kScoreReserve)));
	}
	StartUiPoll();
}
void __cdecl OnClientMessage(const D2RL::PluginContext*, D2RL::Network::ChannelHandle, uint16_t id, const void* data, uint32_t size, void*) noexcept {
	ClientHandle(id, data, size);
}
void __cdecl OnConnection(const D2RL::PluginContext*, const D2RL::Network::ConnectionEvent*, void*) noexcept {
	g_helloGuid = 0; // (re)connected: say hello again
	StartUiPoll();
}

// Client (UI thread): at the end of the falling animation the client puts its own player into DEAD (0x11) and calls
// FUN_1401E8EB0, which closes every interface state and opens state 9 = "You have died / Press ESC to continue"
// (YouDiedModal; its "continue" asks the server to resurrect a player the server doesn't see as dead -> stuck, P2
// 2026-10-31). Skipped while this client's arena countdown runs (+1.5 s grace).
auto LocalArenaDown() -> bool {
	const uint64_t now = NowMs();
	if (now < g_countdownUntil.load() + 1500 || (g_localDownGuid.load() != 0 && now < g_localDownUntil.load() + 1500)) {
		return true;
	}
	// Without a countdown message (network): the local player stands in an arena level, where PvP deaths are never real.
	void* me = g_localPlayer != nullptr ? g_localPlayer(g_localIndex()) : nullptr;
	return me != nullptr && IsPvpLevel(LevelOf(me));
}
// FUN_1400D0770 (every client UI update): if the local player is dead by the client's own test (mode DEATH or DEAD)
// and YouDiedModal isn't up, it dispatches a message that shows it directly - no interface state involved (P2 retest
// 2026-10-31: still stuck with the state-9 hooks). In an arena it is skipped, and the countdown starts here on the
// client itself (the remote player got none: no network countdown), length = respawn_seconds as on the host.
void __fastcall ArenaDeadCheck(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	if (LocalArenaDown()) {
		void* me = g_localPlayer(g_localIndex());
		if (me != nullptr) {
			const int32_t mode = g_getAnimMode(me);
			if ((mode == kPlrModeDeath || mode == kPlrModeDead) && g_countdownUntil.load() == 0) {
				g_countdownUntil = NowMs() + static_cast<uint64_t>(g_pvp.respawnSeconds * 1000.0 + 0.5);
				g_deathUiBlocked.fetch_add(1, std::memory_order_relaxed);
				Log("arena: local player %u is down in the arena: death screen blocked, countdown %.1f s.", Field<uint32_t>(me, 0x08), g_pvp.respawnSeconds);
				StartUiPoll();
			}
		}
		return;
	}
	if (const DeathUiFn orig = g_origDeadCheck.load(std::memory_order_acquire); orig != nullptr) {
		orig(a1, a2, a3, a4);
	}
}
// UI_OpenInterfaceState: state 9 (YouDiedModal) is also opened from FUN_1414FCEA0 (reached through a pointer, not
// through FUN_1401E8EB0) - G P2 retest 2026-10-31 still showed the screen with only the FUN_1401E8EB0 hook.
auto __fastcall ArenaOpenUi(uint32_t state, uint64_t a2, uint64_t a3, uint64_t a4) -> uint64_t {
	if (state == kUiStateYouDied && LocalArenaDown()) {
		g_deathUiBlocked.fetch_add(1, std::memory_order_relaxed);
		return 0;
	}
	const OpenUiFn orig = g_origOpenUi.load(std::memory_order_acquire);
	return orig != nullptr ? orig(state, a2, a3, a4) : 0;
}
void __fastcall ArenaDeathUi(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	if (LocalArenaDown()) {
		g_deathUiBlocked.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (const DeathUiFn orig = g_origDeathUi.load(std::memory_order_acquire); orig != nullptr) {
		orig(a1, a2, a3, a4);
	}
}

// ---- countdown panels (UI thread) ----
// 0.2.12: one fixed panel per second ("Respawning in 5" ... "2", then "Fight!" in the last second; user 2026-11-01):
// setText on the panel's text box never changed it (0.2.11 always showed the layout text). GameplayLeftSlot windows
// like smithing's shop - flags-None panels opened with result 0 but never drew (0.2.9 / 0.2.10). Text styles from the
// stock youdiedmodalhd.json (game folder data/global/ui/layouts): red $HugeFontSize + drop shadow 5/5, red
// $ExtraLargeFontSize + $DefaultDropShadow, zero-size centred rects.
constexpr int kCountdownPanels = 10; // seconds shown; a longer respawn shows "10" until 10 s are left
struct CountdownPanel {
	std::string                      localId, path, layout;
	D2RL::Panels::RegistrationHandle handle = D2RL::Panels::InvalidHandle;
};
std::array<CountdownPanel, kCountdownPanels + 1> g_cdPanels; // [k] = k seconds left (1 = "Fight!"); kept alive (resource bytes)
int  g_cdShown     = 0;     // UI thread: index of the open panel, 0 = none
bool g_uiProbeDone = false; // UI thread: one-time log of the interface states the first open changed
std::array<uint8_t, 0x40> g_uiBefore {};

auto CountdownLayout(int k) -> std::string {
	const std::string name = "arena/Countdown" + std::to_string(k);
	std::string       children;
	auto box = [](const char* id, int y, const std::string& text, const char* size, const char* shadow) {
		return std::string("{ \"type\": \"TextBoxWidget\", \"name\": \"") + id + "\", \"fields\": { \"rect\": { \"x\": 1980, \"y\": " + std::to_string(y)
		     + ", \"width\": 0, \"height\": 0 }, \"text\": \"" + text + "\", \"style\": { \"fontColor\": { \"r\": 200, \"g\": 20, \"b\": 0, \"a\": 255 }, "
		     + "\"pointSize\": \"" + size + "\", \"alignment\": { \"h\": \"center\", \"v\": \"center\" }, \"dropShadow\": " + shadow + " } } }";
	};
	if (k == 1) {
		children = box("Text", 450, "Fight!", "$HugeFontSize", "{ \"offset\": { \"x\": 5, \"y\": 5 } }");
	} else {
		children = box("Title", 350, "You are down", "$HugeFontSize", "{ \"offset\": { \"x\": 5, \"y\": 5 } }") + ",\n"
		         + box("Text", 550, g_pvp.respawnText + " " + std::to_string(k), "$ExtraLargeFontSize", "\"$DefaultDropShadow\"");
	}
	return "{\n\"type\": \"Panel\", \"name\": \"" + name + "\",\n\"fields\": { \"priority\": 5, \"rect\": \"$LeftPanelRect\", \"anchor\": \"$LeftPanelAnchor\" },\n"
	       "\"children\": [\n" + children + "\n]\n}";
}

// The stock inventory panel (layout root names in data/global/ui/layouts/playerinventory*hd.json). A GameplayLeftSlot
// panel opens it as its right half, and closing it closes the countdown too (0.2.13 test) -> hide it instead.
bool g_invHidden  = false; // UI thread: the plugin hid the inventory panel
bool g_invLogDone = false;
using FindTopLevelPanelFn = void*(__fastcall*)(const char* name);           // UI_FindTopLevelPanelByName (set in the HUD setup)
using FindChildByNameFn   = void*(__fastcall*)(void* parent, const char* name); // UI_Widget_FindChildByName
FindTopLevelPanelFn g_findTopLevelPanel = nullptr;
FindChildByNameFn   g_findChildByName   = nullptr;
auto SetInventoryVisible(const D2RL::PluginContext* ctx, bool visible) -> D2RL::Widgets::Result {
	if (g_widgets == nullptr || !D2RL::HasWidgetServiceField(g_widgets, D2RL::WidgetServiceRequiredSize) || g_widgets->setWidgetVisible == nullptr) {
		return D2RL::Widgets::Result::Unsupported;
	}
	// the side-panel frame (panelborderspanelhd.json "PanelBordersPanel", shown with any side panel; 0.2.14 test).
	// 2.4.3 (user: no left border around the windows): only its "RightAnchor" half (RightClickCatcher + RightBorderImage,
	// stock layout read 2026-11-08 from Data/global/ui/layouts); the left half frames our left-slot window.
	// 2.4.4 (user: the right frame came back): BordersPanel keeps its RightAnchor shown while the (hidden) inventory counts
	// as open, so the anchor's two children are hidden themselves; each lookup result is logged once.
	D2RL::Widgets::WidgetHandle borders = D2RL::Widgets::InvalidHandle;
	const auto bp = g_widgets->findPanel(ctx, "PanelBordersPanel", &borders);
	static bool s_logged = false;
	std::string found;
	for (const char* part : { "RightAnchor", "RightBorderImage", "RightClickCatcher" }) {
		D2RL::Widgets::WidgetHandle h = D2RL::Widgets::InvalidHandle;
		const auto r = bp == D2RL::Widgets::Result::Success ? g_widgets->findWidget(ctx, borders, part, &h) : bp;
		const auto v = r == D2RL::Widgets::Result::Success ? g_widgets->setWidgetVisible(ctx, h, visible) : r;
		found += std::string(" ") + part + " " + std::to_string(static_cast<int>(r)) + "/" + std::to_string(static_cast<int>(v));
	}
	if (!s_logged && g_pvp.diagnostics) {
		s_logged = true;
		Log("arena: side frame (find/visible results, 0 = ok): PanelBordersPanel %d,%s.", static_cast<int>(bp), found.c_str());
	}
	auto last = D2RL::Widgets::Result::NotFound;
	for (const char* name : { "PlayerInventoryExpansionLayout", "PlayerInventoryOriginalLayout" }) {
		D2RL::Widgets::WidgetHandle h = D2RL::Widgets::InvalidHandle;
		if (g_widgets->findPanel(ctx, name, &h) == D2RL::Widgets::Result::Success) {
			last = g_widgets->setWidgetVisible(ctx, h, visible);
			if (last == D2RL::Widgets::Result::Success) {
				// 2.4.25 (crash 2026-11-14 14:18, client, obelisk click): setWidgetVisible only clears the draw flag (+0x51);
				// UI_Widget_OnMessage routes input by +0x50 (notes/ui-widgets.md), so the hidden inventory still took clicks.
				// On the client its active unit was unset (id -1 / type 6): UI_HandleEquippedItemClick ->
				// UNITS_GetInventory(null) -> assert hUnit + access violation. The hidden inventory now takes no input;
				// it gets it back together with visibility (only done while interface state 1 is open).
				if (g_findTopLevelPanel != nullptr) {
					if (auto* p = static_cast<uint8_t*>(g_findTopLevelPanel(name)); p != nullptr) {
						p[0x50] = visible ? 1 : 0;
					}
				}
				return last;
			}
		}
	}
	return last;
}

// 2.4.20: only the right border pieces (RightBorderImage + RightClickCatcher), for the stake trade popup. True = both set.
bool g_borderRestorePending = false; // UI thread
auto SetRightBorderVisible(const D2RL::PluginContext* ctx, bool visible) -> bool {
	if (g_widgets == nullptr || !D2RL::HasWidgetServiceField(g_widgets, D2RL::WidgetServiceRequiredSize) || g_widgets->setWidgetVisible == nullptr) {
		return false;
	}
	D2RL::Widgets::WidgetHandle borders = D2RL::Widgets::InvalidHandle;
	if (g_widgets->findPanel(ctx, "PanelBordersPanel", &borders) != D2RL::Widgets::Result::Success) {
		return false;
	}
	bool ok = true;
	for (const char* part : { "RightBorderImage", "RightClickCatcher" }) {
		D2RL::Widgets::WidgetHandle h = D2RL::Widgets::InvalidHandle;
		ok = g_widgets->findWidget(ctx, borders, part, &h) == D2RL::Widgets::Result::Success
		     && g_widgets->setWidgetVisible(ctx, h, visible) == D2RL::Widgets::Result::Success && ok;
	}
	return ok;
}

// 2.4.1 (user 2026-11-08): no inventory / right frame next to the team lobby or the Duel Rules window. Both are left-slot
// panels and the game pairs them with the inventory; closing the inventory would close them too (0.2.13), so it is hidden
// as for the 0.2.14 countdown window, and shown again before either window closes (also before the stock trade window
// of a stake opens). UI thread.
bool g_invHiddenByWindow = false;
bool g_invRestorePending = false; // 2.4.3: shown again when the inventory is really open next (else a dead "ghost" draws)
void HideInvForWindow(const D2RL::PluginContext* ctx, bool recheck) {
	if (!g_invHiddenByWindow || recheck) {
		g_invHiddenByWindow = SetInventoryVisible(ctx, false) == D2RL::Widgets::Result::Success || g_invHiddenByWindow;
	}
}
// 2.4.3 (user: after the window closed a dead inventory picture stayed on screen - no clicks, no Esc, I opened the real
// one): making the hidden inventory visible while its interface state is already closed draws it without the panel being
// open. So: visible again at once only while the inventory state is open (our own close: the window and the inventory
// close together right after); otherwise when the inventory opens next (InvRestorePoll).
void ShowInvAfterWindow(const D2RL::PluginContext* ctx) {
	if (!g_invHiddenByWindow) {
		return;
	}
	g_invHiddenByWindow = false;
	// 2.4.15 (RF3 log 2026-11-14: one game - the client - still had interface state 1 open after the window closed, every
	// round; the host never): showing the inventory + RightAnchor here while the state was open left the empty right border
	// on the client next to the stake trade popup, even after the state was closed. Now every game takes the host's path:
	// the state is closed by the UI poll (g_closeInvPending) and the inventory is shown when it really opens next.
	// 2.4.21 (RF11 2026-11-14: with the deferral the challenger's inventory never came back - its trade request opens and
	// closes state 1 and the trade window never opens it again, so the trade window had a border but no inventory): back to
	// 2.4.13 (shown at once while state 1 is open, else when it opens next). The empty border this left next to the stake
	// popup is now hidden by SetRightBorderVisible (2.4.20) instead.
	if (g_getUi != nullptr && g_getUi(kUiStateInventory) != 0) {
		(void)SetInventoryVisible(ctx, true);
		g_invRestorePending = false;
	} else {
		g_invRestorePending = true;
	}
}
auto InvRestorePoll(const D2RL::PluginContext* ctx) -> bool { // true while waiting (keep polling)
	if (!g_invRestorePending || g_invHiddenByWindow) {
		return false;
	}
	if (g_closeInvPending.load()) {
		return true; // 2.4.15: the left-over state 1 is closed first (later in this poll), not shown
	}
	if (g_getUi != nullptr && g_getUi(kUiStateInventory) != 0) {
		(void)SetInventoryVisible(ctx, true);
		g_invRestorePending = false;
		return false;
	}
	return true;
}
// ---- countdown HUD (0.4.1, UI thread) ----
// skill-ext 0.5.2 found that a flags-None panel DOES draw when it is opened once the local player is ready and kept
// open for the whole game, with its text written straight into the TextBoxWidget's native string (the method of
// Lukaszpg's d2rl-buff-hud). Panels opened on demand (0.2.9/0.2.10) and WidgetService setText (0.2.11) never showed.
// One centred panel "arena/CountdownHud" with two text boxes (Title, Text), hidden while no countdown runs; no left
// slot, so no inventory / border hiding. countdown_style = "window" keeps the 0.2.15 left-slot windows.
// Native finders, bytes read 2026-11-01 in /d2rloader_full.dmp (same functions as skill-ext / buff-hud):
constexpr uint64_t kRvaFindTopLevelPanel = 0x846170; // UI_FindTopLevelPanelByName(const char* name)
constexpr uint64_t kRvaFindChildByName   = 0x856220; // UI_Widget_FindChildByName(void* parent, const char* name)
constexpr uint8_t  kFindTopLevelPanelBytes[] = { 0x48, 0x8B, 0xD1, 0x48, 0x8B, 0x0D, 0xF6, 0x9F, 0xBF, 0x02, 0x48, 0x85, 0xC9,
	0x0F, 0x85, 0xDD, 0x95, 0x05, 0x00, 0x33, 0xC0, 0xC3 };
constexpr uint8_t  kFindChildByNameBytes[]   = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83,
	0xEC, 0x20, 0x48, 0x8B, 0x59, 0x58, 0x48, 0x8B, 0xF2, 0x48, 0x8B, 0x41, 0x60, 0x48, 0x8D, 0x3C, 0xC3, 0x48, 0x3B };
// (FindTopLevelPanelFn / FindChildByNameFn and their pointers are declared above SetInventoryVisible, 2.4.25)
// TextBoxWidget (ctor FUN_1402cde10, notes/skill-ext.md 0.5.2): text string at +0x88 = {char* data, size +0x08,
// capacity|flags +0x10}, bit 63 = stored inside the widget; the draw function reads it every frame.
// Widget +0x50 = active, +0x51 = visible (notes/ui-widgets.md).
constexpr size_t   kHudTextOffset   = 0x88;
constexpr uint64_t kHudEmbeddedFlag = 0x8000000000000000ull;
constexpr uint64_t kHudCapacityMask = 0x7FFFFFFFFFFFFFFFull;
constexpr size_t   kHudReserve      = 126; // placeholder bytes (42 x U+200B) = longest text written
constexpr char     kHudPanelName[]  = "arena/CountdownHud";

struct HudText {
	const char*                 name;
	const char*                 panel   = kHudPanelName;                // top-level panel holding the box
	std::string                 reserve;                                // layout placeholder (reserves a heap buffer)
	char*                       buffer  = nullptr;                      // qualified native buffer we write to
	D2RL::Widgets::WidgetHandle handle  = D2RL::Widgets::InvalidHandle;
	bool                        visible = false;
};
std::array<HudText, 6> g_hudText { HudText { "Title" }, HudText { "Text" }, HudText { "WinTitle" }, HudText { "WinText" },
                                   HudText { "LoseTitle" }, HudText { "LoseText" } };
D2RL::Panels::RegistrationHandle g_hudPanel = D2RL::Panels::InvalidHandle;
std::string g_hudLayout;
bool        g_hudOn     = false; // HUD registered and in use
bool        g_hudOpen   = false; // UI thread
bool        g_hudInGame = false; // UI thread: between LocalPlayerReady and GameLeft
bool        g_hudLogged = false;
bool        g_hudFirst  = false;
bool        g_hudFailed = false;
int         g_hudShown  = 0;     // UI thread: seconds shown, 0 = hidden

bool HudWritable(const void* p, size_t n) {
	MEMORY_BASIC_INFORMATION mbi {};
	if (p == nullptr || VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
		return false;
	}
	if (reinterpret_cast<uintptr_t>(p) + n > reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
		return false;
	}
	const DWORD prot = mbi.Protect & 0xFF;
	return prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

void* HudWidget(const HudText& t) {
	void* panel = g_findTopLevelPanel(t.panel);
	return panel != nullptr ? g_findChildByName(panel, t.name) : nullptr;
}

// The widget's native text buffer if it is ours: heap storage big enough, holding our reserve or our last text.
char* HudBuffer(void* widget, const HudText& t) {
	if (widget == nullptr) {
		return nullptr;
	}
	const auto* str = static_cast<const uint8_t*>(widget) + kHudTextOffset;
	if (!HudWritable(str, 0x18)) {
		return nullptr;
	}
	char*    data = nullptr;
	uint64_t cap  = 0;
	std::memcpy(&data, str, sizeof(data));
	std::memcpy(&cap, str + 0x10, sizeof(cap));
	const size_t reserve = t.reserve.size();
	if ((cap & kHudEmbeddedFlag) != 0 || (cap & kHudCapacityMask) < reserve || !HudWritable(data, reserve + 1)) {
		return nullptr;
	}
	if (data == t.buffer) {
		return data;
	}
	return std::memcmp(data, t.reserve.data(), reserve + 1) == 0 ? data : nullptr;
}

bool HudWrite(HudText& t, const std::string& text) {
	void* widget = HudWidget(t);
	char* data   = HudBuffer(widget, t);
	if (data == nullptr) {
		return false;
	}
	const size_t n = std::min(text.size(), t.reserve.size());
	std::memcpy(data, text.data(), n);
	data[n] = '\0';
	const uint64_t size = n;
	std::memcpy(static_cast<uint8_t*>(widget) + kHudTextOffset + 0x08, &size, sizeof(size));
	t.buffer = data;
	return true;
}

// Reserve back before the close, so a reopened layout qualifies again (as buff-hud does).
void HudRestoreText(HudText& t) {
	if (t.buffer != nullptr) {
		void* widget = HudWidget(t);
		if (HudBuffer(widget, t) == t.buffer) {
			std::memcpy(t.buffer, t.reserve.data(), t.reserve.size() + 1);
			const uint64_t size = t.reserve.size();
			std::memcpy(static_cast<uint8_t*>(widget) + kHudTextOffset + 0x08, &size, sizeof(size));
		}
	}
	t.buffer = nullptr;
}
void HudRestore() {
	for (HudText& t : g_hudText) {
		HudRestoreText(t);
	}
}

void HudSetVisible(const D2RL::PluginContext* ctx, HudText& t, bool show) {
	if (t.handle != D2RL::Widgets::InvalidHandle && t.visible != show && g_widgets->setWidgetVisible(ctx, t.handle, show) == D2RL::Widgets::Result::Success) {
		t.visible = show;
	}
}

void HudLogState(const char* when) {
	const auto* panel = static_cast<const uint8_t*>(g_findTopLevelPanel(kHudPanelName));
	std::string parts;
	for (HudText& t : g_hudText) {
		const auto* w = panel != nullptr ? static_cast<const uint8_t*>(g_findChildByName(const_cast<uint8_t*>(panel), t.name)) : nullptr;
		char b[128] {};
		std::snprintf(b, sizeof(b), ", %s %s (visible %d, buffer %s)", t.name, w != nullptr ? "found" : "MISSING", w != nullptr ? w[0x51] : -1,
		              w == nullptr ? "-" : HudBuffer(const_cast<uint8_t*>(w), t) != nullptr ? "ok" : "NOT QUALIFIED");
		parts += b;
	}
	Log("arena: countdown HUD %s: native panel %s (active %d, visible %d)%s.", when, panel != nullptr ? "found" : "MISSING",
	    panel != nullptr ? panel[0x50] : -1, panel != nullptr ? panel[0x51] : -1, parts.c_str());
}

// Opens the HUD (kept open while in game); reopens it when the game closed it. UI thread.
bool HudEnsureOpen(const D2RL::PluginContext* ctx) {
	if (!g_hudOn) { // (also opens on the first countdown if LocalPlayerReady never came)
		return false;
	}
	if (g_hudOpen) {
		D2RL::Panels::PanelInfo info {};
		info.structSize = D2RL::Panels::PanelInfoSize;
		if (g_panels->getPanelInfo(ctx, g_hudPanel, &info) == D2RL::Panels::Result::Success && info.presentationState == D2RL::Panels::PresentationState::Closed) {
			g_hudOpen = false; // the game closed it: reopen (fresh layout, reserve text again)
			for (HudText& t : g_hudText) {
				t.buffer = nullptr;
			}
		}
	}
	if (g_hudOpen) {
		return true;
	}
	if (const auto r = g_panels->openPanel(ctx, g_hudPanel); r != D2RL::Panels::Result::Success) {
		if (!g_hudFailed) {
			g_hudFailed = true;
			Log("arena: countdown HUD: openPanel failed (result %d).", static_cast<int>(r));
		}
		return false;
	}
	g_hudOpen  = true;
	g_hudShown = 0;
	D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle;
	const bool found = g_widgets->findPanel(ctx, kHudPanelName, &panel) == D2RL::Widgets::Result::Success;
	if (found) {
		(void)g_widgets->setWidgetEnabled(ctx, panel, false); // display only: no mouse capture (buff-hud)
	}
	for (HudText& t : g_hudText) {
		t.visible = false;
		if (!found || g_widgets->findWidget(ctx, panel, t.name, &t.handle) != D2RL::Widgets::Result::Success) {
			t.handle = D2RL::Widgets::InvalidHandle;
			continue;
		}
		(void)g_widgets->setWidgetEnabled(ctx, t.handle, false);
		// hide explicitly: the layout's "visible": false is not applied (skill-ext log: visible 1 right after open)
		t.visible = g_widgets->setWidgetVisible(ctx, t.handle, false) != D2RL::Widgets::Result::Success;
	}
	if (!g_hudLogged && g_pvp.diagnostics) {
		g_hudLogged = true;
		HudLogState("opened");
	}
	return true;
}

void HudClose(const D2RL::PluginContext* ctx) {
	if (g_hudOpen) {
		HudRestore();
		(void)g_panels->closePanel(ctx, g_hudPanel);
	}
	g_hudOpen  = false;
	g_hudShown = 0;
	for (HudText& t : g_hudText) {
		t.handle  = D2RL::Widgets::InvalidHandle;
		t.visible = false;
		t.buffer  = nullptr;
	}
}

constexpr int kHudReturnBase = 100; // HudShow(kHudReturnBase + n): return-to-town countdown, n seconds left
constexpr int kHudDuelBase   = 200; // HudShow(kHudDuelBase + n): duel start countdown, n seconds left
constexpr int kHudWinBase    = 300; // HudShow(kHudWinBase + n): duel won, n seconds until town
constexpr int kHudLoseBase   = 400; // HudShow(kHudLoseBase + n): duel lost, n seconds until town
constexpr int kHudSuddenBase = 1000; // HudShow(kHudSuddenBase + SuddenKind): 1.9.3 sudden death banner, stays until the result
// k seconds left: k > 1 "You are down" + "<respawn_text> k"; 1 = "Fight!"; 0 = hidden.
void HudShow(const D2RL::PluginContext* ctx, int k) {
	if (!HudEnsureOpen(ctx) || k == g_hudShown) {
		return;
	}
	HudText& title = g_hudText[0];
	HudText& text  = g_hudText[1];
	bool     ok    = true;
	const bool sudden = k > kHudSuddenBase;
	const bool result = !sudden && k > kHudWinBase;
	for (size_t i = 2; i < g_hudText.size(); ++i) { // the result boxes only show a result
		if (!result || (k > kHudLoseBase) != (i >= 4)) {
			HudSetVisible(ctx, g_hudText[i], false);
		}
	}
	if (sudden) {
		const int   kind = k - kHudSuddenBase;
		const char* line = kind == 3 ? "No respawn in sudden death" : kind == 2 ? "No respawns - the last team standing wins" : "The next kill wins";
		ok = HudWrite(title, kind == 3 ? "You were downed" : "Sudden death") && HudWrite(text, line);
		HudSetVisible(ctx, title, ok);
		HudSetVisible(ctx, text, ok);
	} else if (result) {
		HudSetVisible(ctx, title, false);
		HudSetVisible(ctx, text, false);
		const bool lost = k > kHudLoseBase;
		HudText&   t1   = g_hudText[lost ? 4 : 2];
		HudText&   t2   = g_hudText[lost ? 5 : 3];
		ok = HudWrite(t1, lost ? "You lost the duel" : "You won the duel") && HudWrite(t2, "Back to town in " + std::to_string(k - (lost ? kHudLoseBase : kHudWinBase)));
		HudSetVisible(ctx, t1, ok);
		HudSetVisible(ctx, t2, ok);
	} else if (k <= 0) {
		HudSetVisible(ctx, title, false);
		HudSetVisible(ctx, text, false);
	} else if (k > kHudDuelBase) {
		ok = HudWrite(title, "Duel") && HudWrite(text, "Starts in " + std::to_string(k - kHudDuelBase));
		HudSetVisible(ctx, title, ok);
		HudSetVisible(ctx, text, ok);
	} else if (k > kHudReturnBase) {
		ok = HudWrite(title, g_pvp.returnTitle) && HudWrite(text, g_pvp.returnText + " " + std::to_string(k - kHudReturnBase));
		HudSetVisible(ctx, title, ok);
		HudSetVisible(ctx, text, ok);
	} else if (k == 1) {
		ok = HudWrite(title, "Fight!");
		HudSetVisible(ctx, text, false);
		HudSetVisible(ctx, title, ok);
	} else {
		ok = HudWrite(title, "You are down") && HudWrite(text, g_pvp.respawnText + " " + std::to_string(k));
		HudSetVisible(ctx, title, ok);
		HudSetVisible(ctx, text, ok);
	}
	if (!ok) {
		if (!g_hudFailed) {
			g_hudFailed = true;
			HudLogState("write failed");
		}
		return;
	}
	if (k > 0 && !g_hudFirst) {
		g_hudFirst = true;
		if (g_pvp.diagnostics) {
			HudLogState("first text");
		}
	}
	g_hudShown = k;
}

// Same text styles as the windows (stock youdiedmodalhd.json); centre anchor, rects relative to the screen centre.
auto HudLayout() -> std::string {
	constexpr const char* kRed  = "{ \"r\": 200, \"g\": 20, \"b\": 0, \"a\": 255 }";
	constexpr const char* kGold = "{ \"r\": 199, \"g\": 179, \"b\": 119, \"a\": 255 }"; // stock $FontColorGoldYellow
	// size: a layout token ("$HugeFontSize" = 66) or a plain number; face "" = the default font, "Exocet" = the title font
	auto box = [](const char* id, int y, const std::string& text, const std::string& size, const char* shadow, const char* color, const char* face) {
		const std::string pt = size.front() == '$' ? "\"" + size + "\"" : size;
		return std::string("{ \"type\": \"TextBoxWidget\", \"name\": \"") + id + "\", \"fields\": { \"visible\": false, \"rect\": { \"x\": -1500, \"y\": "
		     + std::to_string(y) + ", \"width\": 3000, \"height\": 200 }, \"text\": \"" + text + "\", \"style\": { "
		     + (face[0] != 0 ? std::string("\"fontFace\": \"") + face + "\", " : std::string()) + "\"fontColor\": " + color + ", \"pointSize\": " + pt
		     + ", \"alignment\": { \"h\": \"center\", \"v\": \"center\" }, \"dropShadow\": " + shadow + " } } }";
	};
	// 0.9.1: solid black shadow (D2WinFontDropShadow.shadowColor, reflection data) so gold and red stand out on the sand
	const char* bigShadow = "{ \"shadowColor\": { \"r\": 0, \"g\": 0, \"b\": 0, \"a\": 255 }, \"offset\": { \"x\": 6, \"y\": 6 } }";
	return std::string("{\n\"type\": \"Panel\", \"name\": \"") + kHudPanelName
	     + "\",\n\"fields\": { \"anchor\": { \"x\": 0.5, \"y\": 0.5 }, \"priority\": 101 },\n\"children\": [\n"
	     + box("Title", -500, g_hudText[0].reserve, "$HugeFontSize", bigShadow, kRed, "") + ",\n"
	     + box("Text", -300, g_hudText[1].reserve, "$ExtraLargeFontSize", "\"$DefaultDropShadow\"", kRed, "") + ",\n"
	     // duel result (0.8.5): bigger than the respawn text; winner in gold Exocet, loser in the respawn red
	     + box("WinTitle", -520, g_hudText[2].reserve, "84", bigShadow, kGold, "Exocet") + ",\n"
	     + box("WinText", -300, g_hudText[3].reserve, "$XExtraLargeFontSize", bigShadow, kGold, "Exocet") + ",\n"
	     + box("LoseTitle", -520, g_hudText[4].reserve, "84", bigShadow, kRed, "") + ",\n"
	     + box("LoseText", -300, g_hudText[5].reserve, "$XExtraLargeFontSize", bigShadow, kRed, "") + "\n]\n}";
}

// Registers the HUD layout + panel at load; false = use the left-slot windows.
// The two native finders the HUD text writes need (countdown HUD and scoreboard); false = this build doesn't match.
bool HudFinders(const D2RL::PluginContext* ctx) {
	if (g_findTopLevelPanel != nullptr) {
		return true;
	}
	if (!ctx->CheckExpectedBytes(kRvaFindTopLevelPanel, kFindTopLevelPanelBytes, sizeof(kFindTopLevelPanelBytes))
	    || !ctx->CheckExpectedBytes(kRvaFindChildByName, kFindChildByNameBytes, sizeof(kFindChildByNameBytes))) {
		return false;
	}
	g_findTopLevelPanel = reinterpret_cast<FindTopLevelPanelFn>(ctx->exeBase + kRvaFindTopLevelPanel);
	g_findChildByName   = reinterpret_cast<FindChildByNameFn>(ctx->exeBase + kRvaFindChildByName);
	return true;
}

bool HudRegister(const D2RL::PluginContext* ctx, const D2RL::ResourceService* res) {
	if (!HudFinders(ctx)) {
		ctx->LogWarn("arena: countdown HUD: native UI finders don't match this game build; using the countdown windows.");
		return false;
	}
	// zero-width spaces (U+200B): the placeholder draws nothing even while a box is visible (skill-ext 2026-11-01:
	// underscores drew a line across the screen, and the layout's "visible": false is not applied)
	for (HudText& t : g_hudText) {
		t.reserve.clear();
		while (t.reserve.size() < kHudReserve) {
			t.reserve += "\xE2\x80\x8B";
		}
	}
	g_hudLayout = HudLayout();
	const D2RL::Resources::ResourceRegistration layout {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.path       = "data/global/ui/layouts/arena/CountdownHudhd.json",
		.bytes      = g_hudLayout.c_str(),
		.byteCount  = static_cast<uint32_t>(g_hudLayout.size()),
	};
	D2RL::Resources::RegistrationHandle lh = D2RL::Resources::InvalidHandle;
	const D2RL::Panels::PanelRegistration panel { .structSize = D2RL::Panels::PanelRegistrationSize, .flags = D2RL::Panels::PanelFlags::None, .localId = "CountdownHud" };
	if (res->registerResource(ctx, &layout, &lh) != D2RL::Resources::Result::Success || g_panels->registerPanel(ctx, &panel, &g_hudPanel) != D2RL::Panels::Result::Success) {
		ctx->LogWarn("arena: countdown HUD could not be registered; using the countdown windows.");
		g_hudPanel = D2RL::Panels::InvalidHandle;
		return false;
	}
	g_hudOn = true;
	return true;
}

void ShowCountdown(const D2RL::PluginContext* ctx, int k) {
	if (g_hudOn) {
		HudShow(ctx, k);
		return;
	}
	if (g_panels == nullptr || k == g_cdShown) {
		return;
	}
	if (g_cdShown != 0) {
		if (g_invHidden) {
			(void)SetInventoryVisible(ctx, true); // before the close, so the next real inventory open shows it
			g_invHidden = false;
		}
		(void)g_panels->closePanel(ctx, g_cdPanels[g_cdShown].handle);
	}
	g_cdShown = 0;
	if (k > 0 && g_cdPanels[k].handle != D2RL::Panels::InvalidHandle) {
		const bool probe = !g_uiProbeDone && g_getUi != nullptr && g_pvp.diagnostics;
		if (probe) {
			for (int i = 0; i < static_cast<int>(g_uiBefore.size()); ++i) {
				g_uiBefore[i] = g_getUi(i);
			}
		}
		const bool invWasOpen = g_getUi != nullptr && g_getUi(kUiStateInventory) != 0;
		const auto r          = g_panels->openPanel(ctx, g_cdPanels[k].handle);
		g_cdShown             = r == D2RL::Panels::Result::Success ? k : 0;
		// GameplayLeftSlot opens the player inventory as its right half (panels.h); the countdown doesn't need it.
		if (g_cdShown != 0 && !invWasOpen) {
			const auto vr = SetInventoryVisible(ctx, false);
			g_invHidden   = vr == D2RL::Widgets::Result::Success;
			if (!g_invLogDone && g_pvp.diagnostics) {
				g_invLogDone = true;
				Log("arena: hide inventory panel under the countdown: result %d.", static_cast<int>(vr));
			}
		}
		if (probe) { // which interface states the left-slot window opened (the inventory id, to close it later)
			std::string changed;
			for (int i = 0; i < static_cast<int>(g_uiBefore.size()); ++i) {
				if (const uint8_t now = g_getUi(i); now != g_uiBefore[i]) {
					char b[24] {};
					std::snprintf(b, sizeof(b), " %d:%u->%u", i, g_uiBefore[i], now);
					changed += b;
				}
			}
			g_uiProbeDone = true;
			Log("arena: countdown panel %d open: result %d; interface states changed:%s", k, static_cast<int>(r), changed.empty() ? " none" : changed.c_str());
		}
	}
}

// ---- scoreboard (0.5.0, UI thread) ----
// User 2026-11-01: top centre above the monster hover bar, only while the player stands in an arena level, one line
// with the players of that arena (best score first), a Controls hotkey / `arena scoreboard` hides it, no drift.
// Same technique as the countdown HUD: own flags-None panel kept open all game, text written into the native string.
// The host decides what each player sees (kMsgScore, "" = not in an arena); the hosting player reads g_hostScore.
// 0.5.2-1.9.8: 1 v 1 used the DuelScore art (make_duel_sprites.py); replaced by the team art in 2.0.0.
// 2.0.0 (user 2026-11-08): every 2-sided fight (1 v 1 and team duels) uses the team art from the user's pieces
// (arena/art/Centerpiece.png + Nameplate.png -> PANEL\Arena\TeamCentre / TeamPlate, arena/tools/make_team_sprites.py):
// the medallion in the middle with the two team totals, one nameplate per player (name + own score), up to 4 per side,
// fanned around the ring. Team One always on the left, Team Two on the right. The widget service can't move widgets
// (installed D2RCore has no setWidgetRect), so the layout holds 7 plate slots per side at dy = (slot - 3) x kTeamStep:
// 1 player -> slot 3; 2 -> 2, 4; 3 -> 1, 3, 5; 4 -> 0, 2, 4, 6.
// Message (kTeamMark): mark total0 mark total1, then per player mark side('0'/'1')name mark score.
constexpr char   kScorePanelName[] = "arena/ScoreHud";
// make_team_sprites.py --centre 0.2 --plate 0.22 --tuck 42 --gap 2 (2026-11-08)
constexpr int    kTeamCentreW = 248, kTeamCentreH = 248, kTeamPlateW = 475, kTeamPlateH = 58;
constexpr double kTeamCentreCx = 123.8, kTeamCentreCy = 125.2, kTeamRing = 100.0, kTeamDisc = 82.4;
constexpr double kTeamField[4] = { 59.4, 10.6, 418.9, 46.9 }; // plate's dark field in the plate sprite
constexpr double kTeamTuck = 42, kTeamStep = 60;
constexpr int    kTeamSlots = 7, kTeamPlates = 2 * kTeamSlots; // P0-P6 left, P7-P13 right
enum ScoreBox { kBoxLine, kBoxScore, kBoxNotice, kBoxPlateName, kBoxPlateScore = kBoxPlateName + kTeamPlates, kBoxSoloName = kBoxPlateScore + kTeamPlates, kBoxSoloScore, kBoxRoyal1, kBoxRoyal2, kBoxScoreL, kBoxScoreR,
                kBoxScoreLRed, kBoxScoreRRed, kBoxSoloScoreRed, kBoxPlateScoreRed, kScoreBoxes = kBoxPlateScoreRed + kTeamPlates };
// 2.3.1 (user, screenshot 20:10 "6 : -2"): a negative score is shown without the minus, in red. Text boxes don't read colour
// codes, so every score box has a red twin at the same rect; WriteSigned fills one and empties the other.
// 2.2.0 (user): the two medallion numbers ~8 px closer to the colon: own boxes ending kScoreGap units from the centre
// (screenshot 18:57: plates 475 units = ~172 px, so 8 px = ~22 units; the numbers sat ~52 units out).
constexpr double kScoreGap = 30;
constexpr double kTeamNameLift = 3; // 2.0.3 (user screenshot 18:57): names sat low -> 6; 2.1.0 (user): half of that back down
const char* const kPlateNameIds[kTeamPlates]  = { "PN0", "PN1", "PN2", "PN3", "PN4", "PN5", "PN6", "PN7", "PN8", "PN9", "PN10", "PN11", "PN12", "PN13" };
const char* const kPlateScoreIds[kTeamPlates] = { "PS0", "PS1", "PS2", "PS3", "PS4", "PS5", "PS6", "PS7", "PS8", "PS9", "PS10", "PS11", "PS12", "PS13" };
const char* const kPlateRedIds[kTeamPlates]   = { "PR0", "PR1", "PR2", "PR3", "PR4", "PR5", "PR6", "PR7", "PR8", "PR9", "PR10", "PR11", "PR12", "PR13" };
// 2.0.3: "Solo" = one plate in the middle (no medallion) for a player alone in an arena (message side '2')
const char* const kScoreImageIds[2 + kTeamPlates] = { "Centre", "P0", "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "P9", "P10", "P11", "P12", "P13", "Solo" };
constexpr int     kScoreImages = 2 + kTeamPlates;
constexpr int     kImgSolo     = kScoreImages - 1;
auto MakeScoreBoxes() -> std::array<HudText, kScoreBoxes> {
	std::array<HudText, kScoreBoxes> b {};
	b[kBoxLine]   = HudText { "Line", kScorePanelName };
	b[kBoxScore]  = HudText { "Score", kScorePanelName };
	b[kBoxNotice] = HudText { "Notice", kScorePanelName };
	for (int i = 0; i < kTeamPlates; ++i) {
		b[kBoxPlateName + i]  = HudText { kPlateNameIds[i], kScorePanelName };
		b[kBoxPlateScore + i] = HudText { kPlateScoreIds[i], kScorePanelName };
	}
	b[kBoxSoloName]  = HudText { "SN", kScorePanelName };
	b[kBoxSoloScore] = HudText { "SS", kScorePanelName };
	b[kBoxRoyal1]    = HudText { "BR1", kScorePanelName };
	b[kBoxRoyal2]    = HudText { "BR2", kScorePanelName };
	b[kBoxScoreL]    = HudText { "ScoreL", kScorePanelName };
	b[kBoxScoreR]    = HudText { "ScoreR", kScorePanelName };
	b[kBoxScoreLRed]    = HudText { "ScoreLR", kScorePanelName };
	b[kBoxScoreRRed]    = HudText { "ScoreRR", kScorePanelName };
	b[kBoxSoloScoreRed] = HudText { "SSR", kScorePanelName };
	for (int i = 0; i < kTeamPlates; ++i) {
		b[kBoxPlateScoreRed + i] = HudText { kPlateRedIds[i], kScorePanelName };
	}
	return b;
}
std::array<HudText, kScoreBoxes> g_scoreBoxes = MakeScoreBoxes();
auto ScoreBoxReserve(int i) -> size_t { // bytes (multiples of 3)
	return i == kBoxLine ? kScoreReserve : i == kBoxScore || i == kBoxRoyal1 || i == kBoxRoyal2 || i == kBoxScoreL || i == kBoxScoreR || i == kBoxScoreLRed || i == kBoxScoreRRed ? 36 : i == kBoxNotice ? 225 : i < kBoxPlateScore || i == kBoxSoloName ? 48 : 18;
}
std::string g_noticeShown; // UI thread
std::array<D2RL::Widgets::WidgetHandle, kScoreImages> g_scoreImg {};     // Centre, P0-P13
std::array<bool, kScoreImages>                       g_scoreImgShow {}; // what we last set
uint64_t                         g_scoreOpenedAt = 0;     // 0.9.2: NowMs of the last (re)open
bool                             g_scoreArtFixLogged = false;
D2RL::Panels::RegistrationHandle g_scorePanel   = D2RL::Panels::InvalidHandle;
std::string                      g_scoreLayout;
bool                             g_scoreOn     = false;
bool                             g_scoreOpen   = false; // UI thread
bool                             g_scoreFailed = false;
bool                             g_scoreLogged = false;
bool                             g_scoreFirst  = false;
bool                             g_duelFirst   = false;
std::string                      g_scoreShown; // UI thread: message on screen, "" = hidden

void ScoreLogState(const char* when) {
	const auto* panel = static_cast<const uint8_t*>(g_findTopLevelPanel(kScorePanelName));
	std::string parts;
	for (int i : { static_cast<int>(kBoxLine), static_cast<int>(kBoxScore), static_cast<int>(kBoxNotice), static_cast<int>(kBoxPlateName), static_cast<int>(kBoxPlateScore) }) {
		HudText&    t = g_scoreBoxes[i];
		const auto* w = panel != nullptr ? static_cast<const uint8_t*>(g_findChildByName(const_cast<uint8_t*>(panel), t.name)) : nullptr;
		char b[128] {};
		std::snprintf(b, sizeof(b), ", %s %s (visible %d, buffer %s)", t.name, w != nullptr ? "found" : "MISSING", w != nullptr ? w[0x51] : -1,
		              w == nullptr ? "-" : HudBuffer(const_cast<uint8_t*>(w), t) != nullptr ? "ok" : "NOT QUALIFIED");
		parts += b;
	}
	int images = 0;
	for (const char* id : kScoreImageIds) {
		images += panel != nullptr && g_findChildByName(const_cast<uint8_t*>(panel), id) != nullptr ? 1 : 0;
	}
	Log("arena: scoreboard %s: native panel %s (active %d, visible %d)%s, images %d of %d found.", when, panel != nullptr ? "found" : "MISSING",
	    panel != nullptr ? panel[0x50] : -1, panel != nullptr ? panel[0x51] : -1, parts.c_str(), images, kScoreImages);
}

void ScoreImgVisible(const D2RL::PluginContext* ctx, int i, bool show) {
	if (g_scoreImg[i] != D2RL::Widgets::InvalidHandle && g_scoreImgShow[i] != show && g_widgets->setWidgetVisible(ctx, g_scoreImg[i], show) == D2RL::Widgets::Result::Success) {
		g_scoreImgShow[i] = show;
	}
}
void ScoreArtVisible(const D2RL::PluginContext* ctx, bool show) { // false = all art off; true = the medallion only
	for (int i = 0; i < kScoreImages; ++i) {
		ScoreImgVisible(ctx, i, show && i == 0);
	}
}

// 0.9.2: the art stayed up after a duel (frame only, until save and exit; the hotkey didn't clear it). The layout's
// "visible": false is not applied, so a panel the game reopened can show the art while g_scoreImgShow says hidden.
// Compare with the native widget (+0x51) and set it again. Returns true while an image was wrong (keep polling).
bool ScoreArtEnforce(const D2RL::PluginContext* ctx) {
	if (g_findTopLevelPanel == nullptr || g_findChildByName == nullptr) {
		return false;
	}
	auto* panel = static_cast<uint8_t*>(g_findTopLevelPanel(kScorePanelName));
	if (panel == nullptr) {
		return false;
	}
	bool wrong = false;
	for (int i = 0; i < kScoreImages; ++i) {
		const auto* art = g_scoreImg[i] != D2RL::Widgets::InvalidHandle ? static_cast<const uint8_t*>(g_findChildByName(panel, kScoreImageIds[i])) : nullptr;
		if (art == nullptr || (art[0x51] != 0) == g_scoreImgShow[i]) {
			continue;
		}
		if (!g_scoreArtFixLogged && g_pvp.diagnostics) {
			g_scoreArtFixLogged = true;
			Log("arena: scoreboard image %s was %s while it should be %s: set again.", kScoreImageIds[i], art[0x51] != 0 ? "shown" : "hidden", g_scoreImgShow[i] ? "shown" : "hidden");
		}
		(void)g_widgets->setWidgetVisible(ctx, g_scoreImg[i], g_scoreImgShow[i]);
		wrong = true;
	}
	static int tries = 0; // don't poll forever if the service can't change it
	if (wrong && ++tries == 200) {
		Log("arena: scoreboard art: visibility won't change.");
	}
	return wrong && tries < 200;
}

// Opens the scoreboard panel (kept open while in game); reopens it when the game closed it. UI thread.
bool ScoreEnsureOpen(const D2RL::PluginContext* ctx) {
	if (!g_scoreOn) {
		return false;
	}
	if (g_scoreOpen) {
		D2RL::Panels::PanelInfo info {};
		info.structSize = D2RL::Panels::PanelInfoSize;
		if (g_panels->getPanelInfo(ctx, g_scorePanel, &info) == D2RL::Panels::Result::Success && info.presentationState == D2RL::Panels::PresentationState::Closed) {
			g_scoreOpen = false; // the game closed it: reopen (fresh layout)
			for (HudText& t : g_scoreBoxes) {
				t.buffer = nullptr;
			}
		}
	}
	if (g_scoreOpen) {
		return true;
	}
	if (const auto r = g_panels->openPanel(ctx, g_scorePanel); r != D2RL::Panels::Result::Success) {
		if (!g_scoreFailed) {
			g_scoreFailed = true;
			Log("arena: scoreboard: openPanel failed (result %d).", static_cast<int>(r));
		}
		return false;
	}
	g_scoreOpen = true;
	g_scoreOpenedAt = NowMs();
	g_scoreShown.clear();
	D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle;
	const bool found = g_widgets->findPanel(ctx, kScorePanelName, &panel) == D2RL::Widgets::Result::Success;
	if (found) {
		(void)g_widgets->setWidgetEnabled(ctx, panel, false); // display only: no mouse capture
	}
	for (HudText& t : g_scoreBoxes) {
		t.visible = false;
		if (!found || g_widgets->findWidget(ctx, panel, t.name, &t.handle) != D2RL::Widgets::Result::Success) {
			t.handle = D2RL::Widgets::InvalidHandle;
			continue;
		}
		(void)g_widgets->setWidgetEnabled(ctx, t.handle, false);
		// hide explicitly: the layout's "visible": false is not applied (countdown HUD, 0.4.1)
		t.visible = g_widgets->setWidgetVisible(ctx, t.handle, false) != D2RL::Widgets::Result::Success;
	}
	for (int i = 0; i < kScoreImages; ++i) {
		g_scoreImgShow[i] = false;
		if (!found || g_widgets->findWidget(ctx, panel, kScoreImageIds[i], &g_scoreImg[i]) != D2RL::Widgets::Result::Success) {
			g_scoreImg[i] = D2RL::Widgets::InvalidHandle;
		} else {
			(void)g_widgets->setWidgetEnabled(ctx, g_scoreImg[i], false);
			g_scoreImgShow[i] = g_widgets->setWidgetVisible(ctx, g_scoreImg[i], false) != D2RL::Widgets::Result::Success;
		}
	}
	if (!g_scoreLogged && g_pvp.diagnostics) {
		g_scoreLogged = true;
		ScoreLogState("opened");
	}
	return true;
}

void ScoreClose(const D2RL::PluginContext* ctx) {
	if (g_scoreOpen) {
		for (HudText& t : g_scoreBoxes) {
			HudRestoreText(t);
		}
		(void)g_panels->closePanel(ctx, g_scorePanel);
	}
	g_scoreOpen = false;
	g_scoreShown.clear();
	g_noticeShown.clear(); // the box is gone: a stale value would reopen the panel just to hide it
	for (HudText& t : g_scoreBoxes) {
		t.handle  = D2RL::Widgets::InvalidHandle;
		t.visible = false;
		t.buffer  = nullptr;
	}
	g_scoreImg.fill(D2RL::Widgets::InvalidHandle);
	g_scoreImgShow.fill(false);
}

// Fields of a team message (text without the leading mark): total0, total1, then side+name / score pairs.
auto SplitTeam(const std::string& text) -> std::vector<std::string> {
	std::vector<std::string> f;
	size_t                   pos = 1;
	while (pos <= text.size()) {
		const size_t next = text.find(kTeamMark, pos);
		f.push_back(text.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
		if (next == std::string::npos) {
			break;
		}
		pos = next + 1;
	}
	return f;
}
// Plate slots used by n players on one side (see the comment above kScorePanelName).
auto TeamSlots(size_t n) -> std::vector<int> {
	switch (n) {
	case 1: return { 3 };
	case 2: return { 2, 4 };
	case 3: return { 1, 3, 5 };
	case 4: return { 0, 2, 4, 6 };
	default: return {};
	}
}

// 2.3.1: "-3" -> "3" in the red box, "" in the normal one; otherwise the reverse.
auto WriteSigned(int normal, int red, const std::string& v) -> bool {
	const bool neg = !v.empty() && v[0] == '-';
	return HudWrite(g_scoreBoxes[normal], neg ? std::string() : v) && HudWrite(g_scoreBoxes[red], neg ? v.substr(1) : std::string());
}
// Shows what the host gave this player (line, team art, or nothing). UI thread (UiPoll).
// Returns true while UiPoll should keep polling (art just reopened or corrected).
bool ScoreUpdate(const D2RL::PluginContext* ctx, uint32_t guid) {
	if (!g_scoreOn || guid == 0 || !g_pvp.scoreboard) {
		return false;
	}
	std::string text;
	{
		std::lock_guard lock(g_scoreLock);
		const auto it = g_hostScore.find(guid);
		text          = it != g_hostScore.end() ? it->second : g_netScore;
	}
	if (g_scoreHidden.load()) {
		text.clear();
	}
	if (text.empty() && !g_scoreOpen) {
		return false;
	}
	if (!ScoreEnsureOpen(ctx)) {
		return false;
	}
	const bool watch = ScoreArtEnforce(ctx) || NowMs() - g_scoreOpenedAt < 3000;
	if (text == g_scoreShown) {
		return watch;
	}
	const auto hideAll = [&] {
		for (int i = 0; i < kScoreBoxes; ++i) {
			if (i != kBoxNotice) {
				HudSetVisible(ctx, g_scoreBoxes[i], false);
			}
		}
		ScoreArtVisible(ctx, false);
	};
	if (text.empty()) {
		hideAll();
		g_scoreShown.clear();
		return true;
	}
	bool                           ok   = true;
	const std::vector<std::string> team = text.front() == kTeamMark ? SplitTeam(text) : std::vector<std::string>();
	const bool                     art  = team.size() >= 4 && team.size() % 2 == 0;
	if (art) {
		std::vector<std::pair<std::string, std::string>> side[3]; // name, score; side 2 = alone (2.0.3)
		for (size_t i = 2; i + 1 < team.size(); i += 2) {
			if (!team[i].empty() && team[i][0] >= '0' && team[i][0] <= '2' && side[team[i][0] - '0'].size() < 4) {
				side[team[i][0] - '0'].emplace_back(team[i].substr(1), team[i + 1]);
			}
		}
		const bool solo   = !side[2].empty() && side[0].empty() && side[1].empty();
		const bool royale = team[0] == "*"; // 2.1.0: free-for-all of 3+ (no teams): "Battle Royale" in the medallion
		std::array<bool, kTeamPlates> used {};
		ok = solo     ? HudWrite(g_scoreBoxes[kBoxSoloName], side[2][0].first) && WriteSigned(kBoxSoloScore, kBoxSoloScoreRed, side[2][0].second)
		   : royale ? HudWrite(g_scoreBoxes[kBoxRoyal1], "Battle") && HudWrite(g_scoreBoxes[kBoxRoyal2], "Royale")
		            : HudWrite(g_scoreBoxes[kBoxScore], ":") && WriteSigned(kBoxScoreL, kBoxScoreLRed, team[0]) && WriteSigned(kBoxScoreR, kBoxScoreRRed, team[1]);
		for (int s = 0; s < 2 && ok; ++s) {
			const std::vector<int> slots = TeamSlots(side[s].size());
			for (size_t k = 0; k < slots.size() && ok; ++k) {
				const int p = s * kTeamSlots + slots[k];
				used[p]     = true;
				ok          = HudWrite(g_scoreBoxes[kBoxPlateName + p], side[s][k].first) && WriteSigned(kBoxPlateScore + p, kBoxPlateScoreRed + p, side[s][k].second);
			}
		}
		HudSetVisible(ctx, g_scoreBoxes[kBoxLine], false);
		for (int p = 0; p < kTeamPlates; ++p) {
			ScoreImgVisible(ctx, 1 + p, ok && used[p]);
			HudSetVisible(ctx, g_scoreBoxes[kBoxPlateName + p], ok && used[p]);
			HudSetVisible(ctx, g_scoreBoxes[kBoxPlateScore + p], ok && used[p]);
			HudSetVisible(ctx, g_scoreBoxes[kBoxPlateScoreRed + p], ok && used[p]);
		}
		ScoreImgVisible(ctx, 0, ok && !solo);
		HudSetVisible(ctx, g_scoreBoxes[kBoxScore], ok && !solo && !royale);
		HudSetVisible(ctx, g_scoreBoxes[kBoxScoreL], ok && !solo && !royale);
		HudSetVisible(ctx, g_scoreBoxes[kBoxScoreR], ok && !solo && !royale);
		HudSetVisible(ctx, g_scoreBoxes[kBoxScoreLRed], ok && !solo && !royale);
		HudSetVisible(ctx, g_scoreBoxes[kBoxScoreRRed], ok && !solo && !royale);
		HudSetVisible(ctx, g_scoreBoxes[kBoxRoyal1], ok && royale && !solo);
		HudSetVisible(ctx, g_scoreBoxes[kBoxRoyal2], ok && royale && !solo);
		ScoreImgVisible(ctx, kImgSolo, ok && solo);
		HudSetVisible(ctx, g_scoreBoxes[kBoxSoloName], ok && solo);
		HudSetVisible(ctx, g_scoreBoxes[kBoxSoloScore], ok && solo);
		HudSetVisible(ctx, g_scoreBoxes[kBoxSoloScoreRed], ok && solo);
	} else {
		ok = HudWrite(g_scoreBoxes[kBoxLine], text);
		hideAll();
		HudSetVisible(ctx, g_scoreBoxes[kBoxLine], ok);
	}
	if (!ok) {
		if (!g_scoreFailed) {
			g_scoreFailed = true;
			ScoreLogState("write failed");
		}
		hideAll();
		return watch;
	}
	g_scoreShown = text;
	if (art ? !g_duelFirst : !g_scoreFirst) {
		(art ? g_duelFirst : g_scoreFirst) = true;
		if (g_pvp.diagnostics) {
			ScoreLogState(art ? "first team art" : "first text");
		}
	}
	return true; // one more poll checks the art took
}

// Root anchored at the top centre like hudmonsterhealthhd.json (anchor x 0.5, rect y = scoreboard_y, priority -90).
// Units = layout units, x 0 = the medallion's centre, y 0 = the board's top. Children draw in order: plates, medallion, texts.
auto ScoreLayout() -> std::string {
	const auto n   = [](double v) { return std::to_string(static_cast<int>(std::lround(v))); };
	const auto box = [&](const char* name, double x0, double y0, double x1, double y1, const std::string& text, const char* size, const char* color, const char* h) {
		return std::string("{ \"type\": \"TextBoxWidget\", \"name\": \"") + name + "\", \"fields\": { \"visible\": false, \"rect\": { \"x\": " + n(x0)
		     + ", \"y\": " + n(y0) + ", \"width\": " + n(x1 - x0) + ", \"height\": " + n(y1 - y0) + " }, \"text\": \"" + text
		     + "\", \"style\": { \"fontColor\": " + color + ", \"pointSize\": \"" + size
		     + "\", \"alignment\": { \"h\": \"" + h + "\", \"v\": \"center\" }, \"dropShadow\": \"$DefaultDropShadow\" } } }";
	};
	const auto img = [&](const char* name, double x, double y, const char* file) {
		return std::string("{ \"type\": \"ImageWidget\", \"name\": \"") + name + "\", \"fields\": { \"visible\": false, \"rect\": { \"x\": " + n(x) + ", \"y\": " + n(y)
		     + " }, \"filename\": \"PANEL\\\\Arena\\\\" + file + "\" } }";
	};
	constexpr const char* kGold  = "{ \"r\": 199, \"g\": 179, \"b\": 119, \"a\": 255 }";
	constexpr const char* kLight = "{ \"r\": 240, \"g\": 230, \"b\": 210, \"a\": 255 }";
	constexpr const char* kRed   = "{ \"r\": 225, \"g\": 45, \"b\": 35, \"a\": 255 }"; // 2.3.1: negative scores
	const double cy = kTeamCentreCy; // medallion centre: sprite top at y 0
	std::string  kids;
	std::string  texts;
	for (int p = 0; p < kTeamPlates; ++p) {
		const int    side = p / kTeamSlots;
		const double dy   = (p % kTeamSlots - 3) * kTeamStep;
		const double edge = std::sqrt(std::max(kTeamRing * kTeamRing - dy * dy, 0.0));
		const double x    = side == 0 ? -edge + kTeamTuck - kTeamPlateW : edge - kTeamTuck;
		const double y    = cy + dy - kTeamPlateH / 2.0;
		kids += img(kScoreImageIds[1 + p], x, y, "TeamPlate") + ",\n";
		const double f0 = x + kTeamField[0], f1 = x + kTeamField[2], fy0 = y + kTeamField[1], fy1 = y + kTeamField[3];
		if (side == 0) { // name at the outer (left) end, score at the inner end by the medallion
			texts += box(kPlateNameIds[p], f0 + 12, fy0 - kTeamNameLift, f1 - 70, fy1 - kTeamNameLift, g_scoreBoxes[kBoxPlateName + p].reserve, "$SmallFontSize", kLight, "left") + ",\n";
			texts += box(kPlateScoreIds[p], f1 - 66, fy0, f1 - 8, fy1, g_scoreBoxes[kBoxPlateScore + p].reserve, "$SmallFontSize", kGold, "right") + ",\n";
			texts += box(kPlateRedIds[p], f1 - 66, fy0, f1 - 8, fy1, g_scoreBoxes[kBoxPlateScoreRed + p].reserve, "$SmallFontSize", kRed, "right") + ",\n";
		} else {
			texts += box(kPlateScoreIds[p], f0 + 8, fy0, f0 + 66, fy1, g_scoreBoxes[kBoxPlateScore + p].reserve, "$SmallFontSize", kGold, "left") + ",\n";
			texts += box(kPlateRedIds[p], f0 + 8, fy0, f0 + 66, fy1, g_scoreBoxes[kBoxPlateScoreRed + p].reserve, "$SmallFontSize", kRed, "left") + ",\n";
			texts += box(kPlateNameIds[p], f0 + 70, fy0 - kTeamNameLift, f1 - 12, fy1 - kTeamNameLift, g_scoreBoxes[kBoxPlateName + p].reserve, "$SmallFontSize", kLight, "right") + ",\n";
		}
	}
	kids += img(kScoreImageIds[0], -kTeamCentreCx, 0, "TeamCentre") + ",\n";
	{ // 2.0.3: alone in the arena: one plate centred at the middle height, name left, score right
		const double x = -kTeamPlateW / 2.0, y = cy - kTeamPlateH / 2.0;
		kids += img(kScoreImageIds[kImgSolo], x, y, "TeamPlate") + ",\n";
		const double f0 = x + kTeamField[0], f1 = x + kTeamField[2], fy0 = y + kTeamField[1], fy1 = y + kTeamField[3];
		texts += box("SN", f0 + 12, fy0 - kTeamNameLift, f1 - 70, fy1 - kTeamNameLift, g_scoreBoxes[kBoxSoloName].reserve, "$SmallFontSize", kLight, "left") + ",\n";
		texts += box("SS", f1 - 66, fy0, f1 - 8, fy1, g_scoreBoxes[kBoxSoloScore].reserve, "$SmallFontSize", kGold, "right") + ",\n";
		texts += box("SSR", f1 - 66, fy0, f1 - 8, fy1, g_scoreBoxes[kBoxSoloScoreRed].reserve, "$SmallFontSize", kRed, "right") + ",\n";
	}
	const double d = kTeamDisc * 0.8;
	return std::string("{\n\"type\": \"Panel\", \"name\": \"") + kScorePanelName + "\",\n\"fields\": { \"anchor\": { \"x\": 0.5 }, \"rect\": { \"y\": "
	     + std::to_string(g_pvp.scoreboardY) + " }, \"priority\": -90 },\n\"children\": [\n" + kids + texts
	     + box("Score", -d, cy - d, d, cy + d, g_scoreBoxes[kBoxScore].reserve, "$LargeFontSize", kGold, "center") + ",\n" // the colon
	     + box("ScoreL", -d, cy - d, -kScoreGap, cy + d, g_scoreBoxes[kBoxScoreL].reserve, "$LargeFontSize", kGold, "right") + ",\n"
	     + box("ScoreR", kScoreGap, cy - d, d, cy + d, g_scoreBoxes[kBoxScoreR].reserve, "$LargeFontSize", kGold, "left") + ",\n"
	     + box("ScoreLR", -d, cy - d, -kScoreGap, cy + d, g_scoreBoxes[kBoxScoreLRed].reserve, "$LargeFontSize", kRed, "right") + ",\n"
	     + box("ScoreRR", kScoreGap, cy - d, d, cy + d, g_scoreBoxes[kBoxScoreRRed].reserve, "$LargeFontSize", kRed, "left") + ",\n"
	     // 2.1.0: "Battle" / "Royale" on two lines in the medallion (free-for-all)
	     + box("BR1", -d, cy - 44, d, cy, g_scoreBoxes[kBoxRoyal1].reserve, "$MediumFontSize", kGold, "center") + ",\n"
	     + box("BR2", -d, cy, d, cy + 44, g_scoreBoxes[kBoxRoyal2].reserve, "$MediumFontSize", kGold, "center") + ",\n"
	     + box("Line", -1500, 0, 1500, 52, g_scoreBoxes[kBoxLine].reserve, "$MediumFontSize", kGold, "center") + ",\n"
	     // duel notices (0.6.0): one line under the board
	     + box("Notice", -1500, kTeamCentreH + 10, 1500, kTeamCentreH + 62, g_scoreBoxes[kBoxNotice].reserve, "$MediumFontSize", kLight, "center") + "\n]\n}";
}

// Registers the scoreboard layout + panel at load (needs the panel/widget services and the native finders).
bool ScoreRegister(const D2RL::PluginContext* ctx, const D2RL::ResourceService* res) {
	if (!HudFinders(ctx)) {
		ctx->LogWarn("arena: scoreboard: native UI finders don't match this game build; no scoreboard (`arena scores` still works).");
		return false;
	}
	for (int i = 0; i < kScoreBoxes; ++i) {
		HudText& t = g_scoreBoxes[i];
		t.reserve.clear();
		while (t.reserve.size() < ScoreBoxReserve(i)) {
			t.reserve += "\xE2\x80\x8B";
		}
	}
	g_scoreLayout = ScoreLayout();
	const D2RL::Resources::ResourceRegistration layout {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.path       = "data/global/ui/layouts/arena/ScoreHudhd.json",
		.bytes      = g_scoreLayout.c_str(),
		.byteCount  = static_cast<uint32_t>(g_scoreLayout.size()),
	};
	D2RL::Resources::RegistrationHandle lh = D2RL::Resources::InvalidHandle;
	const D2RL::Panels::PanelRegistration panel { .structSize = D2RL::Panels::PanelRegistrationSize, .flags = D2RL::Panels::PanelFlags::None, .localId = "ScoreHud" };
	if (res->registerResource(ctx, &layout, &lh) != D2RL::Resources::Result::Success || g_panels->registerPanel(ctx, &panel, &g_scorePanel) != D2RL::Panels::Result::Success) {
		ctx->LogWarn("arena: scoreboard could not be registered (`arena scores` still works).");
		g_scorePanel = D2RL::Panels::InvalidHandle;
		return false;
	}
	g_scoreOn = true;
	return true;
}

// Duel notice under the scoreboard (also with the scoreboard hidden). UI thread; true = keep polling.
auto InvitePopupUp() -> bool; // player menu section
bool NoticeUpdate(const D2RL::PluginContext* ctx) {
	std::string text;
	bool        active = false;
	const bool  popup  = InvitePopupUp(); // 1.2.1: the duel popup replaces the challenge notice (and shows its countdown)
	{
		std::lock_guard lock(g_noticeLock);
		if (!g_hudInGame) { // a poll or a late message after GameLeft must not reopen the panel on the character screen
			g_noticeUntil = 0;
			g_noticeText.clear();
			g_noticeBaseUntil = 0;
			g_noticeBaseText.clear();
			return false;
		}
		const uint64_t now = NowMs();
		if (g_noticeUntil != 0 && now < g_noticeUntil && popup && g_noticeText.find("challenges you to") != std::string::npos) {
			active = true; // keep polling; shown again if the popup goes away first
		} else if (g_noticeUntil != 0 && now < g_noticeUntil) {
			active = true;
			text   = g_noticeText;
			if (g_noticeSeconds) {
				text += " (" + std::to_string((g_noticeUntil - now + 999) / 1000) + ")";
			}
		} else {
			g_noticeUntil = 0;
			if (g_noticeBaseUntil != 0 && now < g_noticeBaseUntil) {
				active = true;
				text   = g_noticeBaseText + " (" + std::to_string((g_noticeBaseUntil - now + 999) / 1000) + ")";
			} else {
				g_noticeBaseUntil = 0;
			}
		}
	}
	if (!g_scoreOn || (text.empty() && g_noticeShown.empty()) || text == g_noticeShown || !ScoreEnsureOpen(ctx)) {
		return active;
	}
	HudText& t = g_scoreBoxes[kBoxNotice];
	if (text.empty()) {
		HudSetVisible(ctx, t, false);
	} else if (HudWrite(t, text)) {
		HudSetVisible(ctx, t, true);
	} else {
		ScoreLogState("notice write failed");
		return active;
	}
	g_noticeShown = text;
	return active;
}

auto __cdecl OnDuelAcceptAction(const D2RL::PluginContext*, const D2RL::Input::ActionEvent* ev, void*) noexcept -> D2RL::Input::ActionResult {
	if (ev == nullptr || ev->kind != D2RL::Input::ActionEventKind::Pressed) {
		return D2RL::Input::ActionResult::Ignored;
	}
	(void)SendDuelCmd(kDuelAccept, "");
	return D2RL::Input::ActionResult::Handled;
}
auto __cdecl OnDuelDeclineAction(const D2RL::PluginContext*, const D2RL::Input::ActionEvent* ev, void*) noexcept -> D2RL::Input::ActionResult {
	if (ev == nullptr || ev->kind != D2RL::Input::ActionEventKind::Pressed) {
		return D2RL::Input::ActionResult::Ignored;
	}
	(void)SendDuelCmd(kDuelDecline, "");
	return D2RL::Input::ActionResult::Handled;
}

void ToggleScoreboard() {
	g_scoreHidden = !g_scoreHidden.load();
	Log("arena: scoreboard %s (hotkey / console).", g_scoreHidden.load() ? "hidden" : "shown");
	StartUiPoll();
}
auto __cdecl OnScoreAction(const D2RL::PluginContext* ctx, const D2RL::Input::ActionEvent* ev, void*) noexcept -> D2RL::Input::ActionResult {
	if (ctx == nullptr || ev == nullptr || ev->kind != D2RL::Input::ActionEventKind::Pressed) {
		return D2RL::Input::ActionResult::Ignored;
	}
	ToggleScoreboard();
	return D2RL::Input::ActionResult::Handled;
}

// g_client_bGameLoaded 0x142A233C4: 0 from packet 0x05 until packet 0x04 (D2CLIENT_SCMD_Rcv0x04_LoadComplete, Rcv0x05). Read, not written.
// Checked through the writer at 0x1401299CE: MOV dword ptr [rip+0x28F99EC],1.
constexpr uint64_t kRvaClientGameLoaded       = 0x2A233C4;
constexpr uint64_t kRvaClientGameLoadedWriter = 0x1299CE;
constexpr uint8_t  kClientGameLoadedWriter[]  = { 0xC7, 0x05, 0xEC, 0x99, 0x8F, 0x02, 0x01, 0x00, 0x00, 0x00 };
const volatile int32_t* g_clientGameLoaded    = nullptr;

// ---- 1.1.0 wager window, client side ----
// Laid out like the stock trade window (tradelayouthd.json: same background, hinge, title, name and gold rows, accept
// buttons). Top = the other player, bottom = you. The two 10 x 4 item grids of the background stay empty until item
// stakes exist (W3). Texts are written into the TextBoxes' native buffers (HudWrite), the gold is an input box read with
// getInputText; the buttons send PanelManager:TogglePanel:arena/Wager<Action> (registered command, no such panel),
// consumed in OnUiMessage.
constexpr char kWagerPanelName[] = "arena/Wager";
// 1.6.0: the Duel Rules page - names, then 8 rule rows (label, value, the challenger's arrows); row 1's label changes
std::array<HudText, 14> g_wagerText { HudText { "name_them", kWagerPanelName }, HudText { "name_us", kWagerPanelName }, HudText { "label1", kWagerPanelName },
                                      HudText { "val0", kWagerPanelName }, HudText { "val1", kWagerPanelName }, HudText { "val2", kWagerPanelName },
                                      HudText { "val3", kWagerPanelName }, HudText { "val4", kWagerPanelName }, HudText { "val5", kWagerPanelName },
                                      HudText { "val6", kWagerPanelName }, HudText { "val7", kWagerPanelName }, HudText { "val8", kWagerPanelName },
                                      HudText { "val9", kWagerPanelName }, HudText { "val10", kWagerPanelName } };
enum WagerBox { kWbNameThem, kWbNameUs, kWbLabel1, kWbVal0 };
constexpr int kRuleRows = 11; // 2.2.0: + Kill bonus (row 10; its arrow messages use the character '0' + 10 = ':')
const char* const kRuleLabels[kRuleRows] = { "Duel type", "", "Points per kill", "Death penalty", "Time limit", "Potion cooldown", "Potion limit", "Stakes",
	                                         "Teleport cooldown", "Opponent on map", "Kill bonus" };
// 1.2.1: the installed D2RCore refuses the TextEntry panel flag (D2RCORE_PANEL_RegisterPanelImpl: flags > 3 -> 2), and
// without it typed digits would also fire the belt hotkeys. Your gold is set with the stash arrows on these steps.
constexpr uint32_t kWagerSteps[] = { 0, 100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000, 250000, 500000, 1000000, 2500000 };
D2RL::Panels::RegistrationHandle g_wagerPanel = D2RL::Panels::InvalidHandle;
std::string                      g_wagerLayout;
bool                             g_wagerOn = false;      // registered
bool                             g_wagerPanelOpen = false; // UI thread
uint32_t                         g_wagerShownSeq  = 0;
uint32_t                         g_wagerSentGold  = UINT32_MAX;
D2RL::Widgets::WidgetHandle      g_wagerReadyThem = D2RL::Widgets::InvalidHandle;
D2RL::Widgets::WidgetHandle      g_wagerReadyMark = D2RL::Widgets::InvalidHandle; // our accept: the check tinted green (1.3.3)
D2RL::Widgets::WidgetHandle      g_wagerReadyBlock = D2RL::Widgets::InvalidHandle; // 1.3.3: the check tinted red during the agree wait
D2RL::Widgets::WidgetHandle      g_wagerReadyUs   = D2RL::Widgets::InvalidHandle; // the accept button (off during the agree wait)
std::array<D2RL::Widgets::WidgetHandle, kRuleRows * 2> g_ruleArrows {}; // 1.6.0: R<i>L / R<i>M (challenger only)
int                              g_wagerUiState   = -1; // last applied visibility/enabled set
bool                             g_wagerFailed    = false;

// 8 rows: label (static, row 1 dynamic), value, and the challenger's arrows (R<i>L / R<i>M).
auto RuleRows(const std::string& msg = "arena/WR", int y0 = 318, int dy = 84, const char* label7 = nullptr) -> std::string { // 2.2.0: dy 88 -> 84 (11 rows)
	std::string j;
	const char* labelStyle = "{ \"fontColor\": \"$FontColorGoldYellow\", \"pointSize\": \"$XMediumPanelFontSize\", \"alignment\": { \"h\": \"left\", \"v\": \"center\" } }";
	const char* valueStyle = "{ \"fontColor\": \"$FontColorWhite\", \"pointSize\": \"$XMediumPanelFontSize\", \"alignment\": { \"h\": \"center\", \"v\": \"center\" } }";
	for (int i = 0; i < kRuleRows; ++i) {
		const int  y  = y0 + i * dy;
		const auto ys = std::to_string(y);
		const std::string label = i == 1 ? g_wagerText[kWbLabel1].reserve : i == 7 && label7 != nullptr ? std::string(label7) : std::string(kRuleLabels[i]);
		if (i != 0) {
			j += ",\n";
		}
		j += std::string("{ \"type\": \"TextBoxWidget\", \"name\": \"") + (i == 1 ? "label1" : "label" + std::to_string(i)) + "\", \"fields\": { \"rect\": { \"x\": 130, \"y\": " + ys
		   + ", \"width\": 450, \"height\": 64 }, \"text\": \"" + label + "\", \"style\": " + labelStyle + " } },\n";
		j += "{ \"type\": \"ButtonWidget\", \"name\": \"R" + std::to_string(i) + "L\", \"fields\": { \"rect\": { \"x\": 600, \"y\": " + ys
		   + ", \"width\": 64, \"height\": 64 }, \"filename\": \"PANEL\\\\Stash\\\\Stash_LeftArrow\", \"normalFrame\": 0, \"pressedFrame\": 2, \"hoveredFrame\": 1, \"disabledFrame\": 3, "
		     "\"onClickMessage\": \"PanelManager:TogglePanel:" + msg + std::string(1, static_cast<char>('0' + i)) + "L\" } },\n";
		j += "{ \"type\": \"TextBoxWidget\", \"name\": \"val" + std::to_string(i) + "\", \"fields\": { \"rect\": { \"x\": 670, \"y\": " + ys
		   + ", \"width\": 280, \"height\": 64 }, \"text\": \"" + g_wagerText[kWbVal0 + i].reserve + "\", \"style\": " + valueStyle + " } },\n";
		j += "{ \"type\": \"ButtonWidget\", \"name\": \"R" + std::to_string(i) + "M\", \"fields\": { \"rect\": { \"x\": 960, \"y\": " + ys
		   + ", \"width\": 64, \"height\": 64 }, \"filename\": \"PANEL\\\\Stash\\\\Stash_RightArrow\", \"normalFrame\": 0, \"pressedFrame\": 2, \"hoveredFrame\": 1, \"disabledFrame\": 3, "
		     "\"onClickMessage\": \"PanelManager:TogglePanel:" + msg + std::string(1, static_cast<char>('0' + i)) + "M\" } }";
	}
	return j;
}
auto RuleValue(const DuelRules& r, int row) -> std::string {
	const bool kills = r.type == kTypeKills;
	switch (row) {
	case 0: return r.type == kTypeKills ? "Kills" : r.type == kTypePoints ? "Points" : "Timed";
	case 1: return r.type == kTypeTimed ? "-" : std::to_string(r.target);
	case 2: return kills ? "-" : std::to_string(r.killWorth);
	case 3: return kills ? "-" : r.deathPenalty == 0 ? "none" : "-" + std::to_string(r.deathPenalty);
	case 4: return r.minutes == 0 ? "none" : std::to_string(r.minutes) + " min";
	case 5: return r.potionCd == 0 ? "none" : std::to_string(r.potionCd) + " s";
	case 6: return r.potionLimit == 0 ? "no limit" : std::to_string(r.potionLimit) + " each";
	case 7: return r.stakes != 0 ? "items + gold" : "none";
	case 8: return r.teleCd == 0 ? "stock" : std::to_string(r.teleCd) + " s";
	case 9: return r.mapShow == 0 ? "hidden" : r.mapShow == 1 ? "shown" : r.mapShow == 6 ? "in sight" : "within " + std::to_string(MapRange(r));
	case 10: return r.killBonus != 0 ? "on" : "off";
	default: return std::string();
	}
}
auto WagerLayout() -> std::string {
	const auto text = [](const char* name, const char* rect, const std::string& reserve, const char* style) {
		return std::string("{ \"type\": \"TextBoxWidget\", \"name\": \"") + name + "\", \"fields\": { \"rect\": " + rect + ", \"text\": \"" + reserve
		     + "\", \"style\": " + style + " } }";
	};
	const char* nameStyle = "{ \"pointSize\": \"$XMediumPanelFontSize\", \"spacing\": \"$MinimumSpacing\", \"alignment\": { \"h\": \"left\", \"v\": \"center\" } }";
	return std::string("{\n\"type\": \"Panel\", \"name\": \"") + kWagerPanelName + "\",\n"
	     + "\"fields\": { \"priority\": 5, \"rect\": \"$LeftPanelRect\", \"anchor\": \"$LeftPanelAnchor\" },\n\"children\": [\n"
	     + "{ \"type\": \"ClickCatcherWidget\", \"name\": \"click_catcher\", \"fields\": { \"rect\": \"$PanelClickCatcherRect\" } },\n"
	     + "{ \"type\": \"ImageWidget\", \"name\": \"background\", \"fields\": { \"filename\": \"PANEL\\\\Trade\\\\MultiplayerTrade_BG\" } },\n"
	     + "{ \"type\": \"ImageWidget\", \"name\": \"LeftHinge\", \"fields\": { \"rect\": \"$LeftHingeRect\", \"filename\": \"$LeftHingeSprite\" } },\n"
	     + "{ \"type\": \"TextBoxWidget\", \"name\": \"Title\", \"fields\": { \"rect\": \"$InGamePanelTitleRect\", \"style\": \"$StyleTitleBlock\", \"text\": \"Duel Rules\" } },\n"
	     + text("name_them", "{ \"x\": 224, \"y\": 207, \"width\": 430, \"height\": 52 }", g_wagerText[kWbNameThem].reserve, nameStyle) + ",\n"
	     + text("name_us", "{ \"x\": 224, \"y\": 1277, \"width\": 430, \"height\": 52 }", g_wagerText[kWbNameUs].reserve, nameStyle) + ",\n"
	     // 1.6.0: the rules box over both grids
	     // 2.3.0 (user): solid black like the team lobby (was 0.88), down to y 1256 for the 11th row (name_us box at 1277)
	     + "{ \"type\": \"NineTileImageWidget\", \"name\": \"rules_box\", \"fields\": { \"rect\": { \"x\": 86, \"y\": 266, \"width\": 992, \"height\": 990 }, "
	       "\"filename\": \"PANEL/TooltipBorder/TooltipBorder\", \"backgroundColor\": [ 0.0, 0.0, 0.0, 1.0 ], \"tileScale\": 1.0 } },\n"
	     + RuleRows() + ",\n"
	     // 2.3.2 (user): side tab -> Duel Rules Guide, as the team lobby's guide tab
	     + "{ \"type\": \"ButtonWidget\", \"name\": \"guide_tab\", \"fields\": { \"rect\": { \"x\": 1108, \"y\": 711 }, \"filename\": \"PANEL\\\\Character_Sheet\\\\AdvancedStatsButton\", "
	       "\"hoveredFrame\": 3, \"pressLabelOffset\": { \"x\": 0, \"y\": 2 }, \"tooltipString\": \"Duel rules guide\", \"onClickMessage\": \"PanelManager:TogglePanel:arena/RulesGuideTab\" } },\n"
	     // 1.3.3: coloured like the stock trade. TradePanel_Update 0x1402E4EE0 copies the panel's enabledTint (+0x168) /
	     // disabledTint (+0x178) into the accept buttons; UI_TradePanel_Create 0x1402E42C0 sets them to green (0,1,0,1) and
	     // red (1,0,0,1). Here: the check image (frame 0) with the reflected AbstractImageWidget tint, shown by state.
	     + "{ \"type\": \"ImageWidget\", \"name\": \"ready_them\", \"fields\": { \"rect\": { \"x\": 103, \"y\": 198 }, \"filename\": \"PANEL\\\\Trade\\\\AcceptButton\", \"frame\": 0, \"tint\": [ 0.0, 1.0, 0.0, 1.0 ] } },\n"
	     + "{ \"type\": \"ButtonWidget\", \"name\": \"ready_us\", \"fields\": { \"rect\": { \"x\": 103, \"y\": 1267 }, \"filename\": \"PANEL\\\\Trade\\\\AcceptButton\", "
	       "\"hoveredFrame\": 3, \"normalFrame\": 0, \"pressedFrame\": 1, \"disabledFrame\": 0, \"disabledTint\": [ 1.0, 0.0, 0.0, 1.0 ], \"tooltipString\": \"@d2r:strAcceptTrade\", \"tooltipOffset\": { \"x\": 0, \"y\": -3 }, "
	       "\"onClickMessage\": \"PanelManager:TogglePanel:arena/WagerReady\" } },\n"
	     + "{ \"type\": \"ImageWidget\", \"name\": \"ready_us_mark\", \"fields\": { \"rect\": { \"x\": 103, \"y\": 1267 }, \"filename\": \"PANEL\\\\Trade\\\\AcceptButton\", \"frame\": 0, \"tint\": [ 0.0, 1.0, 0.0, 1.0 ] } },\n"
	     + "{ \"type\": \"ImageWidget\", \"name\": \"ready_us_block\", \"fields\": { \"rect\": { \"x\": 103, \"y\": 1267 }, \"filename\": \"PANEL\\\\Trade\\\\AcceptButton\", \"frame\": 0, \"tint\": [ 1.0, 0.0, 0.0, 1.0 ] } },\n"
	     + "{ \"type\": \"ButtonWidget\", \"name\": \"close\", \"fields\": { \"rect\": { \"x\": 1075, \"y\": 9 }, \"filename\": \"PANEL\\\\closebtn_4x\", "
	       "\"hoveredFrame\": 3, \"normalFrame\": 0, \"pressedFrame\": 1, \"tooltipString\": \"@d2r:strClose\", \"sound\": \"cursor_close_window_hd\", "
	       "\"onClickMessage\": \"PanelManager:TogglePanel:arena/WagerClose\" } }\n]\n}";
}

bool WagerRegister(const D2RL::PluginContext* ctx, const D2RL::ResourceService* res) {
	if (!HudFinders(ctx)) {
		return false;
	}
	for (HudText& t : g_wagerText) {
		t.reserve.clear();
		while (t.reserve.size() < kHudReserve) {
			t.reserve += "\xE2\x80\x8B";
		}
	}
	g_wagerLayout = WagerLayout();
	const D2RL::Resources::ResourceRegistration layout {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.path       = "data/global/ui/layouts/arena/Wagerhd.json",
		.bytes      = g_wagerLayout.c_str(),
		.byteCount  = static_cast<uint32_t>(g_wagerLayout.size()),
	};
	D2RL::Resources::RegistrationHandle lh = D2RL::Resources::InvalidHandle;
	const auto flags = D2RL::Panels::PanelFlags::CloseOnEscape | D2RL::Panels::PanelFlags::GameplayLeftSlot;
	const D2RL::Panels::PanelRegistration panel { .structSize = D2RL::Panels::PanelRegistrationSize, .flags = flags, .localId = "Wager" };
	if (res->registerResource(ctx, &layout, &lh) != D2RL::Resources::Result::Success || g_panels->registerPanel(ctx, &panel, &g_wagerPanel) != D2RL::Panels::Result::Success) {
		ctx->LogWarn("arena: wager window could not be registered; use `arena duel <name> <gold>`.");
		g_wagerPanel = D2RL::Panels::InvalidHandle;
		return false;
	}
	g_wagerOn = true;
	return true;
}

// Panel -> host (the hosting player's own commands go straight into the queue).
void SendWagerCmd(uint8_t kind, uint32_t value) {
	bool host = false;
	{
		std::lock_guard lock(g_pvpLock);
		host = g_game != nullptr;
	}
	if (host) {
		{
			std::lock_guard lock(g_wagerCmdLock);
			g_wagerCmds.push_back({ g_localGuid.load(), kind, value });
		}
		QueueTick();
		return;
	}
	if (g_net != nullptr) {
		uint8_t msg[5] { kind };
		std::memcpy(msg + 1, &value, 4);
		(void)g_net->sendToHost(g_ctx, g_channel, kMsgWagerCmd, msg, sizeof(msg));
	}
}

D2RL::Panels::RegistrationHandle g_rulesGuidePanel = D2RL::Panels::InvalidHandle; // 2.3.2: Duel Rules Guide (lobby section)
auto RulesGuideOpen(const D2RL::PluginContext* ctx) -> bool;                       // lobby section
void WagerClosePanel(const D2RL::PluginContext* ctx) {
	ShowInvAfterWindow(ctx); // 2.4.1
	for (HudText& t : g_wagerText) { // 1.3.1: also when the game closed it (Esc): else the next open can't write
		HudRestoreText(t);
	}
	if (RulesGuideOpen(ctx)) {
		(void)g_panels->closePanel(ctx, g_rulesGuidePanel); // 2.3.2: the guide goes with the window
	}
	if (g_wagerPanelOpen) {
		(void)g_panels->closePanel(ctx, g_wagerPanel);
	}
	g_wagerPanelOpen = false;
	g_wagerReadyThem = D2RL::Widgets::InvalidHandle;
	g_wagerReadyMark = g_wagerReadyBlock = g_wagerReadyUs = D2RL::Widgets::InvalidHandle;
	g_ruleArrows.fill(D2RL::Widgets::InvalidHandle);
	g_wagerUiState   = -1;
	g_wagerSentGold  = UINT32_MAX;
	for (HudText& t : g_wagerText) {
		t.buffer = nullptr;
	}
}

auto WagerGold(uint32_t gold) -> std::string {
	std::string d = std::to_string(gold), out;
	for (size_t i = 0; i < d.size(); ++i) { // 12,345 like the stock gold boxes
		if (i != 0 && (d.size() - i) % 3 == 0) {
			out += ',';
		}
		out += d[i];
	}
	return out;
}

// UI thread (UiPoll): opens / fills / closes the window from the host's view; reads the gold box. True = keep polling.
bool WagerUiUpdate(const D2RL::PluginContext* ctx) {
	WagerView v;
	{
		std::lock_guard lock(g_wagerViewLock);
		v = g_wagerView;
		if (v.open == 1 && !g_wagerOn) { // 1.2.2: no window here (not registered): close it on the host at once
			g_wagerView.open = 0;
		}
	}
	if (!g_wagerOn) {
		if (v.open == 1) {
			SendWagerCmd(kWagerClose, 0);
			Log("arena: wager window can't be shown here; closed.");
		}
		return false;
	}
	if (v.open != 1) {
		if (g_wagerPanelOpen) {
			WagerClosePanel(ctx);
		}
		return false;
	}
	if (g_wagerPanelOpen) { // closed by Esc or another left panel: that is a cancel
		D2RL::Panels::PanelInfo info {};
		info.structSize = D2RL::Panels::PanelInfoSize;
		if (g_panels->getPanelInfo(ctx, g_wagerPanel, &info) == D2RL::Panels::Result::Success && info.presentationState == D2RL::Panels::PresentationState::Closed) {
			g_wagerPanelOpen = false;
			WagerClosePanel(ctx);
			{
				std::lock_guard lock(g_wagerViewLock);
				g_wagerView.open = 0;
			}
			SendWagerCmd(kWagerClose, 0);
			return false;
		}
	}
	if (!g_wagerPanelOpen) {
		{
			std::lock_guard lock(g_inviteLock); // accepted: the invite popup is done
			g_inviteView.until = 0;
		}
		if (g_panels->openPanel(ctx, g_wagerPanel) != D2RL::Panels::Result::Success) {
			Log("arena: wager window: openPanel failed; closed for both (see d2rloader.log).");
			g_wagerFailed = true;
			{
				std::lock_guard lock(g_wagerViewLock);
				g_wagerView.open = 0;
			}
			SendWagerCmd(kWagerClose, 0); // 1.2.2: never leave the other player (or the host) with a window nobody sees
			return false;
		}
		g_wagerPanelOpen = true;
		g_wagerShownSeq  = 0;
		D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle;
		if (g_widgets->findPanel(ctx, kWagerPanelName, &panel) == D2RL::Widgets::Result::Success) {
			(void)g_widgets->findWidget(ctx, panel, "ready_them", &g_wagerReadyThem);
			(void)g_widgets->findWidget(ctx, panel, "ready_us_mark", &g_wagerReadyMark);
			(void)g_widgets->findWidget(ctx, panel, "ready_us_block", &g_wagerReadyBlock);
			(void)g_widgets->findWidget(ctx, panel, "ready_us", &g_wagerReadyUs);
			for (int i = 0; i < kRuleRows; ++i) {
				(void)g_widgets->findWidget(ctx, panel, ("R" + std::to_string(i) + "L").c_str(), &g_ruleArrows[i * 2]);
				(void)g_widgets->findWidget(ctx, panel, ("R" + std::to_string(i) + "M").c_str(), &g_ruleArrows[i * 2 + 1]);
			}
		}
		Log("arena: wager window open.");
	}
	if (v.seq != g_wagerShownSeq) {
		bool ok = HudWrite(g_wagerText[kWbNameThem], v.them) && HudWrite(g_wagerText[kWbNameUs], v.me)
		       && HudWrite(g_wagerText[kWbLabel1], v.rules.type == kTypeKills ? "Kills to win" : v.rules.type == kTypePoints ? "Points to win" : "Target");
		for (int i = 0; i < kRuleRows && ok; ++i) {
			ok = HudWrite(g_wagerText[kWbVal0 + i], RuleValue(v.rules, i));
		}
		if (ok) {
			g_wagerShownSeq = v.seq;
		}
	}
	// marks, the challenger's arrows, and the accept button (off until 2 s after the last change)
	const bool canAgree = NowMs() >= v.agreeAt;
	const int  state    = (v.theirReady ? 1 : 0) | (v.myReady ? 2 : 0) | (v.canSet ? 4 : 0) | (canAgree ? 8 : 0) | (static_cast<int>(v.seq & 0xFFFF) << 4);
	if (state != g_wagerUiState) {
		g_wagerUiState = state;
		const auto vis = [&](D2RL::Widgets::WidgetHandle h, bool on) {
			if (h != D2RL::Widgets::InvalidHandle) {
				(void)g_widgets->setWidgetVisible(ctx, h, on);
			}
		};
		vis(g_wagerReadyThem, v.theirReady);
		vis(g_wagerReadyMark, v.myReady && canAgree);
		vis(g_wagerReadyBlock, !canAgree);
		for (D2RL::Widgets::WidgetHandle h : g_ruleArrows) {
			vis(h, v.canSet);
		}
		if (g_wagerReadyUs != D2RL::Widgets::InvalidHandle) {
			(void)g_widgets->setWidgetEnabled(ctx, g_wagerReadyUs, canAgree);
		}
	}
	return true;
}
// The arrows: the next step up or down from your current offer (UI thread).
void WagerStep(bool more) {
	uint32_t cur = 0;
	{
		std::lock_guard lock(g_wagerViewLock);
		cur = g_wagerView.myGold;
	}
	uint32_t next = cur;
	if (more) {
		for (uint32_t v : kWagerSteps) {
			if (v > cur) {
				next = v;
				break;
			}
		}
	} else {
		next = 0;
		for (uint32_t v : kWagerSteps) {
			if (v < cur) {
				next = v;
			}
		}
	}
	if (next != cur) {
		SendWagerCmd(kWagerOffer, next);
	}
}


// ---- 1.9.0 team duel lobby window (opened by the obelisk in Lut Gholein) ----
// Same frame as the rules page: two team columns of 4 names with a Join button each, the rule rows (the first player in
// the lobby sets them; stakes none for now), the ready check and a status line.
constexpr char kLobbyPanelName[] = "arena/Lobby";
constexpr int  kLobbyNames       = 8; // n0-n3 team 1, n4-n7 team 2
std::array<HudText, kLobbyNames + 3 + kRuleRows + kLobbyNames> g_lobbyText {
	HudText { "n0", kLobbyPanelName }, HudText { "n1", kLobbyPanelName }, HudText { "n2", kLobbyPanelName }, HudText { "n3", kLobbyPanelName },
	HudText { "n4", kLobbyPanelName }, HudText { "n5", kLobbyPanelName }, HudText { "n6", kLobbyPanelName }, HudText { "n7", kLobbyPanelName },
	HudText { "status", kLobbyPanelName }, HudText { "label1", kLobbyPanelName }, HudText { "val0", kLobbyPanelName }, HudText { "val1", kLobbyPanelName },
	HudText { "val2", kLobbyPanelName }, HudText { "val3", kLobbyPanelName }, HudText { "val4", kLobbyPanelName }, HudText { "val5", kLobbyPanelName },
	HudText { "val6", kLobbyPanelName }, HudText { "val7", kLobbyPanelName }, HudText { "val8", kLobbyPanelName }, HudText { "val9", kLobbyPanelName }, HudText { "val10", kLobbyPanelName },
	HudText { "readyx", kLobbyPanelName },
	// 2.0.1: the same rows in green (a ready player); the lobby text boxes don't read colour codes (screenshot 17:56).
	// 2.3.2: named gn0-gn7 - as g0-g7 they clashed with the guide panel's g0-g26 and the guide no longer opened.
	HudText { "gn0", kLobbyPanelName }, HudText { "gn1", kLobbyPanelName }, HudText { "gn2", kLobbyPanelName }, HudText { "gn3", kLobbyPanelName },
	HudText { "gn4", kLobbyPanelName }, HudText { "gn5", kLobbyPanelName }, HudText { "gn6", kLobbyPanelName }, HudText { "gn7", kLobbyPanelName } };
enum LobbyBox { kLbStatus = kLobbyNames, kLbLabel1, kLbVal0, kLbReady = kLbVal0 + kRuleRows, kLbGreen0 };
// 1.9.2 (user 2026-11-08): the explanations moved to a guide that pops out of a side tab, as the smithing recipe book
// (same position: x 1398, y -651 from $LeftPanelAnchor, AdvancedCharacterSheet background, side tab = the character
// sheet's AdvancedStatsButton at its stock spot 1108, 711).
constexpr char                   kGuidePanelName[] = "arena/LobbyGuide";
D2RL::Panels::RegistrationHandle g_guidePanel      = D2RL::Panels::InvalidHandle;
std::string                      g_guideLayout;
// 2.4.0 (user 2026-11-08, screenshots 21:03 / 21:04): the guides were one long page in the small font that ran past the edge
// and was hard to read on the grey background. Now pages like smithing's recipe book: a black box behind the text, the
// lobby's bigger font ($XMediumPanelFontSize, lines of at most ~30 characters), kGuideLinesPerPage lines a page, Prev / Next
// arrows and "n / m" at the bottom (smithing's BookPrev / BookNext spots). Lines starting with '#' are gold headings.
// Texts are written into reserved boxes (HudWrite) when a page is shown; white and gold twin boxes per line.
constexpr int kGuideLinesPerPage = 16;
constexpr int kGuideBoxes        = 2 * kGuideLinesPerPage + 1; // white lines, gold headings, page label
const char* const kGuideWhiteIds[kGuideLinesPerPage] = { "gw0", "gw1", "gw2", "gw3", "gw4", "gw5", "gw6", "gw7", "gw8", "gw9", "gw10", "gw11", "gw12", "gw13", "gw14", "gw15" };
const char* const kGuideGoldIds[kGuideLinesPerPage]  = { "gy0", "gy1", "gy2", "gy3", "gy4", "gy5", "gy6", "gy7", "gy8", "gy9", "gy10", "gy11", "gy12", "gy13", "gy14", "gy15" };
struct GuideBook {
	const char*                           panel;
	const char*                           tag; // messages arena/<tag>Prev, arena/<tag>Next, arena/<tag>Tab
	std::vector<std::vector<std::string>> pages;
	int                                   page  = 0;
	int                                   shown = -1; // page on screen (-1 = none written yet)
	std::array<HudText, kGuideBoxes>      text {};
};
// 2.3.2: the kill bonus lines with the toml's numbers.
auto KillBonusGuideLines() -> std::vector<std::string> {
	return { "Kill bonus: a kill refills", "   " + std::to_string(g_pvp.killBonusLife) + "% of your missing life",
		     "   and " + std::to_string(g_pvp.killBonusMana) + "% of your mana." };
}
auto TeamGuidePages() -> std::vector<std::vector<std::string>> {
	std::vector<std::string> p4 = { "#Rules", "Opponent on map: shown,", "   in sight, near (within a", "   distance) or hidden." };
	for (const std::string& l : KillBonusGuideLines()) {
		p4.push_back(l);
	}
	p4.insert(p4.end(), { "#Leaving", "Walk away from the obelisk", "   or close the window." });
	return {
		{ "#Teams", "Picked: join Team One", "   or Team Two yourself.", "   Allowed: 1v1-4v4, 1v2, 1v3.", "Mixed: join the draw.",
		  "   When everyone is ready", "   the teams are drawn at", "   random; an odd player", "   joins Team Two. The teams",
		  "   show for 3 seconds.", "#Lobby", "The first player (R) sets", "   the rules.", "Ready: click the check",
		  "   mark - your name turns", "   green." },
		{ "#Lobby", "Teammates can't hurt", "   each other.", "#Duel types", "Kills: the first team to", "   reach the kills wins.",
		  "Points: kills give points,", "   deaths cost points; the", "   first team to the points", "   wins.", "Timed: the most points",
		  "   when the time runs out.", "   A tie: sudden death - no", "   more respawns, the last", "   team standing wins." },
		{ "#Rules", "Points per kill: added to", "   the killer's team.", "Death penalty: taken from", "   the fallen player's team.",
		  "Time limit: Kills / Points", "   duels stop there too.", "Potion cooldown: seconds", "   between two potions.",
		  "Potion limit: potions of", "   each kind per duel.", "Teleport cooldown: seconds", "   between two Teleports." },
		p4,
	};
}
auto RulesGuidePages() -> std::vector<std::vector<std::string>> {
	std::vector<std::string> p3 = { "#Rules", "Stakes: items + gold in the", "   trade window; the winner", "   takes the loser's stake.",
		                            "Teleport cooldown: seconds", "   between two Teleports.", "Opponent on map: shown,", "   in sight, near (within a",
		                            "   distance) or hidden." };
	for (const std::string& l : KillBonusGuideLines()) {
		p3.push_back(l);
	}
	p3.insert(p3.end(), { "#Leaving", "Close the window or walk", "   away to cancel." });
	return {
		{ "#Duel types", "Kills: the first to reach", "   the kills wins.", "Points: kills give points,", "   deaths cost points; the",
		  "   first to the points wins.", "Timed: the most points", "   when the time runs out.", "   A tie: sudden death -",
		  "   the next kill wins.", "#Agreeing", "The challenger sets the", "   rules with the arrows.", "Both click the check mark",
		  "   to start; any change", "   takes both checks back." },
		{ "#Rules", "Kills / Points to win:", "   the target.", "Points per kill: added to", "   the killer.", "Death penalty: taken from",
		  "   the fallen player.", "Time limit: Kills / Points", "   duels stop there too.", "Potion cooldown: seconds",
		  "   between two potions.", "Potion limit: potions of", "   each kind per duel." },
		p3,
	};
}
// 2.3.2 (user): Duel Rules window side tab -> "Duel Rules Guide" (what each row means).
constexpr char kRulesGuidePanelName[] = "arena/RulesGuide"; // g_rulesGuidePanel: next to WagerClosePanel
std::string    g_rulesGuideLayout;
GuideBook      g_teamGuide { kGuidePanelName, "LobbyGuide" };
GuideBook      g_rulesGuide { kRulesGuidePanelName, "RulesGuide" };
void GuideInit(GuideBook& g, std::vector<std::vector<std::string>> pages) {
	g.pages = std::move(pages);
	g.page = 0, g.shown = -1;
	for (int i = 0; i < kGuideLinesPerPage; ++i) {
		g.text[i]                      = HudText { kGuideWhiteIds[i], g.panel };
		g.text[kGuideLinesPerPage + i] = HudText { kGuideGoldIds[i], g.panel };
	}
	g.text[kGuideBoxes - 1] = HudText { "gpage", g.panel };
	for (HudText& t : g.text) {
		while (t.reserve.size() < kHudReserve) {
			t.reserve += "\xE2\x80\x8B";
		}
	}
}
auto GuideLayoutFor(const GuideBook& g, const char* title) -> std::string {
	const std::string& rsv = g.text[0].reserve;
	const auto line = [&](const char* name, int y, const char* color) {
		return std::string(",\n{ \"type\": \"TextBoxWidget\", \"name\": \"") + name + "\", \"fields\": { \"rect\": { \"x\": 100, \"y\": " + std::to_string(y)
		     + ", \"width\": 690, \"height\": 60 }, \"text\": \"" + rsv + "\", \"style\": { \"fontColor\": \"" + color
		     + "\", \"pointSize\": \"$XMediumPanelFontSize\", \"alignment\": { \"h\": \"left\", \"v\": \"center\" } } } }";
	};
	std::string j = std::string("{\n\"type\": \"Panel\", \"name\": \"") + g.panel + "\",\n"
	              + "\"fields\": { \"priority\": 6, \"rect\": { \"x\": 1398, \"y\": -651 }, \"anchor\": \"$LeftPanelAnchor\" },\n\"children\": [\n"
	              + "{ \"type\": \"ImageWidget\", \"name\": \"Background\", \"fields\": { \"filename\": \"PANEL\\\\AdvancedCharacterSheet\\\\AdvancedCharacterSheet_BG\" } },\n"
	              + "{ \"type\": \"ClickCatcherWidget\", \"name\": \"ClickCatcher\", \"fields\": { \"rect\": { \"width\": 875, \"height\": 1500 } } },\n"
	              + "{ \"type\": \"TextBoxWidget\", \"name\": \"Title\", \"fields\": { \"text\": \"" + title + "\", \"rect\": { \"x\": 120, \"y\": 61, \"width\": 640, \"height\": 85 }, \"style\": \"$StyleTitleBlock\" } },\n"
	              + "{ \"type\": \"NineTileImageWidget\", \"name\": \"guide_box\", \"fields\": { \"rect\": { \"x\": 70, \"y\": 160, \"width\": 736, \"height\": 1070 }, "
	                "\"filename\": \"PANEL/TooltipBorder/TooltipBorder\", \"backgroundColor\": [ 0.0, 0.0, 0.0, 0.92 ], \"tileScale\": 1.0 } }";
	for (int i = 0; i < kGuideLinesPerPage; ++i) {
		const int y = 180 + i * 64;
		j += line(kGuideWhiteIds[i], y, "$FontColorWhite");
		j += line(kGuideGoldIds[i], y, "$FontColorGoldYellow");
	}
	j += std::string(",\n{ \"type\": \"TextBoxWidget\", \"name\": \"gpage\", \"fields\": { \"rect\": { \"x\": 364, \"y\": 1375, \"width\": 148, \"height\": 64 }, \"text\": \"") + rsv
	   + "\", \"style\": { \"fontColor\": \"$FontColorWhite\", \"pointSize\": \"$XMediumPanelFontSize\", \"alignment\": { \"h\": \"center\", \"v\": \"center\" } } } }";
	for (const char* dir : { "Prev", "Next" }) {
		const bool next = dir[0] == 'N';
		j += std::string(",\n{ \"type\": \"ButtonWidget\", \"name\": \"Guide") + dir + "\", \"fields\": { \"rect\": { \"x\": " + (next ? "512" : "300")
		   + ", \"y\": 1375, \"width\": 64, \"height\": 64 }, \"filename\": \"PANEL\\\\Stash\\\\Stash_" + (next ? "Right" : "Left") + "Arrow\", "
		     "\"normalFrame\": 0, \"pressedFrame\": 2, \"hoveredFrame\": 1, \"disabledFrame\": 3, \"onClickMessage\": \"PanelManager:TogglePanel:arena/" + g.tag + dir + "\" } }";
	}
	return j + "\n]\n}";
}
// Writes the current page while the guide is open (UI poll). True while open (keep polling).
auto GuideUiUpdate(GuideBook& g, bool open) -> bool {
	if (!open || g.pages.empty()) {
		g.shown = -1;
		return false;
	}
	g.page = std::clamp(g.page, 0, static_cast<int>(g.pages.size()) - 1);
	if (g.shown == g.page) {
		return true;
	}
	const std::vector<std::string>& lines = g.pages[g.page];
	bool ok = true;
	for (int i = 0; i < kGuideLinesPerPage && ok; ++i) {
		const std::string l    = i < static_cast<int>(lines.size()) ? lines[i] : std::string();
		const bool        head = !l.empty() && l[0] == '#';
		ok = HudWrite(g.text[i], head ? std::string() : l) && HudWrite(g.text[kGuideLinesPerPage + i], head ? l.substr(1) : std::string());
	}
	ok = ok && HudWrite(g.text[kGuideBoxes - 1], std::to_string(g.page + 1) + " / " + std::to_string(g.pages.size()));
	if (ok) {
		g.shown = g.page;
	}
	return true;
}
auto RulesGuideOpen(const D2RL::PluginContext* ctx) -> bool {
	D2RL::Panels::PanelInfo info {};
	info.structSize = D2RL::Panels::PanelInfoSize;
	return g_rulesGuidePanel != D2RL::Panels::InvalidHandle && g_panels->getPanelInfo(ctx, g_rulesGuidePanel, &info) == D2RL::Panels::Result::Success
	    && info.presentationState == D2RL::Panels::PresentationState::Open;
}
auto GuideOpen(const D2RL::PluginContext* ctx) -> bool {
	D2RL::Panels::PanelInfo info {};
	info.structSize = D2RL::Panels::PanelInfoSize;
	return g_guidePanel != D2RL::Panels::InvalidHandle && g_panels->getPanelInfo(ctx, g_guidePanel, &info) == D2RL::Panels::Result::Success
	    && info.presentationState == D2RL::Panels::PresentationState::Open;
}
D2RL::Panels::RegistrationHandle g_lobbyPanel = D2RL::Panels::InvalidHandle;
std::string                      g_lobbyLayout;
bool                             g_lobbyOn = false, g_lobbyPanelOpen = false;
uint32_t                         g_lobbyShownSeq = 0;
int                              g_lobbyUiState  = -1;
D2RL::Widgets::WidgetHandle      g_lobbyReadyUs = D2RL::Widgets::InvalidHandle, g_lobbyReadyMark = D2RL::Widgets::InvalidHandle,
                                 g_lobbyReadyBlock = D2RL::Widgets::InvalidHandle;
std::array<D2RL::Widgets::WidgetHandle, kRuleRows * 2> g_lobbyArrows {};

auto LobbyLayout() -> std::string {
	const std::string& rsv  = g_lobbyText[0].reserve;
	const char*        name = "{ \"fontColor\": \"$FontColorWhite\", \"pointSize\": \"$XMediumPanelFontSize\", \"alignment\": { \"h\": \"left\", \"v\": \"center\" } }";
	const char*        green = "{ \"fontColor\": { \"r\": 40, \"g\": 210, \"b\": 40, \"a\": 255 }, \"pointSize\": \"$XMediumPanelFontSize\", \"alignment\": { \"h\": \"left\", \"v\": \"center\" } }";
	std::string        j    = std::string("{\n\"type\": \"Panel\", \"name\": \"") + kLobbyPanelName + "\",\n"
	              + "\"fields\": { \"priority\": 5, \"rect\": \"$LeftPanelRect\", \"anchor\": \"$LeftPanelAnchor\" },\n\"children\": [\n"
	              + "{ \"type\": \"ClickCatcherWidget\", \"name\": \"click_catcher\", \"fields\": { \"rect\": \"$PanelClickCatcherRect\" } },\n"
	              + "{ \"type\": \"ImageWidget\", \"name\": \"background\", \"fields\": { \"filename\": \"PANEL\\\\Trade\\\\MultiplayerTrade_BG\" } },\n"
	              + "{ \"type\": \"ImageWidget\", \"name\": \"LeftHinge\", \"fields\": { \"rect\": \"$LeftHingeRect\", \"filename\": \"$LeftHingeSprite\" } },\n"
	              + "{ \"type\": \"TextBoxWidget\", \"name\": \"Title\", \"fields\": { \"rect\": \"$InGamePanelTitleRect\", \"style\": \"$StyleTitleBlock\", \"text\": \"Team Duels\" } },\n"
	              + "{ \"type\": \"NineTileImageWidget\", \"name\": \"lobby_box\", \"fields\": { \"rect\": { \"x\": 86, \"y\": 180, \"width\": 992, \"height\": 1060 }, "
	                "\"filename\": \"PANEL/TooltipBorder/TooltipBorder\", \"backgroundColor\": [ 0.0, 0.0, 0.0, 1.0 ], \"tileScale\": 1.0 } },\n";
	for (int t = 0; t < 2; ++t) {
		const std::string x = std::to_string(t == 0 ? 120 : 600);
		j += "{ \"type\": \"ButtonWidget\", \"name\": \"join" + std::to_string(t + 1) + "\", \"fields\": { \"rect\": { \"x\": " + x
		   + ", \"y\": 196, \"width\": 440, \"height\": 56 }, \"textString\": \"Join Team " + std::string(t == 0 ? "One" : "Two")
		   + "\", \"pointSize\": \"$XMediumPanelFontSize\", \"textColor\": \"$FontColorGoldYellow\", \"textHoverColor\": \"$FontColorBlue\", \"sound\": \"cursor_pass\", "
		     "\"onClickMessage\": \"PanelManager:TogglePanel:arena/LobbyJoin" + std::to_string(t + 1) + "\" } },\n";
		for (int i = 0; i < 4; ++i) {
			for (const char* kind : { "n", "gn" }) { // 2.0.1: gn = the green copy of the row for a ready player (2.3.2: was g)
				j += std::string("{ \"type\": \"TextBoxWidget\", \"name\": \"") + kind + std::to_string(t * 4 + i) + "\", \"fields\": { \"rect\": { \"x\": " + std::to_string((t == 0 ? 130 : 610))
				   + ", \"y\": " + std::to_string(256 + i * 50) + ", \"width\": 430, \"height\": 48 }, \"text\": \"" + rsv + "\", \"style\": " + (kind[0] == 'g' ? green : name) + " } },\n"; // NOLINT
			}
		}
	}
	j += RuleRows("arena/LR", 470, 68, "Teams") + ",\n"; // 2.2.0: 74 -> 68 for the 11th row (lobby box ends at y 1240) // 1.9.1: row 7 = picked / mixed teams (no stakes in team duels)
	// the trade window's name box (status) and gold box (ready count), styled like tradelayouthd.json
	j += "{ \"type\": \"TextBoxWidget\", \"name\": \"status\", \"fields\": { \"useAltStyleIfDoesntFit\": true, \"rect\": { \"x\": 224, \"y\": 1277, \"width\": 430, \"height\": 52 }, \"text\": \"" + rsv
	   + "\", \"style\": { \"pointSize\": \"$XMediumPanelFontSize\", \"spacing\": \"$MinimumSpacing\", \"alignment\": { \"h\": \"left\", \"v\": \"center\" } }, "
	     "\"altStyle\": { \"pointSize\": \"$SmallPanelFontSize\", \"spacing\": \"$MinimumSpacing\", \"alignment\": { \"h\": \"left\", \"v\": \"center\" } } } },\n";
	j += "{ \"type\": \"TextBoxWidget\", \"name\": \"readyx\", \"fields\": { \"rect\": { \"x\": 733, \"y\": 1278, \"width\": 309, \"height\": 53 }, \"text\": \"" + rsv
	   + "\", \"style\": { \"fontColor\": \"$FontColorWhite\", \"pointSize\": \"$XMediumPanelFontSize\", \"alignment\": { \"h\": \"center\", \"v\": \"center\" } } } },\n";
	j += "{ \"type\": \"ButtonWidget\", \"name\": \"guide_tab\", \"fields\": { \"rect\": { \"x\": 1108, \"y\": 711 }, \"filename\": \"PANEL\\\\Character_Sheet\\\\AdvancedStatsButton\", "
	     "\"hoveredFrame\": 3, \"pressLabelOffset\": { \"x\": 0, \"y\": 2 }, \"tooltipString\": \"Team duel guide\", \"onClickMessage\": \"PanelManager:TogglePanel:arena/LobbyGuideTab\" } },\n";
	j += "{ \"type\": \"ButtonWidget\", \"name\": \"ready_us\", \"fields\": { \"rect\": { \"x\": 103, \"y\": 1267 }, \"filename\": \"PANEL\\\\Trade\\\\AcceptButton\", "
	     "\"hoveredFrame\": 3, \"normalFrame\": 0, \"pressedFrame\": 1, \"disabledFrame\": 0, \"disabledTint\": [ 1.0, 0.0, 0.0, 1.0 ], \"tooltipString\": \"Ready\", \"tooltipOffset\": { \"x\": 0, \"y\": -3 }, "
	     "\"onClickMessage\": \"PanelManager:TogglePanel:arena/LobbyReady\" } },\n";
	j += "{ \"type\": \"ImageWidget\", \"name\": \"ready_us_mark\", \"fields\": { \"rect\": { \"x\": 103, \"y\": 1267 }, \"filename\": \"PANEL\\\\Trade\\\\AcceptButton\", \"frame\": 0, \"tint\": [ 0.0, 1.0, 0.0, 1.0 ] } },\n";
	j += "{ \"type\": \"ImageWidget\", \"name\": \"ready_us_block\", \"fields\": { \"rect\": { \"x\": 103, \"y\": 1267 }, \"filename\": \"PANEL\\\\Trade\\\\AcceptButton\", \"frame\": 0, \"tint\": [ 1.0, 0.0, 0.0, 1.0 ] } },\n";
	j += "{ \"type\": \"ButtonWidget\", \"name\": \"close\", \"fields\": { \"rect\": { \"x\": 1075, \"y\": 9 }, \"filename\": \"PANEL\\\\closebtn_4x\", "
	     "\"hoveredFrame\": 3, \"normalFrame\": 0, \"pressedFrame\": 1, \"tooltipString\": \"@d2r:strClose\", \"sound\": \"cursor_close_window_hd\", "
	     "\"onClickMessage\": \"PanelManager:TogglePanel:arena/LobbyClose\" } }\n]\n}";
	return j;
}
bool LobbyRegister(const D2RL::PluginContext* ctx, const D2RL::ResourceService* res) {
	if (!HudFinders(ctx)) {
		return false;
	}
	for (HudText& t : g_lobbyText) {
		t.reserve.clear();
		while (t.reserve.size() < kHudReserve) {
			t.reserve += "\xE2\x80\x8B";
		}
	}
	for (HudText& t : g_wagerText) { // RuleRows writes the rules page's placeholders (the same text) - also when that page is off
		if (t.reserve.empty()) {
			t.reserve = g_lobbyText[0].reserve;
		}
	}
	g_lobbyLayout = LobbyLayout();
	const D2RL::Resources::ResourceRegistration layout {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.path       = "data/global/ui/layouts/arena/Lobbyhd.json",
		.bytes      = g_lobbyLayout.c_str(),
		.byteCount  = static_cast<uint32_t>(g_lobbyLayout.size()),
	};
	D2RL::Resources::RegistrationHandle lh = D2RL::Resources::InvalidHandle;
	const auto flags = D2RL::Panels::PanelFlags::CloseOnEscape | D2RL::Panels::PanelFlags::GameplayLeftSlot;
	const D2RL::Panels::PanelRegistration panel { .structSize = D2RL::Panels::PanelRegistrationSize, .flags = flags, .localId = "Lobby" };
	if (res->registerResource(ctx, &layout, &lh) != D2RL::Resources::Result::Success || g_panels->registerPanel(ctx, &panel, &g_lobbyPanel) != D2RL::Panels::Result::Success) {
		ctx->LogWarn("arena: team duel lobby window could not be registered.");
		g_lobbyPanel = D2RL::Panels::InvalidHandle;
		return false;
	}
	g_lobbyOn = true;
	GuideInit(g_teamGuide, TeamGuidePages()); // 2.4.0: paged guides
	GuideInit(g_rulesGuide, RulesGuidePages());
	g_guideLayout = GuideLayoutFor(g_teamGuide, "Team Duel Guide"); // 1.9.2: optional - without it the side tab does nothing
	const D2RL::Resources::ResourceRegistration guide {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.path       = "data/global/ui/layouts/arena/LobbyGuidehd.json",
		.bytes      = g_guideLayout.c_str(),
		.byteCount  = static_cast<uint32_t>(g_guideLayout.size()),
	};
	D2RL::Resources::RegistrationHandle gh = D2RL::Resources::InvalidHandle;
	const D2RL::Panels::PanelRegistration guidePanel { .structSize = D2RL::Panels::PanelRegistrationSize, .flags = D2RL::Panels::PanelFlags::CloseOnEscape, .localId = "LobbyGuide" };
	if (res->registerResource(ctx, &guide, &gh) != D2RL::Resources::Result::Success || g_panels->registerPanel(ctx, &guidePanel, &g_guidePanel) != D2RL::Panels::Result::Success) {
		g_guidePanel = D2RL::Panels::InvalidHandle;
		ctx->LogWarn("arena: the team duel guide could not be registered; its side tab does nothing.");
	}
	g_rulesGuideLayout = GuideLayoutFor(g_rulesGuide, "Duel Rules Guide"); // 2.3.2 / 2.4.0
	const D2RL::Resources::ResourceRegistration rulesGuide {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.path       = "data/global/ui/layouts/arena/RulesGuidehd.json",
		.bytes      = g_rulesGuideLayout.c_str(),
		.byteCount  = static_cast<uint32_t>(g_rulesGuideLayout.size()),
	};
	D2RL::Resources::RegistrationHandle rh = D2RL::Resources::InvalidHandle;
	const D2RL::Panels::PanelRegistration rulesPanel { .structSize = D2RL::Panels::PanelRegistrationSize, .flags = D2RL::Panels::PanelFlags::CloseOnEscape, .localId = "RulesGuide" };
	if (res->registerResource(ctx, &rulesGuide, &rh) != D2RL::Resources::Result::Success || g_panels->registerPanel(ctx, &rulesPanel, &g_rulesGuidePanel) != D2RL::Panels::Result::Success) {
		g_rulesGuidePanel = D2RL::Panels::InvalidHandle;
		ctx->LogWarn("arena: the duel rules guide could not be registered; its side tab does nothing.");
	}
	return true;
}
void LobbyClosePanel(const D2RL::PluginContext* ctx) {
	ShowInvAfterWindow(ctx); // 2.4.1
	for (HudText& t : g_lobbyText) {
		HudRestoreText(t);
	}
	if (GuideOpen(ctx)) {
		(void)g_panels->closePanel(ctx, g_guidePanel); // the guide goes with the lobby
	}
	if (g_lobbyPanelOpen) {
		(void)g_panels->closePanel(ctx, g_lobbyPanel);
	}
	g_lobbyPanelOpen = false;
	g_lobbyReadyUs = g_lobbyReadyMark = g_lobbyReadyBlock = D2RL::Widgets::InvalidHandle;
	g_lobbyArrows.fill(D2RL::Widgets::InvalidHandle);
	g_lobbyUiState = -1;
	for (HudText& t : g_lobbyText) {
		t.buffer = nullptr;
	}
}
// UI thread (UiPoll). True = keep polling.
bool LobbyUiUpdate(const D2RL::PluginContext* ctx) {
	LobbyView v;
	{
		std::lock_guard lock(g_lobbyViewLock);
		v = g_lobbyView;
	}
	if (!g_lobbyOn || v.open != 1) {
		if (g_lobbyPanelOpen) {
			LobbyClosePanel(ctx);
		}
		if (!g_lobbyOn && v.open == 1) {
			{
				std::lock_guard lock(g_lobbyViewLock);
				g_lobbyView.open = 0;
			}
			SendWagerCmd(kLobbyClose, 0);
		}
		if (v.open != 1 && !v.note.empty() && v.seq != g_lobbyShownSeq) {
			g_lobbyShownSeq = v.seq; // closed with a reason (walked away, the duel started without you)
		}
		return false;
	}
	if (g_lobbyPanelOpen) { // closed by Esc or another left panel: out of the lobby
		D2RL::Panels::PanelInfo info {};
		info.structSize = D2RL::Panels::PanelInfoSize;
		if (g_panels->getPanelInfo(ctx, g_lobbyPanel, &info) == D2RL::Panels::Result::Success && info.presentationState == D2RL::Panels::PresentationState::Closed) {
			LobbyClosePanel(ctx);
			{
				std::lock_guard lock(g_lobbyViewLock);
				g_lobbyView.open = 0;
			}
			SendWagerCmd(kLobbyClose, 0);
			return false;
		}
	}
	if (!g_lobbyPanelOpen) {
		if (g_panels->openPanel(ctx, g_lobbyPanel) != D2RL::Panels::Result::Success) {
			Log("arena: team duel lobby: openPanel failed; left the lobby (see d2rloader.log).");
			{
				std::lock_guard lock(g_lobbyViewLock);
				g_lobbyView.open = 0;
			}
			SendWagerCmd(kLobbyClose, 0);
			return false;
		}
		g_lobbyPanelOpen = true;
		g_lobbyShownSeq  = 0;
		D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle;
		if (g_widgets->findPanel(ctx, kLobbyPanelName, &panel) == D2RL::Widgets::Result::Success) {
			(void)g_widgets->findWidget(ctx, panel, "ready_us", &g_lobbyReadyUs);
			(void)g_widgets->findWidget(ctx, panel, "ready_us_mark", &g_lobbyReadyMark);
			(void)g_widgets->findWidget(ctx, panel, "ready_us_block", &g_lobbyReadyBlock);
			for (int i = 0; i < kRuleRows; ++i) {
				(void)g_widgets->findWidget(ctx, panel, ("R" + std::to_string(i) + "L").c_str(), &g_lobbyArrows[i * 2]);
				(void)g_widgets->findWidget(ctx, panel, ("R" + std::to_string(i) + "M").c_str(), &g_lobbyArrows[i * 2 + 1]);
			}
		}
		Log("arena: team duel lobby open.");
	}
	if (v.seq != g_lobbyShownSeq) {
		// 2.0.1 (user 2026-11-08, screenshot 16:56: "Name - ready" ran past the column): a ready player's name goes into
		// the green box of its row (g0-g7) instead of the white one; the rules setter gets " (R)". Colour codes show as text.
		std::array<std::string, kLobbyNames> names, greens;
		const auto put = [&](int row, const LobbyView::Member& m) {
			((m.flags & 1) != 0 ? greens : names)[row] = m.name + ((m.flags & 2) != 0 ? " (R)" : "");
		};
		int                                  n[2] {}, ready = 0, onTeams = 0, drawn = 0;
		for (const LobbyView::Member& m : v.members) {
			if (m.team == kInDraw) { // 1.9.1: the draw (no team yet); 1.9.8 (user): split over both columns in turn, the draw rearranges them
				++onTeams;
				ready += (m.flags & 1) != 0 ? 1 : 0;
				if (drawn < kLobbyNames) {
					put((drawn % 2) * 4 + drawn / 2, m);
				}
				++drawn;
			} else if (m.team < 2) {
				++onTeams;
				ready += (m.flags & 1) != 0 ? 1 : 0;
				if (n[m.team] < 4) {
					put(m.team * 4 + n[m.team], m);
				}
				++n[m.team];
			}
		}
		// 1.9.2: short - the rules of the lobby are in the guide; the ready count sits where trade shows your gold
		const std::string status = !v.note.empty() ? v.note
		                         : v.mix && v.myTeam == kNoTeam ? "Mixed: join the draw"
		                         : v.mix ? "Mixed: " + std::to_string(drawn) + " in the draw"
		                         : v.myTeam == kNoTeam ? "Join a team"
		                         : std::to_string(n[0]) + " v " + std::to_string(n[1]) + (v.sizesOk ? "" : " - not allowed");
		const std::string readyx = onTeams == 0 ? std::string("Ready -") : "Ready " + std::to_string(ready) + "/" + std::to_string(onTeams);
		bool ok = HudWrite(g_lobbyText[kLbStatus], status) && HudWrite(g_lobbyText[kLbReady], readyx)
		       && HudWrite(g_lobbyText[kLbLabel1], v.rules.type == kTypeKills ? "Kills to win" : v.rules.type == kTypePoints ? "Points to win" : "Target");
		for (int i = 0; i < kLobbyNames && ok; ++i) {
			ok = HudWrite(g_lobbyText[i], names[i]) && HudWrite(g_lobbyText[kLbGreen0 + i], greens[i]);
		}
		for (int i = 0; i < kRuleRows && ok; ++i) {
			ok = HudWrite(g_lobbyText[kLbVal0 + i], i == 7 ? std::string(v.mix ? "mixed" : "picked") : RuleValue(v.rules, i));
		}
		if (ok) {
			g_lobbyShownSeq = v.seq;
		}
	}
	const bool canAgree = NowMs() >= v.agreeAt;
	const bool canReady = canAgree && v.myTeam != kNoTeam && v.sizesOk && !v.revealing;
	const int  state    = (v.myReady ? 1 : 0) | (v.leader ? 2 : 0) | (canReady ? 4 : 0) | (canAgree ? 8 : 0) | (static_cast<int>(v.seq & 0xFFFF) << 4);
	if (state != g_lobbyUiState) {
		g_lobbyUiState = state;
		const auto vis = [&](D2RL::Widgets::WidgetHandle h, bool on) {
			if (h != D2RL::Widgets::InvalidHandle) {
				(void)g_widgets->setWidgetVisible(ctx, h, on);
			}
		};
		vis(g_lobbyReadyMark, v.myReady && canAgree);
		vis(g_lobbyReadyBlock, !canAgree);
		for (int i = 0; i < kRuleRows * 2; ++i) {
			vis(g_lobbyArrows[i], v.leader && !v.revealing); // 1.9.1: row 7 switches picked / mixed teams
		}
		if (g_lobbyReadyUs != D2RL::Widgets::InvalidHandle) {
			(void)g_widgets->setWidgetEnabled(ctx, g_lobbyReadyUs, canReady);
		}
	}
	return true;
}

// ---- 1.2.0 player menu (click another player: Trade / Duel / Cancel) and the duel invite popup ----
// Both are plugin panels styled like the stock NPC dialog (npcdialogpanelhd.json: TooltipBorder nine-tile frame on 70 %
// black, $StyleNPCDialogueSize text, white options turning blue on hover). The native dialog (maybe_DIALOG_Create /
// UI_AddNpcMenuEntry, D2CLIENT_TRADE_ShowAcceptTrade) is not driven: it is a single global dialog tied to the trade UI
// state and the NPC position code.
// Click: maybe_D2CLIENT_UNITMODE_InteractWithUnit 0x1400FA180 (uint16 unitType CX, uint32 guid EDX, r8, r9); its type-0
// branch (another player, roster ok) calls D2CLIENT_TRADE_BeginNewTradeOffer - the stock trade request. Hooked at its
// 15-byte prologue (pushes + SUB RSP,0x90; the RIP-relative cookie load starts at +0xF). Trade = call the original.
constexpr char kMenuPanelName[]   = "arena/PlayerMenu";
constexpr char kInvitePanelName[] = "arena/DuelInvite";
using InteractFn   = void(__fastcall*)(uint32_t unitType, uint32_t guid, uint64_t a3, uint64_t a4);
using ClientUnitFn = void*(__fastcall*)(uint32_t guid, int32_t type);
using CheckStateFn = int32_t(__fastcall*)(void* unit, int32_t state);
std::atomic<InteractFn> g_origInteract { nullptr };
ClientUnitFn            g_clientUnit    = nullptr; // CLIENT_GetUnitByIdAndType 0x14009A5D0
CheckStateFn            g_checkState    = nullptr; // STATES_CheckState 0x1403351B0
std::atomic<uint32_t>   g_menuTarget { 0 };        // set by the click hook, UI thread opens the menu
uint64_t                g_menuA3 = 0, g_menuA4 = 0;
uint32_t                g_menuGuid = 0;           // UI thread: whom the open menu is for
bool                    g_interactPass = false;   // UI thread: calling the original for Trade
uint32_t                g_tradePending = 0;       // 1.2.3: Trade picked: the stock request goes out on the next poll, not inside the click message
D2RL::Panels::RegistrationHandle g_menuPanel   = D2RL::Panels::InvalidHandle;
D2RL::Panels::RegistrationHandle g_invitePanel = D2RL::Panels::InvalidHandle;
std::string                      g_menuLayout, g_inviteLayout;
bool                             g_menuOn = false, g_menuOpen = false, g_inviteOpen = false;
uint32_t                         g_inviteShownSeq = 0;
std::array<HudText, 3> g_inviteText { HudText { "Title", kInvitePanelName }, HudText { "Text", kInvitePanelName }, HudText { "Text2", kInvitePanelName } };

void __fastcall ArenaInteract(uint32_t unitType, uint32_t guid, uint64_t a3, uint64_t a4) {
	const InteractFn orig = g_origInteract.load(std::memory_order_acquire);
	if (!g_interactPass && g_menuOn && (unitType & 0xFFFF) == 0 && g_localPlayer != nullptr) {
		void* me     = g_localPlayer(g_localIndex());
		void* target = g_clientUnit(guid, 0);
		if (me != nullptr && target != nullptr && target != me && g_checkState(target, 7) == 0) { // state 7 = dead: corpse, stock path
			g_menuA3 = a3, g_menuA4 = a4;
			g_menuTarget = guid;
			StartUiPoll();
			return;
		}
	}
	orig(unitType, guid, a3, a4);
}

auto DialogLayout(const char* name, const std::vector<std::pair<const char*, std::string>>& texts,
                  const std::vector<std::pair<const char*, const char*>>& buttons, int w = 640) -> std::string {
	const int rowH = 64;
	const int h    = 48 + static_cast<int>(texts.size()) * 60 + static_cast<int>(buttons.size()) * rowH;
	std::string j  = std::string("{\n\"type\": \"Panel\", \"name\": \"") + name + "\",\n\"fields\": { \"anchor\": { \"x\": 0.5, \"y\": 0.5 }, \"priority\": 20 },\n\"children\": [\n"
	             + "{ \"type\": \"NineTileImageWidget\", \"name\": \"DialogBorder\", \"fields\": { \"rect\": { \"x\": " + std::to_string(-w / 2) + ", \"y\": -360, \"width\": "
	             + std::to_string(w) + ", \"height\": " + std::to_string(h) + " }, \"filename\": \"PANEL/TooltipBorder/TooltipBorder\", \"backgroundColor\": [ 0.0, 0.0, 0.0, 0.70 ], \"tileScale\": 1.0 } }";
	int y = -360 + 24;
	for (const auto& [id, reserve] : texts) {
		j += ",\n{ \"type\": \"TextBoxWidget\", \"name\": \"" + std::string(id) + "\", \"fields\": { \"rect\": { \"x\": " + std::to_string(-w / 2 + 24) + ", \"y\": "
		     + std::to_string(y) + ", \"width\": " + std::to_string(w - 48) + ", \"height\": 56 }, \"text\": \"" + reserve
		     + "\", \"style\": { \"fontColor\": \"$FontColorGoldYellow\", \"pointSize\": \"$StyleNPCDialogueSize\", \"alignment\": { \"h\": \"center\", \"v\": \"center\" } } } }";
		y += 60;
	}
	for (const auto& [label, action] : buttons) {
		j += ",\n{ \"type\": \"ButtonWidget\", \"name\": \"" + std::string(action) + "\", \"fields\": { \"rect\": { \"x\": " + std::to_string(-w / 2 + 24) + ", \"y\": "
		     + std::to_string(y) + ", \"width\": " + std::to_string(w - 48) + ", \"height\": " + std::to_string(rowH - 4) + " }, \"textString\": \"" + label
		     + "\", \"pointSize\": \"$StyleNPCDialogueSize\", \"textColor\": \"$FontColorWhite\", \"textHoverColor\": \"$FontColorBlue\", \"sound\": \"cursor_pass\", "
		       "\"onClickMessage\": \"PanelManager:TogglePanel:arena/" + action + "\" } }";
		y += rowH;
	}
	return j + "\n]\n}";
}

bool MenuRegister(const D2RL::PluginContext* ctx, const D2RL::ResourceService* res) {
	if (!HudFinders(ctx)) {
		return false;
	}
	for (HudText& t : g_inviteText) {
		t.reserve.clear();
		while (t.reserve.size() < kHudReserve) {
			t.reserve += "\xE2\x80\x8B";
		}
	}
	g_menuLayout   = DialogLayout(kMenuPanelName, {}, { { "Trade", "PMTrade" }, { "Duel", "PMDuel" }, { "Cancel", "PMCancel" } });
	g_inviteLayout = DialogLayout(kInvitePanelName, { { "Title", g_inviteText[0].reserve }, { "Text", g_inviteText[1].reserve }, { "Text2", g_inviteText[2].reserve } },
	                              { { "Accept", "DIAccept" }, { "Decline", "DIDecline" } }, 1100); // 1.2.1: wide enough for long names
	const auto reg = [&](const char* path, const std::string& layout, const char* localId, D2RL::Panels::RegistrationHandle* out) {
		const D2RL::Resources::ResourceRegistration r {
			.structSize = D2RL::Resources::ResourceRegistrationSize,
			.path       = path,
			.bytes      = layout.c_str(),
			.byteCount  = static_cast<uint32_t>(layout.size()),
		};
		D2RL::Resources::RegistrationHandle lh = D2RL::Resources::InvalidHandle;
		const D2RL::Panels::PanelRegistration panel { .structSize = D2RL::Panels::PanelRegistrationSize, .flags = D2RL::Panels::PanelFlags::CloseOnEscape, .localId = localId };
		return res->registerResource(ctx, &r, &lh) == D2RL::Resources::Result::Success && g_panels->registerPanel(ctx, &panel, out) == D2RL::Panels::Result::Success;
	};
	if (!reg("data/global/ui/layouts/arena/PlayerMenuhd.json", g_menuLayout, "PlayerMenu", &g_menuPanel)
	    || !reg("data/global/ui/layouts/arena/DuelInvitehd.json", g_inviteLayout, "DuelInvite", &g_invitePanel)) {
		ctx->LogWarn("arena: player menu / duel invite panels could not be registered; use the console commands.");
		return false;
	}
	return true;
}

auto InvitePopupUp() -> bool {
	return g_inviteOpen;
}
void MenuClose(const D2RL::PluginContext* ctx) {
	if (g_menuOpen) {
		(void)g_panels->closePanel(ctx, g_menuPanel);
	}
	g_menuOpen = false;
	g_menuGuid = 0;
}
void InviteClose(const D2RL::PluginContext* ctx) {
	for (HudText& t : g_inviteText) {
		HudRestoreText(t);
	}
	if (g_inviteOpen) {
		(void)g_panels->closePanel(ctx, g_invitePanel);
	}
	g_inviteOpen = false;
	for (HudText& t : g_inviteText) {
		t.buffer = nullptr;
	}
}
auto PanelClosedByGame(const D2RL::PluginContext* ctx, D2RL::Panels::RegistrationHandle h) -> bool {
	D2RL::Panels::PanelInfo info {};
	info.structSize = D2RL::Panels::PanelInfoSize;
	return g_panels->getPanelInfo(ctx, h, &info) == D2RL::Panels::Result::Success && info.presentationState == D2RL::Panels::PresentationState::Closed;
}

// 2.4.10 (IV14: the host never saw the stock trade open or close): while this client has the wager flag, watch its own
// trade ids (read in Ghidra 2026-11-09: D2CLIENT_TRADE_OnTradeEventCanceled 0x1401EDBA0 compares the packet id with
// [0x142A96AB8] (accepted id) else [0x142A96AB0] (offer id); D2CLIENT_TRADE_ResetTradeState 0x1401F32C0 zeroes both).
// Seen non-zero, then both zero while the flag is still on (a completed exchange turns the flag off first) = the popup
// was declined or the window cancelled: tell the host once.
constexpr uint64_t kRvaTradeOfferId = 0x2A96AB0, kRvaTradeAcceptedId = 0x2A96AB8;
bool g_stakeTradeSeen = false;
// 2.4.11: D2CLIENT_TRADE_CancelAny 0x1401ED700 (read in Ghidra 2026-11-09): void(void); returns at once when both trade ids
// are 0, else sends the stock 0x2C cancel for the open id (the server closes the popup / window as a cancel).
// Prologue 48 81 EC 68 02 00 00 (sub rsp,0x268), then 48 8B 05 rel32 (cookie, RIP-relative - not compared).
constexpr uint64_t kRvaTradeCancelAny = 0x1ED700;
constexpr uint8_t  kTradeCancelAny[] = { 0x48, 0x81, 0xEC, 0x68, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x05 };
auto StakeTradeWatch(const D2RL::PluginContext* ctx) -> bool {
	if (g_closeTradePending.exchange(false)) {
		static const bool ok = std::memcmp(reinterpret_cast<const void*>(ctx->exeBase + kRvaTradeCancelAny), kTradeCancelAny, sizeof(kTradeCancelAny)) == 0;
		if (ok) {
			reinterpret_cast<void(__fastcall*)()>(ctx->exeBase + kRvaTradeCancelAny)();
		} else {
			ctx->LogWarn("arena: trade cancel function bytes differ; the stake trade window is not closed by the plugin.");
		}
		g_stakeTradeSeen = false;
	}
	const uint64_t until = g_wagerLabelsUntil.load(std::memory_order_relaxed);
	if (until == 0 || NowMs() >= until) {
		g_stakeTradeSeen = false;
		return false;
	}
	const auto offer    = *reinterpret_cast<const volatile int32_t*>(ctx->exeBase + kRvaTradeOfferId);
	const auto accepted = *reinterpret_cast<const volatile int32_t*>(ctx->exeBase + kRvaTradeAcceptedId);
	if (offer != 0 || accepted != 0) {
		g_stakeTradeSeen = true;
	} else if (g_stakeTradeSeen) {
		g_stakeTradeSeen = false;
		SendWagerCmd(kStakeTradeClosed, 0);
	}
	return true;
}

// UI thread (UiPoll). True = keep polling.
bool MenuUiUpdate(const D2RL::PluginContext* ctx) {
	if (!g_menuOn) {
		return false;
	}
	bool more = false;
	if (const uint32_t g = g_tradeStartGuid.exchange(0); g != 0) { // 1.4.0: the wager trade request
		g_tradePending = g;
		g_menuA3 = g_menuA4 = 0;
	}
	if (const uint32_t guid = std::exchange(g_tradePending, 0u); guid != 0) {
		if (const InteractFn orig = g_origInteract.load(std::memory_order_acquire); orig != nullptr) {
			g_interactPass = true; // the stock click: trade request (D2CLIENT_TRADE_BeginNewTradeOffer)
			orig(0, guid, g_menuA3, g_menuA4);
			g_interactPass = false;
		}
	}
	if (const uint32_t t = g_menuTarget.exchange(0); t != 0) {
		MenuClose(ctx);
		if (g_panels->openPanel(ctx, g_menuPanel) == D2RL::Panels::Result::Success) {
			g_menuOpen = true;
			g_menuGuid = t;
		}
	}
	if (g_menuOpen) {
		if (PanelClosedByGame(ctx, g_menuPanel)) { // Esc / another panel
			g_menuOpen = false;
			g_menuGuid = 0;
		} else {
			more = true;
		}
	}
	InviteView v;
	{
		std::lock_guard lock(g_inviteLock);
		v = g_inviteView;
	}
	const bool want = v.until != 0 && NowMs() < v.until;
	if (!want) {
		if (g_inviteOpen) {
			InviteClose(ctx);
		}
		return more;
	}
	if (g_inviteOpen && PanelClosedByGame(ctx, g_invitePanel)) { // Esc = decline
		g_inviteOpen = false;
		InviteClose(ctx);
		{
			std::lock_guard lock(g_inviteLock);
			g_inviteView.until = 0;
		}
		(void)SendDuelCmd(kDuelDecline, "");
		return more;
	}
	if (!g_inviteOpen) {
		if (g_panels->openPanel(ctx, g_invitePanel) != D2RL::Panels::Result::Success) {
			return true;
		}
		g_inviteOpen     = true;
		g_inviteShownSeq = 0;
	}
	// the countdown lives here (1.2.1: the top notice is hidden while the popup is up); rewritten each second
	const uint32_t left = static_cast<uint32_t>((v.until - NowMs() + 999) / 1000);
	if (v.seq * 1000 + left != g_inviteShownSeq) {
		const std::string title = v.from + " challenges you to a duel (" + std::to_string(left) + ")";
		const std::string text  = v.panel ? std::string("A duel with rules set by the challenger.") : "First to " + std::to_string(g_pvp.duelPoints) + " points.";
		const std::string text2 = v.panel ? "The rules and stakes are set after you accept."
		                         : v.stake > 0 ? "For " + WagerGold(v.stake) + " gold each." : std::string();
		if (HudWrite(g_inviteText[0], title) && HudWrite(g_inviteText[1], text) && HudWrite(g_inviteText[2], text2)) {
			g_inviteShownSeq = v.seq * 1000 + left;
		}
	}
	return true;
}

// The player menu's choices and the popup's answers (UI messages, UI thread). True = consumed.
bool MenuUiMessage(const D2RL::PluginContext* ctx, const char* text) {
	if (std::strcmp(text, "arena/PMTrade") == 0) {
		// 1.2.3: called from here (inside the PanelManager message) the stock request opened an empty right panel frame
		// next to its "Asking other player to trade" box (screenshot 2026-11-07 22:43): close the menu first, request next poll
		g_tradePending = g_menuGuid;
		(void)SendDuelCmd(kDuelPlainTrade, "#" + std::to_string(g_menuGuid)); // clears a leftover wager mark first
		MenuClose(ctx);
		StartUiPoll();
		return true;
	}
	if (std::strcmp(text, "arena/PMDuel") == 0) {
		const uint32_t guid = g_menuGuid;
		MenuClose(ctx);
		if (guid != 0) {
			const std::string err = SendDuelCmd(g_wagerOn ? kDuelWager : kDuelChallenge, "#" + std::to_string(guid)); // 1.6.0: popup -> rules page -> (trade)
			if (!err.empty()) {
				Log("arena: player menu: duel not sent (%s).", err.c_str());
			}
		}
		return true;
	}
	if (std::strcmp(text, "arena/PMCancel") == 0) {
		MenuClose(ctx);
		return true;
	}
	if (std::strcmp(text, "arena/DIAccept") == 0 || std::strcmp(text, "arena/DIDecline") == 0) {
		const bool accept = std::strcmp(text, "arena/DIAccept") == 0;
		{
			std::lock_guard lock(g_inviteLock);
			g_inviteView.until = 0;
		}
		InviteClose(ctx);
		{
			std::lock_guard lock(g_noticeLock);
			if (g_noticeText.find("challenges you to") != std::string::npos) {
				g_noticeUntil = 0;
			}
		}
		(void)SendDuelCmd(accept ? kDuelAccept : kDuelDecline, "");
		return true;
	}
	return false;
}

void InstallPlayerMenu(const D2RL::PluginContext* ctx, const D2RL::MutationService* mut) {
	if (!g_pvp.duelEnabled || !g_pvp.playerMenu || !g_menuOn) {
		g_menuOn = false;
		return;
	}
	g_menuOn = false;
	static constexpr uint64_t kRvaInteract   = 0xfa180;  // maybe_D2CLIENT_UNITMODE_InteractWithUnit
	static constexpr uint64_t kRvaClientUnit = 0x9a5d0;  // CLIENT_GetUnitByIdAndType
	static constexpr uint64_t kRvaCheckState = 0x3351b0; // STATES_CheckState
	static constexpr uint8_t  kInteract[]   = { 0x40, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00 };
	static constexpr uint8_t  kClientUnit[] = { 0x4C, 0x63, 0xCA, 0x48, 0x8D, 0x05, 0x36, 0x93, 0x98, 0x02 };
	static constexpr uint8_t  kCheckState[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
	if (!ctx->CheckExpectedBytes(kRvaClientUnit, kClientUnit, sizeof(kClientUnit)) || !ctx->CheckExpectedBytes(kRvaCheckState, kCheckState, sizeof(kCheckState))) {
		ctx->LogWarn("arena: player menu off: unexpected bytes at the client unit / state lookups.");
		return;
	}
	g_clientUnit = reinterpret_cast<ClientUnitFn>(ctx->exeBase + kRvaClientUnit);
	g_checkState = reinterpret_cast<CheckStateFn>(ctx->exeBase + kRvaCheckState);
	D2RL::Mutations::TransactionHandle tx = D2RL::Mutations::InvalidTransactionHandle;
	if (mut->beginTransaction(ctx, &tx) != D2RL::Mutations::Result::Success) {
		ctx->LogWarn("arena: player menu off: transaction failed.");
		return;
	}
	const D2RL::Mutations::InlineHookRequest hook {
		.structSize   = D2RL::Mutations::InlineHookRequestSize,
		.rva          = kRvaInteract,
		.expected     = kInteract,
		.expectedSize = static_cast<uint32_t>(sizeof(kInteract)),
		.target       = reinterpret_cast<void*>(&ArenaInteract),
	};
	D2RL::Mutations::OperationHandle op = D2RL::Mutations::InvalidOperationHandle;
	if (mut->stageInlineHook(ctx, tx, &hook, &op) != D2RL::Mutations::Result::Success) {
		mut->cancelTransaction(ctx, tx);
		ctx->LogWarn("arena: player menu off: InteractWithUnit hook could not be staged (another plugin?).");
		return;
	}
	D2RL::Mutations::CommitResult result { .structSize = D2RL::Mutations::CommitResultSize };
	void* original = nullptr;
	if (mut->commit(ctx, tx, &result) != D2RL::Mutations::Result::Success || result.result != D2RL::Mutations::Result::Success
	    || mut->getInlineHookOriginal(ctx, tx, op, &original) != D2RL::Mutations::Result::Success || original == nullptr) {
		ctx->LogWarn("arena: player menu off: hook commit failed.");
		return;
	}
	g_origInteract.store(reinterpret_cast<InteractFn>(original), std::memory_order_release);
	g_menuOn = true;
	ctx->LogInfo("arena: player menu on (click another player in town: Trade / Duel).");
}

auto __cdecl OnUiMessage(const D2RL::PluginContext* ctx, const D2RL::SharedEvents::UiMessageEvent* ev, void*) noexcept -> D2RL::SharedEvents::UiMessageAction {
	if (ctx == nullptr || ev == nullptr || ev->target == nullptr || ev->command == nullptr || ev->text == nullptr
	    || std::strcmp(ev->target, "PanelManager") != 0 || std::strcmp(ev->command, "TogglePanel") != 0) {
		return D2RL::SharedEvents::UiMessageAction::Continue;
	}
	if (MenuUiMessage(ctx, ev->text)) {
		return D2RL::SharedEvents::UiMessageAction::Consume;
	}
	if (std::strncmp(ev->text, "arena/WR", 8) == 0 && ev->text[8] >= '0' && ev->text[8] < '0' + kRuleRows && (ev->text[9] == 'L' || ev->text[9] == 'M')) {
		SendWagerCmd(kWagerRule, static_cast<uint32_t>(ev->text[8] - '0') | (ev->text[9] == 'M' ? 0x100u : 0u)); // 1.6.0: a rule arrow
		return D2RL::SharedEvents::UiMessageAction::Consume;
	}
	if (std::strncmp(ev->text, "arena/LR", 8) == 0 && ev->text[8] >= '0' && ev->text[8] < '0' + kRuleRows && (ev->text[9] == 'L' || ev->text[9] == 'M')) {
		SendWagerCmd(kLobbyRule, static_cast<uint32_t>(ev->text[8] - '0') | (ev->text[9] == 'M' ? 0x100u : 0u)); // 1.9.0: lobby rule arrow
		return D2RL::SharedEvents::UiMessageAction::Consume;
	}
	if (std::strcmp(ev->text, "arena/LobbyJoin1") == 0 || std::strcmp(ev->text, "arena/LobbyJoin2") == 0) {
		SendWagerCmd(kLobbyJoin, ev->text[15] == '2' ? 1u : 0u);
		return D2RL::SharedEvents::UiMessageAction::Consume;
	}
	// 2.3.3: the tabs send a message that is NOT the guide panel's own name (as smithing's "smithing/Book" for the panel
	// "smithing/RecipeBook"). With the panel's name D2RCore tried to open the plugin panel itself and failed ("Plugin panel
	// open failed ... panel-message-consumed=yes", 2026-11-08) - the guide never opened.
	if (std::strcmp(ev->text, "arena/LobbyGuideTab") == 0 || std::strcmp(ev->text, "arena/RulesGuideTab") == 0) { // 1.9.2 / 2.3.2: side tabs
		// 2.3.2: when the panel fails to open, D2RCore hands the same toggle message back to us; toggling again from inside
		// looped until the stack overflowed (crash 2026-11-08 20:50, 1611 "Plugin panel open failed" lines). Re-entry = ignore.
		static bool s_toggling = false;
		const bool  lobby = ev->text[6] == 'L';
		const D2RL::Panels::RegistrationHandle h = lobby ? g_guidePanel : g_rulesGuidePanel;
		if (!s_toggling && h != D2RL::Panels::InvalidHandle) {
			GuideBook& g = lobby ? g_teamGuide : g_rulesGuide;
			if (!(lobby ? GuideOpen(ctx) : RulesGuideOpen(ctx))) {
				g.page = 0, g.shown = -1; // 2.4.0: opens on page 1
			}
			s_toggling = true;
			(void)g_panels->togglePanel(ctx, h);
			s_toggling = false;
			StartUiPoll(); // the poll writes the page
		}
		return D2RL::SharedEvents::UiMessageAction::Consume;
	}
	for (GuideBook* g : { &g_teamGuide, &g_rulesGuide }) { // 2.4.0: page arrows
		const std::string prev = std::string("arena/") + g->tag + "Prev", next = std::string("arena/") + g->tag + "Next";
		if (prev == ev->text || next == ev->text) {
			g->page = std::clamp(g->page + (next == ev->text ? 1 : -1), 0, std::max(0, static_cast<int>(g->pages.size()) - 1));
			StartUiPoll();
			return D2RL::SharedEvents::UiMessageAction::Consume;
		}
	}
	if (std::strcmp(ev->text, "arena/LobbyReady") == 0) {
		SendWagerCmd(kLobbyReady, 0);
		return D2RL::SharedEvents::UiMessageAction::Consume;
	}
	if (std::strcmp(ev->text, "arena/LobbyClose") == 0) {
		{
			std::lock_guard lock(g_lobbyViewLock);
			g_lobbyView.open = 0;
		}
		LobbyClosePanel(ctx);
		SendWagerCmd(kLobbyClose, 0);
		return D2RL::SharedEvents::UiMessageAction::Consume;
	}
	if (std::strcmp(ev->text, "arena/WagerReady") == 0) {
		SendWagerCmd(kWagerReady, 0);
		return D2RL::SharedEvents::UiMessageAction::Consume;
	}
	if (std::strcmp(ev->text, "arena/WagerClose") == 0) {
		{
			std::lock_guard lock(g_wagerViewLock);
			g_wagerView.open = 0;
		}
		WagerClosePanel(ctx);
		SendWagerCmd(kWagerClose, 0);
		return D2RL::SharedEvents::UiMessageAction::Consume;
	}
	return D2RL::SharedEvents::UiMessageAction::Continue;
}

// UI thread (UiPoll): the open trade window's title. True = keep polling.
bool TradeTitleUpdate() {
	if (g_setTextNative == nullptr || g_findTopLevelPanel == nullptr || g_findChildByName == nullptr) {
		return false;
	}
	const bool on    = WagerLabelsOn();
	auto*      panel = static_cast<uint8_t*>(g_findTopLevelPanel("TradeLayout"));
	const bool open  = panel != nullptr && panel[0x51] != 0;
	if (open && on != g_tradeTitleSet) {
		if (void* title = g_findChildByName(panel, "Title"); title != nullptr) {
			const char* text = on ? "duel wager" : g_origStringById.load(std::memory_order_acquire)(0xD44);
			const struct {
				const char* data;
				size_t      len;
			} str { text, std::strlen(text) };
			g_setTextNative(title, &str);
			g_tradeTitleSet = on;
		}
		if (g_assignStr != nullptr) {
			if (void* ready = g_findChildByName(panel, "ready_us"); ready != nullptr) {
				const char* tip = on ? "Accept Wager" : g_origStringById.load(std::memory_order_acquire)(0x1023);
				// 1.5.2: ButtonWidget_OnLoad 0x14086F830 copies tooltipString (+0x88) to +0xAE0 (the enabled-state tooltip);
				// TradePanel_Update enables ready_us every update, which brings +0xAE0 back - so both strings
				g_assignStr(static_cast<uint8_t*>(ready) + 0x88, tip, std::strlen(tip));
				g_assignStr(static_cast<uint8_t*>(ready) + 0xAE0, tip, std::strlen(tip));
			}
		}
	}
	// (1.5.1: no reset while closed - a set title / tooltip is put back on the next open, kept window or not)
	return on;
}

void __cdecl UiPoll(const D2RL::PluginContext* ctx, void*) noexcept {
	g_uiPolling = false;
	if (ctx == nullptr || g_localPlayer == nullptr) {
		return;
	}
	void*          me   = g_localPlayer(g_localIndex());
	const uint32_t guid = me != nullptr ? Field<uint32_t>(me, 0x08) : 0;
	bool           more = false;
	// duel: answer the host once this client is past its loading screen and stands in the duel level. The level test
	// covers the time before the unload packet 0x05 arrives, when the loaded flag is still set from the old act.
	if (const int32_t loadLevel = g_duelLoadLevel.load(); loadLevel != 0) {
		bool loaded = (g_clientGameLoaded == nullptr || *g_clientGameLoaded != 0) && me != nullptr && LevelOf(me) == loadLevel;
		bool done   = NowMs() >= g_duelLoadUntil.load();
		// 0.8.3: the load signal (packet 0x04) comes before the HD assets. An act-changing client also waits for its
		// LoadScreenPanel (the multiplayer loading screen with the bar) to open and close again; never seen within
		// loading_screen_wait_seconds = answer with the plain signal, the host then uses the fixed wait.
		uint8_t ack = 1;
		if (g_duelExpectScreen.load() && g_pvp.duelScreenCheck && g_findTopLevelPanel != nullptr) {
			const auto* screen  = static_cast<const uint8_t*>(g_findTopLevelPanel("LoadScreenPanel"));
			const bool  visible = screen != nullptr && screen[0x51] != 0;
			if (visible && !g_duelScreenSeen) {
				g_duelScreenSeen = true;
				Log("arena: loading screen seen (duel level %d).", loadLevel);
			}
			if (visible) {
				loaded = false;
			} else if (g_duelScreenSeen) {
				ack = 2;
			} else if (NowMs() < g_duelScreenUntil.load()) {
				loaded = false;
			} else if (!g_duelScreenMissLogged) {
				g_duelScreenMissLogged = true;
				Log("arena: loading screen not seen within %.0f s; answering with the load signal.", g_pvp.duelScreenWait);
			}
		}
		if (loaded && !done) {
			bool host = false;
			{
				std::lock_guard lock(g_pvpLock);
				host = g_game != nullptr;
				if (host) {
					g_duelLoaded[guid] = ack;
				}
			}
			done = host || (g_net != nullptr && g_net->sendToHost(ctx, g_channel, kMsgDuelLoaded, &ack, sizeof(ack)) == D2RL::Network::Result::Success);
			if (done) {
				Log("arena: this client finished loading duel level %d%s.", loadLevel, ack == 2 ? " (loading screen closed)" : "");
			}
		}
		if (done) {
			g_duelLoadLevel      = 0;
			g_duelExpectScreen   = false;
			g_duelScreenSeen     = false;
			g_duelScreenMissLogged = false;
		} else {
			more = true;
		}
	}
	// hello: tell the host which player this client is (for the countdown message)
	if (g_net != nullptr && guid != 0 && g_helloGuid.load() != guid) {
		D2RL::Network::ChannelInfo info { .structSize = D2RL::Network::ChannelInfoSize };
		const bool connected = g_net->getChannelInfo(ctx, g_channel, &info) == D2RL::Network::Result::Success
		                    && info.connectionState == D2RL::Network::ConnectionState::Connected;
		if (connected && g_net->sendToHost(ctx, g_channel, kMsgHello, &guid, sizeof(guid)) == D2RL::Network::Result::Success) {
			g_helloGuid = guid;
		} else {
			if (!connected && g_uiPolls % 50 == 0) {
				(void)g_net->connectToHost(ctx, g_channel);
			}
			more = g_uiPolls < 5000; // keep trying for a while
		}
	}
	// the hosting process's own player
	if (guid != 0 && g_localDownGuid.load() == guid) {
		const uint64_t until = g_localDownUntil.load();
		if (until > g_countdownUntil.load()) {
			g_countdownUntil = until;
		}
	} else if (guid != 0 && g_localDownGuid.load() == 0 && g_countdownUntil.load() != 0 && g_net == nullptr) {
		g_countdownUntil = 0;
	}
	if (guid != 0 && g_localReturnGuid.load() == guid) { // the hosting process's own player is the last one
		g_returnUntil = g_localReturnUntil.load();
		g_localReturnGuid = 0;
	}
	const uint64_t until = g_countdownUntil.load();
	const uint64_t ret   = g_returnUntil.load();
	const uint64_t now   = NowMs();
	if (const uint64_t res = g_duelResultUntil.load(); res != 0 && now < res) {
		const int n = std::clamp(static_cast<int>((res - now + 999) / 1000), 1, 99);
		if (g_hudOn) {
			HudShow(ctx, (g_duelResultKind.load() == 2 ? kHudLoseBase : kHudWinBase) + n);
		}
		more = true;
	} else if (until > now) {
		const int k = std::clamp(static_cast<int>((until - now + 999) / 1000), 1, kCountdownPanels);
		ShowCountdown(ctx, k);
		g_counting = true;
		more       = true;
	} else if (const uint64_t dcd = g_duelCdUntil.load(); dcd != 0 && now < dcd + 1000) { // duel start (HUD only), then "Fight!" 1 s
		if (g_hudOn) {
			HudShow(ctx, now < dcd ? kHudDuelBase + std::clamp(static_cast<int>((dcd - now + 999) / 1000), 1, 99) : 1);
		}
		more = true;
	} else if (ret > now) { // return-to-town countdown (HUD only; the chat line is the notice in window style)
		if (g_counting) {
			ShowCountdown(ctx, 0);
			g_counting       = false;
			g_countdownUntil = 0;
		}
		if (g_hudOn) {
			HudShow(ctx, kHudReturnBase + std::clamp(static_cast<int>((ret - now + 999) / 1000), 1, 99));
		}
		more = true;
	} else {
		g_returnUntil = 0;
		g_duelCdUntil = 0;
		if (const uint64_t su = g_suddenUntil.load(); su != 0 && now >= su) { // 1.9.7: the start banner is over
			g_suddenUntil = 0;
			int32_t start = g_suddenKind.load();
			if (start != kSuddenOut) {
				g_suddenKind.compare_exchange_strong(start, 0);
			}
		}
		if (const int32_t sd = g_suddenKind.load(); sd != 0 && g_hudOn) {
			HudShow(ctx, kHudSuddenBase + sd); // 1.9.3: lowest priority, under the down / result texts
			more = true;
		} else {
			ShowCountdown(ctx, 0);
		}
		if (g_counting && g_getUi != nullptr && g_getUi(kUiStateYouDied) != 0) {
			g_closeUi(kUiStateYouDied, 0, 0, 0); // the "You have died" screen, if it opened anyway
		}
		g_counting       = false;
		g_countdownUntil = 0;
	}
	more |= ScoreUpdate(ctx, guid);
	more |= WagerUiUpdate(ctx);
	more |= LobbyUiUpdate(ctx);
	if (g_lobbyPanelOpen || g_wagerPanelOpen) { // 2.4.1: keep the inventory / side frame hidden (re-check now and then)
		HideInvForWindow(ctx, true); // 2.4.4: every poll (the game re-shows the right frame)
	}
	more |= InvRestorePoll(ctx); // 2.4.3
	more |= StakeTradeWatch(ctx); // 2.4.10
	more |= GuideUiUpdate(g_teamGuide, GuideOpen(ctx));       // 2.4.0
	more |= GuideUiUpdate(g_rulesGuide, RulesGuideOpen(ctx));
	more |= MenuUiUpdate(ctx);
	more |= TradeTitleUpdate();
	// 2.4.20 (RF10 2026-11-14: 2.4.17 hid the inventory itself for the popup and 2.4.18/2.4.19 showed it again on the
	// trade window - on the challenger that drew a dead inventory (interface state 0: hover shows the ground, clicks walk)).
	// Now the popup hides only the two right-border pieces; they come back when the inventory opens, or when the trade is
	// accepted (TradeLayout visible + accepted trade id +0x2A96AB8, both seen in the RF10 log). The inventory is untouched.
	if (g_borderRestorePending && g_findTopLevelPanel != nullptr) {
		const auto* trade    = static_cast<const uint8_t*>(g_findTopLevelPanel("TradeLayout"));
		const bool  tradeOn  = trade != nullptr && trade[0x51] != 0
		                     && *reinterpret_cast<const volatile int32_t*>(ctx->exeBase + kRvaTradeAcceptedId) != 0;
		// 2.4.26 (user 2026-11-14: the frame came back on the client's stake popup): no "inventory open" trigger any more -
		// one game still counts its inventory as open at the stake start (RF13 log), which put the border back ~15 ms after
		// the hide. Now only the accepted trade window or the end of the stake trade brings it back.
		const bool  invOn    = false;
		const bool  noTrade  = WagerLabelsOn() == false; // the stake trade is over (cancelled / done): put them back
		if ((tradeOn || invOn || noTrade) && SetRightBorderVisible(ctx, true)) {
			g_borderRestorePending = false;
			if (g_pvp.diagnostics) {
				Log("arena: right border shown again (%s).", tradeOn ? "trade accepted" : invOn ? "inventory open" : "stake trade over");
			}
		} else {
			more = true;
		}
	}
	if (g_closeInvPending.exchange(false) && g_getUi != nullptr && g_closeUi != nullptr && g_getUi(kUiStateInventory) != 0) {
		g_closeUi(kUiStateInventory, 0, 0, 0); // as D2CLIENT_TRADE_BeginNewTradeOffer does: UI_CloseInterfaceState(1, 0)
	}
	if (g_stakeInvCheck.exchange(false) && g_getUi != nullptr) { // 2.4.14
		const int inv = g_getUi(kUiStateInventory);
		if (g_pvp.diagnostics) { // 2.4.15: test log only
			Log("arena: stake trade: inventory state %d, hidden by window %d, restore pending %d, rules window open %d.", inv,
			    g_invHiddenByWindow ? 1 : 0, g_invRestorePending ? 1 : 0, g_wagerPanelOpen ? 1 : 0);
		}
		// 2.4.22 (RF12 2026-11-14: the challenger's trade window still had no inventory; this close fired at the stake start
		// on one game - the requester's trade flow uses state 1): the 2.4.14 close is removed, the state is only logged.
		(void)inv;
		// 2.4.17 (RF4: the frame still showed once on the client, first round): 2.4.20 hides only the right border pieces
		// for the popup (see above), not the inventory.
		if (SetRightBorderVisible(ctx, false)) {
			g_borderRestorePending = true;
		}
	}
	if (const uint64_t cd = g_pendingSkillCd.exchange(0); cd != 0 && g_cltSetCd != nullptr && me != nullptr) {
		g_cltSetCd(me, static_cast<int32_t>(cd & 0xFFFFFFFF), static_cast<int32_t>(cd >> 32)); // 1.7.0: the icon + client gate
	}
	more |= NoticeUpdate(ctx);
	if (guid != 0) {
		g_localGuid = guid;
	}
	++g_uiPolls;
	if (more) {
		StartUiPoll();
	} else {
		g_uiPolls = 0;
	}
}

// Wagers (own transaction; without the hook a leaver keeps their stake, so wagers are then off).
void InstallWager(const D2RL::PluginContext* ctx, const D2RL::MutationService* mut) {
	if (!g_pvp.wagerEnabled) {
		return;
	}
	static constexpr uint64_t kRvaSaveAll   = 0x41e0d0; // PLRSAVE_SaveAllPlayers(game, player, ...): 16 bytes, no RIP-relative, no branch
	static constexpr uint64_t kRvaSaveStamp = 0x406e10; // GAME_SaveAllPlayersAndStampTime(game)
	static constexpr uint8_t  kSaveAll[]   = { 0x48, 0x8B, 0xC4, 0x53, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x68, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9 };
	static constexpr uint8_t  kSaveStamp[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xD2, 0x48, 0x8B, 0xD9, 0xE8 };
	g_pvp.wagerEnabled = false;
	if (!ctx->CheckExpectedBytes(kRvaSaveStamp, kSaveStamp, sizeof(kSaveStamp))) {
		ctx->LogWarn("arena: wagers off: unexpected bytes at GAME_SaveAllPlayersAndStampTime.");
		return;
	}
	D2RL::Mutations::TransactionHandle tx = D2RL::Mutations::InvalidTransactionHandle;
	if (mut->beginTransaction(ctx, &tx) != D2RL::Mutations::Result::Success) {
		ctx->LogWarn("arena: wagers off: transaction failed.");
		return;
	}
	const D2RL::Mutations::InlineHookRequest hook {
		.structSize   = D2RL::Mutations::InlineHookRequestSize,
		.rva          = kRvaSaveAll,
		.expected     = kSaveAll,
		.expectedSize = static_cast<uint32_t>(sizeof(kSaveAll)),
		.target       = reinterpret_cast<void*>(&ArenaSaveAll),
	};
	D2RL::Mutations::OperationHandle op = D2RL::Mutations::InvalidOperationHandle;
	if (mut->stageInlineHook(ctx, tx, &hook, &op) != D2RL::Mutations::Result::Success) {
		mut->cancelTransaction(ctx, tx);
		ctx->LogWarn("arena: wagers off: PLRSAVE_SaveAllPlayers hook could not be staged (another plugin?).");
		return;
	}
	D2RL::Mutations::CommitResult result { .structSize = D2RL::Mutations::CommitResultSize };
	if (mut->commit(ctx, tx, &result) != D2RL::Mutations::Result::Success || result.result != D2RL::Mutations::Result::Success) {
		ctx->LogWarn("arena: wagers off: hook commit failed.");
		return;
	}
	void* original = nullptr;
	if (mut->getInlineHookOriginal(ctx, tx, op, &original) != D2RL::Mutations::Result::Success || original == nullptr) {
		ctx->LogWarn("arena: wagers off: no call-through for the save hook.");
		return;
	}
	g_origSaveAll.store(reinterpret_cast<SaveAllFn>(original), std::memory_order_release);
	g_saveStamp        = reinterpret_cast<SaveStampFn>(ctx->exeBase + kRvaSaveStamp);
	g_pvp.wagerEnabled = true;
	char msg[160] {};
	std::snprintf(msg, sizeof(msg), "arena: wagers on (stake stat %d, winnings stat %d, max %u gold).", g_pvp.stakeStat, g_pvp.winningsStat, g_pvp.wagerMax);
	ctx->LogInfo(msg);
}

// Pets (own transaction: PvP works without it; another plugin may hook SUNITDMG_KillMonster).
void InstallPets(const D2RL::PluginContext* ctx, const D2RL::MutationService* mut) {
	if (!g_pvp.petsKeep) {
		ctx->LogInfo("arena: [arena.pets] enabled = false; pets die in the arena as usual.");
		return;
	}
	const Check checks[] = {
		{ PvpRva::KillMonster, PvpExpected::kKillMonster, sizeof(PvpExpected::kKillMonster), "SUNITDMG_KillMonster" },
		{ PvpRva::MinionOwner, PvpExpected::kMinionOwner, sizeof(PvpExpected::kMinionOwner), "AIGENERAL_GetMinionOwner" },
		{ PvpRva::PetType, PvpExpected::kPetType, sizeof(PvpExpected::kPetType), "PLAYERPETS_GetPetTypeFromPetGUID" },
		{ PvpRva::IteratePets, PvpExpected::kIteratePets, sizeof(PvpExpected::kIteratePets), "PLAYERPETS_IteratePets" },
		{ PvpRva::WarpPet, PvpExpected::kWarpPet, sizeof(PvpExpected::kWarpPet), "maybe_AIUTIL_WarpPetToOwner" },
		{ PvpRva::IdleNeutral, PvpExpected::kIdleNeutral, sizeof(PvpExpected::kIdleNeutral), "AITACTICS_IdleInNeutralMode" },
		{ PvpRva::DeleteEvent, PvpExpected::kDeleteEvent, sizeof(PvpExpected::kDeleteEvent), "D2GAME_EVENTS_Delete" },
	};
	for (const Check& c : checks) {
		if (!ctx->CheckExpectedBytes(c.rva, c.bytes, c.size)) {
			char msg[192] {};
			std::snprintf(msg, sizeof(msg), "arena: unexpected bytes at RVA 0x%llx (%s); pets die in the arena as usual.", static_cast<unsigned long long>(c.rva), c.what);
			ctx->LogWarn(msg);
			return;
		}
	}
	const uint64_t exe = ctx->exeBase;
	g_minionOwner = reinterpret_cast<MinionOwnerFn>(exe + PvpRva::MinionOwner);
	g_petType     = reinterpret_cast<PetTypeFn>(exe + PvpRva::PetType);
	g_iteratePets = reinterpret_cast<IteratePetsFn>(exe + PvpRva::IteratePets);
	g_warpPet     = reinterpret_cast<WarpPetFn>(exe + PvpRva::WarpPet);
	g_idleNeutral = reinterpret_cast<IdleNeutralFn>(exe + PvpRva::IdleNeutral);
	g_deleteEvent = reinterpret_cast<DeleteEventFn>(exe + PvpRva::DeleteEvent);
	D2RL::Mutations::TransactionHandle tx = D2RL::Mutations::InvalidTransactionHandle;
	if (mut->beginTransaction(ctx, &tx) != D2RL::Mutations::Result::Success) {
		ctx->LogWarn("arena: pet hook transaction failed; pets die in the arena as usual.");
		return;
	}
	const D2RL::Mutations::InlineHookRequest killHook {
		.structSize   = D2RL::Mutations::InlineHookRequestSize,
		.rva          = PvpRva::KillMonster,
		.expected     = PvpExpected::kKillMonster,
		.expectedSize = static_cast<uint32_t>(sizeof(PvpExpected::kKillMonster)),
		.target       = reinterpret_cast<void*>(&ArenaKillMonster),
	};
	D2RL::Mutations::OperationHandle killOp = D2RL::Mutations::InvalidOperationHandle;
	if (mut->stageInlineHook(ctx, tx, &killHook, &killOp) != D2RL::Mutations::Result::Success) {
		mut->cancelTransaction(ctx, tx);
		ctx->LogWarn("arena: SUNITDMG_KillMonster hook could not be staged (another plugin?); pets die in the arena as usual.");
		return;
	}
	D2RL::Mutations::CommitResult result { .structSize = D2RL::Mutations::CommitResultSize };
	if (mut->commit(ctx, tx, &result) != D2RL::Mutations::Result::Success || result.result != D2RL::Mutations::Result::Success) {
		ctx->LogWarn("arena: pet hook commit failed; pets die in the arena as usual.");
		return;
	}
	void* original = nullptr;
	if (mut->getInlineHookOriginal(ctx, tx, killOp, &original) == D2RL::Mutations::Result::Success && original != nullptr) {
		g_origKillMonster.store(reinterpret_cast<KillMonsterFn>(original), std::memory_order_release);
		ctx->LogInfo("arena: pets: Iron Golem and bound demons are kept through arena deaths.");
	}
}

// 2.4.23 (hook conflict #18, Kain 2026-11-14): a function arena only calls may carry another plugin's inline hook
// (skill-ext 0.7.0 Cast Triggers hooks SUNIT_GetTargetUnit 0x48FE20). Bytes as expected = fine. Otherwise ask
// DiagnosticsService: every changed range must be a Tracked inline hook with callThrough Yes (the CLAUDE.md hook-audit
// rule; same method as levels/src/events.cpp). Then calling the function runs the hook chain and is fine.
auto CallThroughOk(const D2RL::PluginContext* ctx, uint64_t rva, const uint8_t* expected, uint32_t size, const char* name) -> bool {
	if (ctx->CheckExpectedBytes(rva, expected, size)) {
		return true;
	}
	const D2RL::DiagnosticsService* diag = nullptr;
	if (ctx->QueryService(&diag) != D2RL::ServiceQueryResult::Success
	    || !D2RL::HasDiagnosticsServiceField(diag, D2RL::DiagnosticsServiceEnumerateModificationRangesFieldEnd) || diag->enumerateModificationRanges == nullptr) {
		Log("arena: %s bytes changed and no DiagnosticsService to identify the change.", name);
		return false;
	}
	D2RL::Diagnostics::HookQuery q {};
	q.structSize   = D2RL::Diagnostics::HookQuerySize;
	q.rva          = rva;
	q.expected     = expected;
	q.expectedSize = size;
	std::array<D2RL::Diagnostics::ModificationRange, 8> ranges {};
	for (auto& m : ranges) {
		m.structSize = D2RL::Diagnostics::ModificationRangeSize;
	}
	uint32_t   count = 0;
	const auto res   = diag->enumerateModificationRanges(ctx, &q, ranges.data(), static_cast<uint32_t>(ranges.size()), &count);
	bool       ok    = res == D2RL::Diagnostics::Result::Success && count > 0 && count <= ranges.size();
	for (uint32_t i = 0; ok && i < count; ++i) {
		const auto& m = ranges[i];
		ok = m.state == D2RL::Diagnostics::ModificationState::Tracked && m.kind == D2RL::Diagnostics::ModificationKind::InlineHook
		     && m.callThrough == D2RL::Diagnostics::CallThroughState::Yes;
		Log("arena: %s is changed: rva 0x%llX size %u, state %u, kind %u, callThrough %u, owner '%s'.", name, static_cast<unsigned long long>(m.rva),
		    m.size, static_cast<unsigned>(m.state), static_cast<unsigned>(m.kind), static_cast<unsigned>(m.callThrough), m.ownerPluginId);
	}
	if (ok) {
		Log("arena: %s carries a loader-managed call-through hook; arena calls it as usual.", name);
	} else {
		Log("arena: %s bytes changed and not a call-through hook (result %u, %u range(s)).", name, static_cast<unsigned>(res), count);
	}
	return ok;
}

// Load-time registrations (layout, panel, channel) and the two chart pointer swaps (own transaction: the gates keep
// working when this part can't install).
auto InstallPvp(const D2RL::PluginContext* ctx) -> bool {
	if (!g_pvp.enabled) {
		ctx->LogInfo("arena: pvp = false; arena deaths and hostility are stock.");
		return false;
	}
	const Check checks[] = {
		{ PvpRva::ReadStat, PvpExpected::kReadStat, sizeof(PvpExpected::kReadStat), "ReadWideUnitStat" },
		{ PvpRva::StateStatList, PvpExpected::kStateStatList, sizeof(PvpExpected::kStateStatList), "STATLIST_GetStatListFromUnitAndState" },
		{ PvpRva::ListOwnerGuid, PvpExpected::kListOwnerGuid, sizeof(PvpExpected::kListOwnerGuid), "STATLIST_GetOwnerGUID" },
		{ PvpRva::ListOwnerType, PvpExpected::kListOwnerType, sizeof(PvpExpected::kListOwnerType), "STATLIST_GetOwnerType" },
		{ PvpRva::SetStat, PvpExpected::kSetStat, sizeof(PvpExpected::kSetStat), "SetWideUnitStat" },
		{ PvpRva::ToggleState, PvpExpected::kToggleState, sizeof(PvpExpected::kToggleState), "STATES_ToggleState" },
		{ PvpRva::ServerUnit, PvpExpected::kServerUnit, sizeof(PvpExpected::kServerUnit), "SUNIT_GetServerUnit" },
		{ PvpRva::OpenHostility, PvpExpected::kHostility, sizeof(PvpExpected::kHostility), "FRIENDLY_OpenHostility" },
		{ PvpRva::RemoveHostility, PvpExpected::kHostility, sizeof(PvpExpected::kHostility), "FRIENDLY_RemoveHostility" },
		{ PvpRva::CheckFlag, PvpExpected::kCheckFlag, sizeof(PvpExpected::kCheckFlag), "PLAYERLIST_CheckFlag" },
		{ PvpRva::PartyId, PvpExpected::kPartyId, sizeof(PvpExpected::kPartyId), "SUNIT_GetPartyId" },
		{ PvpRva::LocalPlayer, PvpExpected::kLocalPlayer, sizeof(PvpExpected::kLocalPlayer), "maybe_CLIENT_GetLocalPlayer" },
		{ PvpRva::LocalIndex, PvpExpected::kLocalIndex, sizeof(PvpExpected::kLocalIndex), "maybe_CLIENT_GetLocalDataContext" },
		{ PvpRva::UpdateAttribute, PvpExpected::kUpdateAttribute, sizeof(PvpExpected::kUpdateAttribute), "D2GAME_UpdateAttribute" },
		{ PvpRva::SetTargetable, PvpExpected::kSetTargetable, sizeof(PvpExpected::kSetTargetable), "SUNIT_SetTargetableFlag" },
		{ PvpRva::ChangeMode, PvpExpected::kChangeMode, sizeof(PvpExpected::kChangeMode), "PLRMODES_ChangeMode" },
		{ PvpRva::DeathUi, PvpExpected::kDeathUi, sizeof(PvpExpected::kDeathUi), "client death UI (FUN_1401E8EB0)" },
		{ PvpRva::CloseUiState, PvpExpected::kCloseUiState, sizeof(PvpExpected::kCloseUiState), "UI_CloseInterfaceState" },
		{ PvpRva::GetUiState, PvpExpected::kGetUiState, sizeof(PvpExpected::kGetUiState), "UI_GetUIState" },
		{ PvpRva::OpenUiState, PvpExpected::kOpenUiState, sizeof(PvpExpected::kOpenUiState), "UI_OpenInterfaceState" },
		{ PvpRva::DeadCheck, PvpExpected::kDeadCheck, sizeof(PvpExpected::kDeadCheck), "client dead check (FUN_1400D0770)" },
		{ PvpRva::RemoveStates, PvpExpected::kRemoveStates, sizeof(PvpExpected::kRemoveStates), "D2GAME_NPC_RemoveStates" },
		{ PvpRva::ResyncPos, PvpExpected::kResyncPos, sizeof(PvpExpected::kResyncPos), "maybe_PLRMSG_ResyncClientPosition" },
		{ PvpRva::RemoveDeathStats, PvpExpected::kRemoveDeathStats, sizeof(PvpExpected::kRemoveDeathStats), "RemoveWideDeathStats" },
		{ PvpRva::StayDeathFlags, PvpExpected::kStayDeathFlags, sizeof(PvpExpected::kStayDeathFlags), "STATES_UpdateStayDeathFlags" },
		{ PvpRva::Unshift, PvpExpected::kUnshift, sizeof(PvpExpected::kUnshift), "SKILLS_RemoveTransformStatesFromShapeshiftedUnit" },
		{ PvpRva::CheckItemType, PvpExpected::kCheckItemType, sizeof(PvpExpected::kCheckItemType), "ITEMS_CheckItemTypeId" },
		{ PvpRva::ExecEvents, PvpExpected::kExecEvents, sizeof(PvpExpected::kExecEvents), "SUNITDMG_ExecuteEvents" },
		{ PvpRva::CalcTotal, PvpExpected::kCalcTotal, sizeof(PvpExpected::kCalcTotal), "SUNITDMG_CalculateTotalDamage" },
		{ PvpRva::NpcInteract, PvpExpected::kNpcInteract, sizeof(PvpExpected::kNpcInteract), "maybe_PLAYER_D2R_IsInteractingWithNpc" },
		{ PvpRva::GetInventory, PvpExpected::kGetInventory, sizeof(PvpExpected::kGetInventory), "UNITS_GetInventory" },
		{ PvpRva::CursorItem, PvpExpected::kCursorItem, sizeof(PvpExpected::kCursorItem), "INVENTORY_GetCursorItem" },
		{ PvpRva::TradeState, PvpExpected::kTradeState, sizeof(PvpExpected::kTradeState), "maybe_CLIENTS_GetTradeState" },
	};
	for (const Check& c : checks) {
		if (!ctx->CheckExpectedBytes(c.rva, c.bytes, c.size)) {
			char msg[192] {};
			std::snprintf(msg, sizeof(msg), "arena: unexpected bytes at RVA 0x%llx (%s); PvP not installed.", static_cast<unsigned long long>(c.rva), c.what);
			ctx->LogError(msg);
			return false;
		}
	}
	if (ctx->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		ctx->LogError("arena: ThreadService missing; PvP not installed.");
		g_threads = nullptr;
		return false;
	}
	const uint64_t exe = ctx->exeBase;
	if (g_pvp.bodiesEnabled && !ctx->CheckExpectedBytes(PvpRva::SpawnMonster, PvpExpected::kSpawnMonster, sizeof(PvpExpected::kSpawnMonster))) {
		ctx->LogWarn("arena: unexpected bytes at D2GAME_SpawnMonster; no bodies in the arenas.");
		g_pvp.bodiesEnabled = false;
	}
	if (g_pvp.bodiesEnabled) {
		g_spawnMonster = reinterpret_cast<SpawnMonsterFn>(exe + PvpRva::SpawnMonster);
	}
	g_readStat      = reinterpret_cast<ReadStatFn>(exe + PvpRva::ReadStat);
	// 2.4.23 (#18): checked on its own; only the Sacrifice kill credit needs it (falls back to the attacker)
	if (CallThroughOk(ctx, PvpRva::TargetUnit, PvpExpected::kTargetUnit, sizeof(PvpExpected::kTargetUnit), "SUNIT_GetTargetUnit")) {
		g_targetUnit = reinterpret_cast<TargetUnitFn>(exe + PvpRva::TargetUnit);
	} else {
		g_targetUnit = nullptr;
		ctx->LogWarn("arena: SUNIT_GetTargetUnit can't be used; Sacrifice kill credit goes to the attacker.");
	}
	g_stateStatList = reinterpret_cast<StateStatListFn>(exe + PvpRva::StateStatList);
	g_listOwnerGuid = reinterpret_cast<ListOwnerFn>(exe + PvpRva::ListOwnerGuid);
	g_listOwnerType = reinterpret_cast<ListOwnerFn>(exe + PvpRva::ListOwnerType);
	g_setStat       = reinterpret_cast<SetStatFn>(exe + PvpRva::SetStat);
	g_toggleState   = reinterpret_cast<ToggleStateFn>(exe + PvpRva::ToggleState);
	g_serverUnit    = reinterpret_cast<ServerUnitFn>(exe + PvpRva::ServerUnit);
	g_openHostile   = reinterpret_cast<HostilityFn>(exe + PvpRva::OpenHostility);
	g_removeHostile = reinterpret_cast<HostilityFn>(exe + PvpRva::RemoveHostility);
	g_checkFlag     = reinterpret_cast<CheckFlagFn>(exe + PvpRva::CheckFlag);
	g_partyId       = reinterpret_cast<PartyIdFn>(exe + PvpRva::PartyId);
	g_localPlayer   = reinterpret_cast<LocalPlayerFn>(exe + PvpRva::LocalPlayer);
	if (ctx->CheckExpectedBytes(kRvaClientGameLoadedWriter, kClientGameLoadedWriter, sizeof(kClientGameLoadedWriter))) {
		g_clientGameLoaded = reinterpret_cast<const volatile int32_t*>(exe + kRvaClientGameLoaded);
	} else {
		ctx->LogWarn("arena: client load flag not found; a duel countdown can start while a client still loads.");
	}
	g_localIndex    = reinterpret_cast<LocalIndexFn>(exe + PvpRva::LocalIndex);
	g_origDeath     = reinterpret_cast<StartIdFn>(exe + PvpRva::StartIdDeath);
	g_origNeutral   = reinterpret_cast<StartXyFn>(exe + PvpRva::StartXyNeutral);
	g_origMove      = reinterpret_cast<StartXyFn>(exe + PvpRva::StartXyMove);
	g_updateAttr    = reinterpret_cast<UpdateAttrFn>(exe + PvpRva::UpdateAttribute);
	g_setTargetable = reinterpret_cast<TargetableFn>(exe + PvpRva::SetTargetable);
	g_changeMode2   = reinterpret_cast<ChangeModeFn>(exe + PvpRva::ChangeMode);
	g_origEndAnim   = reinterpret_cast<PlrEventFn>(exe + PvpRva::EndAnim);
	g_origRegen     = reinterpret_cast<PlrEventFn>(exe + PvpRva::StatRegen);
	g_removeStates  = reinterpret_cast<UnitFn>(exe + PvpRva::RemoveStates);
	g_removeDeathStats = reinterpret_cast<UnitFn>(exe + PvpRva::RemoveDeathStats);
	g_refreshPassives  = reinterpret_cast<UnitFn>(exe + PvpRva::RefreshPassives);
	g_unshift          = reinterpret_cast<UnitFn>(exe + PvpRva::Unshift);
	g_stayDeath        = reinterpret_cast<StayDeathFn>(exe + PvpRva::StayDeathFlags);
	g_itemType         = reinterpret_cast<ItemTypeFn>(exe + PvpRva::CheckItemType);
	g_npcInteract      = reinterpret_cast<NpcInteractFn>(exe + PvpRva::NpcInteract);
	g_getInventory     = reinterpret_cast<GetInventoryFn>(exe + PvpRva::GetInventory);
	g_cursorItem       = reinterpret_cast<CursorItemFn>(exe + PvpRva::CursorItem);
	g_resync        = reinterpret_cast<UnitFn>(exe + PvpRva::ResyncPos);
	g_closeUi       = reinterpret_cast<CloseUiFn>(exe + PvpRva::CloseUiState);
	g_getUi         = reinterpret_cast<GetUiFn>(exe + PvpRva::GetUiState);
	{
		static constexpr uint8_t kGameTypeBytes[] = { 0x8B, 0x05, 0x5E, 0x84, 0x99, 0x02, 0xC3 }; // CLIENT_GetGameType
		if (std::memcmp(reinterpret_cast<const void*>(exe + 0x8AF90), kGameTypeBytes, sizeof(kGameTypeBytes)) == 0) {
			g_gameType = reinterpret_cast<GameTypeFn>(exe + 0x8AF90);
		} else {
			ctx->LogWarn("arena: CLIENT_GetGameType bytes differ: the team duel obelisks also show in single player.");
		}
	}

	// countdown panel (optional: without it the respawn still happens)
	const D2RL::ResourceService* res = nullptr;
	if (ctx->QueryService(&res) == D2RL::ServiceQueryResult::Success && D2RL::HasResourceServiceField(res, D2RL::ResourceServiceRequiredSize)
	    && ctx->QueryService(&g_panels) == D2RL::ServiceQueryResult::Success && D2RL::HasPanelServiceField(g_panels, D2RL::PanelServiceRequiredSize)
	    && ctx->QueryService(&g_widgets) == D2RL::ServiceQueryResult::Success && D2RL::HasWidgetServiceField(g_widgets, D2RL::WidgetServiceRequiredSize)) {
		bool ok = true;
		const bool hud = g_pvp.countdownHud && HudRegister(ctx, res);
		for (int k = 1; k <= kCountdownPanels && ok && !hud; ++k) {
			CountdownPanel& cp = g_cdPanels[k];
			cp.localId         = "Countdown" + std::to_string(k);
			cp.path            = "data/global/ui/layouts/arena/" + cp.localId + "hd.json";
			cp.layout          = CountdownLayout(k);
			const D2RL::Resources::ResourceRegistration layout {
				.structSize = D2RL::Resources::ResourceRegistrationSize,
				.path       = cp.path.c_str(),
				.bytes      = cp.layout.c_str(),
				.byteCount  = static_cast<uint32_t>(cp.layout.size()),
			};
			D2RL::Resources::RegistrationHandle lh = D2RL::Resources::InvalidHandle;
			const D2RL::Panels::PanelRegistration panel { .structSize = D2RL::Panels::PanelRegistrationSize, .flags = D2RL::Panels::PanelFlags::GameplayLeftSlot, .localId = cp.localId.c_str() };
			ok = res->registerResource(ctx, &layout, &lh) == D2RL::Resources::Result::Success && g_panels->registerPanel(ctx, &panel, &cp.handle) == D2RL::Panels::Result::Success;
		}
		const bool menuPanels = ok && g_pvp.duelEnabled && g_pvp.playerMenu && MenuRegister(ctx, res);
		g_menuOn              = menuPanels; // InstallPlayerMenu adds the click hook
		const bool wagerPanel = ok && g_pvp.duelEnabled && g_pvp.wagerEnabled && WagerRegister(ctx, res);
		const bool lobbyPanel = ok && g_pvp.duelEnabled && LobbyRegister(ctx, res); // 1.9.0
		if (menuPanels || wagerPanel || lobbyPanel) {
			const D2RL::SharedEventService* events = nullptr;
			D2RL::SharedEvents::ListenerHandle uh = D2RL::SharedEvents::InvalidHandle;
			const D2RL::SharedEvents::UiMessageListener ui { .structSize = D2RL::SharedEvents::UiMessageListenerSize, .callback = OnUiMessage };
			if (ctx->QueryService(&events) != D2RL::ServiceQueryResult::Success || !D2RL::HasSharedEventServiceField(events, D2RL::SharedEventServiceRequiredSize)
			    || events->registerUiMessageListener(ctx, &ui, &uh) != D2RL::SharedEvents::Result::Success) {
				ctx->LogWarn("arena: no UI message listener; the wager window, player menu and duel popup buttons won't work (use the console commands).");
			}
		}
		if (!ok) {
			ctx->LogWarn("arena: countdown panels could not be registered; respawns happen without the on-screen countdown.");
			g_panels = nullptr;
		} else if ((g_pvp.scoreboard || g_pvp.duelEnabled) && ScoreRegister(ctx, res)) {
			const D2RL::InputService* input = nullptr;
			if (ctx->QueryService(&input) == D2RL::ServiceQueryResult::Success && D2RL::HasInputServiceField(input, D2RL::InputServiceRequiredSize)) {
				const D2RL::Input::ActionRegistration action {
					.structSize       = D2RL::Input::ActionRegistrationSize,
					.logicalId        = "arena_scoreboard",
					.displayName      = "Show / hide arena scoreboard",
					.category         = "Arena",
					.defaultPrimary   = { D2RL::Input::Key::None, D2RL::Input::Modifier::None },
					.defaultSecondary = { D2RL::Input::Key::None, D2RL::Input::Modifier::None },
					.callback         = OnScoreAction,
				};
				D2RL::Input::ActionHandle ah = 0;
				if (input->registerAction(ctx, &action, &ah) != D2RL::Input::Result::Success) {
					ctx->LogWarn("arena: scoreboard hotkey not registered; use `arena scoreboard`.");
				}
				if (g_pvp.duelEnabled) {
					for (const auto& [id, label, cb] : { std::tuple { "arena_duel_accept", "Accept duel challenge", &OnDuelAcceptAction },
					                                     std::tuple { "arena_duel_decline", "Decline duel challenge", &OnDuelDeclineAction } }) {
						D2RL::Input::ActionRegistration da = action;
						da.logicalId   = id;
						da.displayName = label;
						da.callback    = cb;
						D2RL::Input::ActionHandle dh = 0;
						if (input->registerAction(ctx, &da, &dh) != D2RL::Input::Result::Success) {
							ctx->LogWarn("arena: duel key not registered; use `arena accept` / `arena decline`.");
						}
					}
				}
			} else {
				ctx->LogWarn("arena: InputService missing; scoreboard hotkey not registered (use `arena scoreboard`).");
			}
		}
	} else {
		ctx->LogWarn("arena: panel services missing; respawns happen without the on-screen countdown.");
		g_panels = nullptr;
	}
	// host <-> client channel (optional: the hosting player's own countdown works without it)
	if (ctx->QueryService(&g_net) == D2RL::ServiceQueryResult::Success && D2RL::HasNetworkServiceField(g_net, D2RL::NetworkServiceRequiredSize)) {
		const D2RL::Network::ChannelRegistration channel {
			.structSize         = D2RL::Network::ChannelRegistrationSize,
			.localChannelId     = 1,
			.compatibilityToken = 0x4152454E41001200ULL, // "ARENA", protocol 0.18 (1.9.1: mixed teams in the lobby state)
			.hostMessage        = OnHostMessage,
			.clientMessage      = OnClientMessage,
			.connectionState    = OnConnection,
		};
		if (g_net->registerChannel(ctx, &channel, &g_channel) != D2RL::Network::Result::Success) {
			ctx->LogWarn("arena: network channel not registered; only the hosting player sees the countdown.");
			g_net = nullptr;
		}
	} else {
		g_net = nullptr;
	}

	const D2RL::MutationService* mut = nullptr;
	if (ctx->QueryService(&mut) != D2RL::ServiceQueryResult::Success || !D2RL::HasMutationServiceField(mut, D2RL::MutationServiceRequiredSize)) {
		return false;
	}
	D2RL::Mutations::TransactionHandle tx = D2RL::Mutations::InvalidTransactionHandle;
	if (mut->beginTransaction(ctx, &tx) != D2RL::Mutations::Result::Success) {
		return false;
	}
	struct Slot {
		uint64_t rva;
		uint64_t original;
		uint64_t wrapper;
	};
	std::vector<Slot> slots = {
		{ PvpRva::PlrModeChart + 0 * 0x18 + 8, exe + PvpRva::StartIdDeath, reinterpret_cast<uint64_t>(&ArenaDeath) },
		{ PvpRva::PlrModeChart + 1 * 0x18 + 0, exe + PvpRva::StartXyNeutral, reinterpret_cast<uint64_t>(&ArenaNeutral) },
		{ PvpRva::PlayerEvents + 1 * 8, exe + PvpRva::EndAnim, reinterpret_cast<uint64_t>(&ArenaEndAnim) },
		{ PvpRva::PlayerEvents + 3 * 8, exe + PvpRva::StatRegen, reinterpret_cast<uint64_t>(&ArenaStatRegen) },
		{ PvpRva::PlrModeChart + 2 * 0x18 + 0, exe + PvpRva::StartXyMove, reinterpret_cast<uint64_t>(&ArenaMove) },
		{ PvpRva::PlrModeChart + 3 * 0x18 + 0, exe + PvpRva::StartXyMove, reinterpret_cast<uint64_t>(&ArenaMove) },
	};
	const std::array<PacketFn, 0x43> gates = MakePacketGates(std::make_integer_sequence<int, 0x43> {});
	g_origPrepare[3] = reinterpret_cast<PrepareFn>(exe + PvpExpected::kPrepareSlots[0][1]);
	g_origPrepare[4] = reinterpret_cast<PrepareFn>(exe + PvpExpected::kPrepareSlots[1][1]);
	g_origPrepare[6] = reinterpret_cast<PrepareFn>(exe + PvpExpected::kPrepareSlots[2][1]);
	slots.push_back({ PvpRva::PrepareTable + 3 * 8, exe + PvpExpected::kPrepareSlots[0][1], reinterpret_cast<uint64_t>(&PotionGate<3>) });
	slots.push_back({ PvpRva::PrepareTable + 4 * 8, exe + PvpExpected::kPrepareSlots[1][1], reinterpret_cast<uint64_t>(&PotionGate<4>) });
	slots.push_back({ PvpRva::PrepareTable + 6 * 8, exe + PvpExpected::kPrepareSlots[2][1], reinterpret_cast<uint64_t>(&PotionGate<6>) });
	if (g_pvp.bodiesEnabled) {
		static constexpr std::array<SrvStartFn, 4> kGates { &CorpseSkillGate<17>, &CorpseSkillGate<21>, &CorpseSkillGate<33>, &CorpseSkillGate<34> };
		for (size_t i = 0; i < 4; ++i) {
			const auto& cs = PvpExpected::kCorpseSlots[i];
			g_origSrvStart[cs[0]] = reinterpret_cast<SrvStartFn>(exe + cs[1]);
			slots.push_back({ PvpRva::SrvStartTable + cs[0] * 8, exe + cs[1], reinterpret_cast<uint64_t>(kGates[i]) });
		}
	}
	for (const auto& ps : PvpExpected::kPacketSlots) {
		g_origPacket[ps[0]] = reinterpret_cast<PacketFn>(exe + ps[1]);
		slots.push_back({ PvpRva::PacketTable + ps[0] * 8, exe + ps[1], reinterpret_cast<uint64_t>(gates[ps[0]]) });
	}
	std::vector<std::array<uint8_t, 8>> expected(slots.size());
	std::vector<std::array<uint8_t, 8>> bytes(slots.size());
	for (size_t i = 0; i < slots.size(); ++i) {
		std::memcpy(expected[i].data(), &slots[i].original, 8);
		std::memcpy(bytes[i].data(), &slots[i].wrapper, 8);
		const D2RL::Mutations::BytePatchRequest patch {
			.structSize   = D2RL::Mutations::BytePatchRequestSize,
			.rva          = slots[i].rva,
			.expected     = expected[i].data(),
			.expectedSize = 8,
			.bytes        = bytes[i].data(),
			.size         = 8,
		};
		D2RL::Mutations::OperationHandle op = D2RL::Mutations::InvalidOperationHandle;
		if (mut->stageBytePatch(ctx, tx, &patch, &op) != D2RL::Mutations::Result::Success) {
			mut->cancelTransaction(ctx, tx);
			char msg[192] {};
			std::snprintf(msg, sizeof(msg), "arena: table entry at RVA 0x%llx is not the stock function (another plugin?); PvP not installed.",
			              static_cast<unsigned long long>(slots[i].rva));
			ctx->LogError(msg);
			return false;
		}
	}
	const D2RL::Mutations::InlineHookRequest uiHook {
		.structSize   = D2RL::Mutations::InlineHookRequestSize,
		.rva          = PvpRva::DeathUi,
		.expected     = PvpExpected::kDeathUi,
		.expectedSize = static_cast<uint32_t>(sizeof(PvpExpected::kDeathUi)),
		.target       = reinterpret_cast<void*>(&ArenaDeathUi),
	};
	D2RL::Mutations::OperationHandle uiOp = D2RL::Mutations::InvalidOperationHandle;
	if (mut->stageInlineHook(ctx, tx, &uiHook, &uiOp) != D2RL::Mutations::Result::Success) {
		mut->cancelTransaction(ctx, tx);
		ctx->LogError("arena: the client death-screen hook could not be staged; PvP not installed.");
		return false;
	}
	const D2RL::Mutations::InlineHookRequest openHook {
		.structSize   = D2RL::Mutations::InlineHookRequestSize,
		.rva          = PvpRva::OpenUiState,
		.expected     = PvpExpected::kOpenUiState,
		.expectedSize = static_cast<uint32_t>(sizeof(PvpExpected::kOpenUiState)),
		.target       = reinterpret_cast<void*>(&ArenaOpenUi),
	};
	D2RL::Mutations::OperationHandle openOp = D2RL::Mutations::InvalidOperationHandle;
	if (mut->stageInlineHook(ctx, tx, &openHook, &openOp) != D2RL::Mutations::Result::Success) {
		mut->cancelTransaction(ctx, tx);
		ctx->LogError("arena: the UI_OpenInterfaceState hook could not be staged; PvP not installed.");
		return false;
	}
	const D2RL::Mutations::InlineHookRequest deadHook {
		.structSize   = D2RL::Mutations::InlineHookRequestSize,
		.rva          = PvpRva::DeadCheck,
		.expected     = PvpExpected::kDeadCheck,
		.expectedSize = static_cast<uint32_t>(sizeof(PvpExpected::kDeadCheck)),
		.target       = reinterpret_cast<void*>(&ArenaDeadCheck),
	};
	D2RL::Mutations::OperationHandle deadOp = D2RL::Mutations::InvalidOperationHandle;
	if (mut->stageInlineHook(ctx, tx, &deadHook, &deadOp) != D2RL::Mutations::Result::Success) {
		mut->cancelTransaction(ctx, tx);
		ctx->LogError("arena: the client dead-check hook could not be staged; PvP not installed.");
		return false;
	}
	const D2RL::Mutations::InlineHookRequest execHook {
		.structSize   = D2RL::Mutations::InlineHookRequestSize,
		.rva          = PvpRva::ExecEvents,
		.expected     = PvpExpected::kExecEvents,
		.expectedSize = static_cast<uint32_t>(sizeof(PvpExpected::kExecEvents)),
		.target       = reinterpret_cast<void*>(&ArenaExecEvents),
	};
	const D2RL::Mutations::InlineHookRequest calcHook {
		.structSize   = D2RL::Mutations::InlineHookRequestSize,
		.rva          = PvpRva::CalcTotal,
		.expected     = PvpExpected::kCalcTotal,
		.expectedSize = static_cast<uint32_t>(sizeof(PvpExpected::kCalcTotal)),
		.target       = reinterpret_cast<void*>(&ArenaCalcTotal),
	};
	D2RL::Mutations::OperationHandle execOp = D2RL::Mutations::InvalidOperationHandle;
	D2RL::Mutations::OperationHandle calcOp = D2RL::Mutations::InvalidOperationHandle;
	if (mut->stageInlineHook(ctx, tx, &execHook, &execOp) != D2RL::Mutations::Result::Success
	    || mut->stageInlineHook(ctx, tx, &calcHook, &calcOp) != D2RL::Mutations::Result::Success) {
		mut->cancelTransaction(ctx, tx);
		ctx->LogError("arena: the damage hooks could not be staged (another plugin hooks SUNITDMG_ExecuteEvents / CalculateTotalDamage?); PvP not installed.");
		return false;
	}
	D2RL::Mutations::CommitResult result { .structSize = D2RL::Mutations::CommitResultSize };
	if (mut->commit(ctx, tx, &result) != D2RL::Mutations::Result::Success || result.result != D2RL::Mutations::Result::Success) {
		ctx->LogError("arena: PvP commit failed.");
		return false;
	}
	void* original = nullptr;
	if (mut->getInlineHookOriginal(ctx, tx, uiOp, &original) == D2RL::Mutations::Result::Success && original != nullptr) {
		g_origDeathUi.store(reinterpret_cast<DeathUiFn>(original), std::memory_order_release);
	}
	original = nullptr;
	if (mut->getInlineHookOriginal(ctx, tx, openOp, &original) == D2RL::Mutations::Result::Success && original != nullptr) {
		g_origOpenUi.store(reinterpret_cast<OpenUiFn>(original), std::memory_order_release);
	}
	original = nullptr;
	if (mut->getInlineHookOriginal(ctx, tx, deadOp, &original) == D2RL::Mutations::Result::Success && original != nullptr) {
		g_origDeadCheck.store(reinterpret_cast<DeathUiFn>(original), std::memory_order_release);
	}
	original = nullptr;
	if (mut->getInlineHookOriginal(ctx, tx, execOp, &original) == D2RL::Mutations::Result::Success && original != nullptr) {
		g_origExec.store(reinterpret_cast<ExecEventsFn>(original), std::memory_order_release);
	}
	original = nullptr;
	if (mut->getInlineHookOriginal(ctx, tx, calcOp, &original) == D2RL::Mutations::Result::Success && original != nullptr) {
		g_origCalcTotal.store(reinterpret_cast<CalcTotalFn>(original), std::memory_order_release);
	}
	g_pvpInstalled = true;
	InstallPets(ctx, mut);
	InstallWager(ctx, mut);
	if (g_pvp.wagerEnabled) {
		InstallItemStakes(ctx, mut);
	}
	InstallLabelsAndLock(ctx, mut, g_pvp.duelEnabled);
	if (g_pvp.duelEnabled) {
		InstallAutomapHook(ctx, mut);
	}
	if (g_pvp.petsHoldDown || g_pvp.petsKeep) {
		InstallAuraGate(ctx, mut); // 2.2.1
	}
	InstallPlayerMenu(ctx, mut);
	return true;
}

void ScoresCommand(const D2RL::ConsoleCommandContext* cmd) {
	std::vector<PlayerScore> list;
	bool                     host = false;
	{
		std::lock_guard lock(g_pvpLock);
		host = g_game != nullptr;
		for (const auto& [guid, s] : g_scores) {
			list.push_back(s);
		}
	}
	if (!host) { // joined client (2026-11-01: "no arena deaths" while there were): the scores live on the host only
		std::string text;
		{
			std::lock_guard lock(g_scoreLock);
			text = g_netScore;
		}
		if (text.empty()) {
			cmd->plugin->WriteConsoleMessage("arena scores: you are not in an arena fight (the host keeps the scores; this shows your arena only).");
			return;
		}
		const std::vector<std::string> f = text.front() == kTeamMark ? SplitTeam(text) : std::vector<std::string>();
		if (f.size() >= 4 && f.size() % 2 == 0) { // 2.0.0: Team One / Team Two totals, then each player (no kills / deaths in this message)
			cmd->plugin->WriteConsoleMessage(f[0] == "*" ? "arena scores (your arena): Battle Royale"
			                                             : ("arena scores (your arena): Team One " + f[0] + " : " + f[1] + " Team Two").c_str());
			for (size_t i = 2; i + 1 < f.size(); i += 2) {
				char line[128] {};
				std::snprintf(line, sizeof(line), "  %s  %-16s %5s", f[i].empty() || f[i][0] == '2' ? "" : f[i][0] == '1' ? "Team Two" : "Team One", f[i].empty() ? "" : f[i].c_str() + 1, f[i + 1].c_str());
				cmd->plugin->WriteConsoleMessage(line);
			}
		} else {
			cmd->plugin->WriteConsoleMessage("arena scores (your arena): name - score (kills / deaths)");
			cmd->plugin->WriteConsoleMessage(("  " + text).c_str());
		}
		return;
	}
	std::sort(list.begin(), list.end(), [](const PlayerScore& a, const PlayerScore& b) { return a.score > b.score; });
	if (list.empty()) {
		cmd->plugin->WriteConsoleMessage("arena scores: no arena deaths in the current fights yet.");
		return;
	}
	cmd->plugin->WriteConsoleMessage("arena scores (current fights; a new fight starts at 0): name - score (kills / deaths)");
	for (const PlayerScore& s : list) {
		char line[128] {};
		std::snprintf(line, sizeof(line), "  %-16s %5d  (%d / %d)", s.name.c_str(), s.score, s.kills, s.deaths);
		cmd->plugin->WriteConsoleMessage(line);
	}
}

// ---------------------------------------------------------------------------
// Install.
// ---------------------------------------------------------------------------
auto Install(const D2RL::PluginContext* ctx) -> bool {
	const Check checks[] = {
		{ Rva::GetRoom, Expected::kGetRoom, sizeof(Expected::kGetRoom), "UNITS_GetRoom" },
		{ Rva::GetCoords, Expected::kGetCoords, sizeof(Expected::kGetCoords), "UNITS_GetCoords" },
		{ Rva::GetAnimMode, Expected::kGetAnimMode, sizeof(Expected::kGetAnimMode), "UNITS_GetAnimMode" },
		{ Rva::ChangeAnimMode, Expected::kChangeAnimMode, sizeof(Expected::kChangeAnimMode), "UNITS_ChangeAnimMode" },
		{ Rva::FreeCoords, Expected::kFreeCoords, sizeof(Expected::kFreeCoords), "COLLISION_GetFreeCoordinates" },
		{ Rva::MovePlayer, Expected::kMovePlayer, sizeof(Expected::kMovePlayer), "sub_6FCBDFE0 (move)" },
		{ Rva::RoomCoords, Expected::kRoomCoords, sizeof(Expected::kRoomCoords), "DUNGEON_GetRoomCoordinates" },
		{ Rva::LevelIdFromRoom, Expected::kLevelIdFromRoom, sizeof(Expected::kLevelIdFromRoom), "DUNGEON_GetLevelIdFromRoom" },
		{ Rva::ClientFromPlayer, Expected::kClientFromPlayer, sizeof(Expected::kClientFromPlayer), "SUNIT_GetClientFromPlayer" },
		{ Rva::RoomInSight, Expected::kRoomInSight, sizeof(Expected::kRoomInSight), "packet 0x07 RoomInSight" },
		{ Rva::SetTeleported, Expected::kSetTeleported, sizeof(Expected::kSetTeleported), "SUNIT_ToggleTeleportedFlagEx" },
		{ Rva::FindRoomAt, Expected::kFindRoomAt, sizeof(Expected::kFindRoomAt), "DUNGEON_FindRoomBySubtileCoordinates" },
		{ Rva::StreamRoom, Expected::kStreamRoom, sizeof(Expected::kStreamRoom), "DUNGEON_StreamRoomAtCoords" },
		{ Rva::GetAct, Expected::kGetActThunk, sizeof(Expected::kGetActThunk), "GetActFromLevelId thunk" },
		{ Rva::ServerMessage, Expected::kServerMessage, sizeof(Expected::kServerMessage), "D2GAME_PACKETS_SendPacket0x26_ServerMessage" },
		{ Rva::AttachSound, Expected::kAttachSound, sizeof(Expected::kAttachSound), "SUNIT_AttachSound" },
		{ Rva::AllocUnit, Expected::kAllocUnit, sizeof(Expected::kAllocUnit), "SUNIT_AllocUnitData" },
		{ Rva::WarpUnit, Expected::kWarpUnit, sizeof(Expected::kWarpUnit), "LEVEL_WarpUnit" },
	};
	for (const Check& c : checks) {
		if (!ctx->CheckExpectedBytes(c.rva, c.bytes, c.size)) {
			char msg[192] {};
			std::snprintf(msg, sizeof(msg), "arena: unexpected bytes at RVA 0x%llx (%s); nothing installed.", static_cast<unsigned long long>(c.rva), c.what);
			ctx->LogError(msg);
			return false;
		}
	}
	const uint64_t exe = ctx->exeBase;
	g_getRoom       = reinterpret_cast<GetRoomFn>(exe + Rva::GetRoom);
	g_getCoords     = reinterpret_cast<GetCoordsFn>(exe + Rva::GetCoords);
	g_getAnimMode   = reinterpret_cast<GetAnimModeFn>(exe + Rva::GetAnimMode);
	g_changeMode    = reinterpret_cast<ChangeAnimModeFn>(exe + Rva::ChangeAnimMode);
	g_freeCoords    = reinterpret_cast<FreeCoordsFn>(exe + Rva::FreeCoords);
	g_movePlayer    = reinterpret_cast<MovePlayerFn>(exe + Rva::MovePlayer);
	g_roomCoords    = reinterpret_cast<RoomCoordsFn>(exe + Rva::RoomCoords);
	g_levelId       = reinterpret_cast<LevelIdFromRoomFn>(exe + Rva::LevelIdFromRoom);
	g_client        = reinterpret_cast<ClientFromPlayerFn>(exe + Rva::ClientFromPlayer);
	g_roomInSight   = reinterpret_cast<RoomInSightFn>(exe + Rva::RoomInSight);
	g_setTeleported = reinterpret_cast<SetTeleportedFn>(exe + Rva::SetTeleported);
	g_findRoomAt    = reinterpret_cast<FindRoomAtFn>(exe + Rva::FindRoomAt);
	g_streamRoom    = reinterpret_cast<StreamRoomFn>(exe + Rva::StreamRoom);
	g_getAct        = reinterpret_cast<GetActFn>(exe + Rva::GetAct);
	g_serverMessage = reinterpret_cast<ServerMessageFn>(exe + Rva::ServerMessage);
	g_attachSound   = reinterpret_cast<AttachSoundFn>(exe + Rva::AttachSound);
	g_allocUnit     = reinterpret_cast<AllocUnitFn>(exe + Rva::AllocUnit);
	g_warpUnit      = reinterpret_cast<WarpUnitFn>(exe + Rva::WarpUnit);
	g_origTrapDoor  = reinterpret_cast<OperateFn>(exe + Rva::TrapDoor);
	g_origPad       = reinterpret_cast<OperateFn>(exe + Rva::TeleportPad);

	const D2RL::MutationService* mut = nullptr;
	if (ctx->QueryService(&mut) != D2RL::ServiceQueryResult::Success || !D2RL::HasMutationServiceField(mut, D2RL::MutationServiceRequiredSize)) {
		ctx->LogError("arena: MutationService unavailable; nothing installed.");
		return false;
	}
	D2RL::Mutations::TransactionHandle tx = D2RL::Mutations::InvalidTransactionHandle;
	if (mut->beginTransaction(ctx, &tx) != D2RL::Mutations::Result::Success) {
		ctx->LogError("arena: beginTransaction failed.");
		return false;
	}
	// Slot value now (the stock function) -> the wrapper. Another plugin owning the slot makes the expected bytes miss.
	struct Slot {
		uint64_t index;
		uint64_t original;
		uint64_t wrapper;
	};
	const Slot slots[] = {
		{ 16, exe + Rva::TrapDoor, reinterpret_cast<uint64_t>(&OperateTrapDoor) },
		{ 27, exe + Rva::TeleportPad, reinterpret_cast<uint64_t>(&OperateGate) },
	};
	std::array<std::array<uint8_t, 8>, 2> expected {};
	std::array<std::array<uint8_t, 8>, 2> bytes {};
	for (size_t i = 0; i < 2; ++i) {
		std::memcpy(expected[i].data(), &slots[i].original, 8);
		std::memcpy(bytes[i].data(), &slots[i].wrapper, 8);
		const D2RL::Mutations::BytePatchRequest patch {
			.structSize   = D2RL::Mutations::BytePatchRequestSize,
			.rva          = Rva::OperateTable + slots[i].index * 8,
			.expected     = expected[i].data(),
			.expectedSize = 8,
			.bytes        = bytes[i].data(),
			.size         = 8,
		};
		D2RL::Mutations::OperationHandle opHandle = D2RL::Mutations::InvalidOperationHandle;
		if (mut->stageBytePatch(ctx, tx, &patch, &opHandle) != D2RL::Mutations::Result::Success) {
			mut->cancelTransaction(ctx, tx);
			char msg[160] {};
			std::snprintf(msg, sizeof(msg), "arena: operate slot %llu is not the stock function (another plugin?); nothing installed.",
			              static_cast<unsigned long long>(slots[i].index));
			ctx->LogError(msg);
			return false;
		}
	}
	D2RL::Mutations::CommitResult result { .structSize = D2RL::Mutations::CommitResultSize };
	if (mut->commit(ctx, tx, &result) != D2RL::Mutations::Result::Success || result.result != D2RL::Mutations::Result::Success) {
		char msg[160] {};
		std::snprintf(msg, sizeof(msg), "arena: commit failed (result %u, reason %u).", static_cast<unsigned>(result.result),
		              static_cast<unsigned>(result.failureReason));
		ctx->LogError(msg);
		return false;
	}
	g_installed = true;
	return true;
}

void __cdecl OnGameplayEvent(const D2RL::PluginContext* ctx, const D2RL::Lifecycle::GameplayEvent* ev, void*) noexcept {
	if (ev != nullptr && ev->kind == D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady) {
		g_hudInGame = true; // countdown HUD: open for the whole game (UI thread)
		(void)HudEnsureOpen(ctx);
		(void)ScoreEnsureOpen(ctx);
		if (void* me = g_localPlayer != nullptr ? g_localPlayer(g_localIndex()) : nullptr; me != nullptr) {
			g_localGuid = Field<uint32_t>(me, 0x08); // host -> own player messages (duel notices)
		}
		return;
	}
	if (ev != nullptr && ev->kind == D2RL::Lifecycle::GameplayEventKind::GameLeft) {
		g_hudInGame = false;
		if (g_hudOn) {
			HudClose(ctx);
		}
		if (g_scoreOn) {
			ScoreClose(ctx);
		}
		if (g_wagerOn) {
			{
				std::lock_guard wl(g_wagerViewLock);
				g_wagerView = WagerView {};
			}
			WagerClosePanel(ctx);
		}
		if (g_lobbyOn) {
			{
				std::lock_guard ll(g_lobbyViewLock);
				g_lobbyView = LobbyView {};
			}
			LobbyClosePanel(ctx);
		}
		if (g_menuOn) {
			{
				std::lock_guard il(g_inviteLock);
				g_inviteView = InviteView {};
			}
			g_menuTarget = 0;
			MenuClose(ctx);
			InviteClose(ctx);
		}
		{
			std::lock_guard sl(g_scoreLock);
			g_hostScore.clear();
			g_netScore.clear();
		}
		{
			std::lock_guard lock(g_stateLock);
			g_opened.clear();
		}
		g_tickEpoch.fetch_add(1);
		g_duelLoadLevel = 0;
		g_duelLoadUntil = 0;
		g_duelCdUntil   = 0;
		g_duelResultUntil = 0;
		g_suddenKind      = 0;
		g_suddenUntil     = 0;
		{
			std::lock_guard ml(g_mapHideLock); // 1.9.3: no duel automap rules into the next game
			g_mapHidden.clear();
			g_teamMates.clear();
			g_mapRange = 0;
		}
		{
			std::lock_guard nl(g_noticeLock); // else the notice keeps counting on the character screen and into the next game
			g_noticeText.clear();
			g_noticeUntil   = 0;
			g_noticeSeconds = false;
			g_noticeBaseText.clear();
			g_noticeBaseUntil = 0;
		}
		std::lock_guard lock(g_pvpLock);
		g_duelLoaded.clear();
		g_game = nullptr;
		g_bodies.clear();
		g_heldPets.clear();
		PublishHeldLocked();
		g_heldCount = 0;
		g_down.clear();
		g_downCount = 0;
		g_scores.clear();
		g_pairs.clear();
		g_lastLevel.clear();
		g_lastTown.clear();
		g_arenaCount.clear();
		g_returns.clear();
		g_alone.clear();
		g_scoreSent.clear();
		g_invites.clear();
		g_duels.clear();
		g_frozen.clear();
		g_frozenCount = 0;
		g_duelBusy    = false;
		g_peers.clear();
		g_localReturnGuid = 0;
		g_returnUntil     = 0;
		g_lastHitBy.clear();
		g_localDownGuid  = 0;
		g_countdownUntil = 0;
	} else if (ev != nullptr && ev->kind == D2RL::Lifecycle::GameplayEventKind::GameJoined) {
		g_helloGuid = 0;
		StartUiPoll();
		g_tickEpoch.fetch_add(1); // anything queued meanwhile may be gone: its work is in flags and lists, so run a fresh tick
		QueueTick();
	}
}

auto StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* cmd, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (cmd == nullptr || cmd->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	std::string levels;
	for (const auto& [id, a] : g_config.arenas) {
		char part[64] {};
		std::snprintf(part, sizeof(part), "%s%d (%zu gates, %zu trapdoors)", levels.empty() ? "" : ", ", id, a.gates.size(), a.trapdoors.size());
		levels += part;
	}
	const size_t      argsLen = cmd->args == nullptr ? 0 : cmd->argsLength != 0 ? cmd->argsLength : std::strlen(cmd->args);
	const std::string args    = cmd->args != nullptr ? Trim(std::string(cmd->args, argsLen)) : std::string();
	if (args == "scores") {
		ScoresCommand(cmd);
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (args.rfind("wager", 0) == 0) { // 1.1.0: challenge; the stakes are set in the wager window after the accept
		const std::string name = Trim(args.substr(5));
		if (name.empty()) {
			cmd->plugin->WriteConsoleMessage("arena wager <character name>: challenge that player to a wager duel; on accept the wager window opens for both.");
			return D2RL::ConsoleCommandResult::Handled;
		}
		const std::string err = SendDuelCmd(kDuelWager, name);
		cmd->plugin->WriteConsoleMessage(err.empty() ? "arena: sent." : ("arena: " + err).c_str());
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (args.rfind("duel", 0) == 0 || args == "accept" || args == "decline" || args == "cancel") {
		uint8_t     kind = kDuelAccept;
		std::string name;
		if (args.rfind("duel", 0) == 0) {
			name = Trim(args.substr(4));
			kind = kDuelChallenge;
			if (name.empty()) {
				cmd->plugin->WriteConsoleMessage("arena duel <character name> [gold]: challenge that player to a duel, optionally for gold each.");
				return D2RL::ConsoleCommandResult::Handled;
			}
		} else if (args == "decline") {
			kind = kDuelDecline;
		} else if (args == "cancel") {
			kind = kDuelCancel;
		}
		const std::string err = SendDuelCmd(kind, name);
		cmd->plugin->WriteConsoleMessage(err.empty() ? "arena: sent." : ("arena: " + err).c_str());
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (args == "scoreboard") {
		ToggleScoreboard();
		cmd->plugin->WriteConsoleMessage(!g_scoreOn ? "arena scoreboard: not available (see the log)."
		                                 : g_scoreHidden.load() ? "arena scoreboard: hidden." : "arena scoreboard: shown (in arena levels).");
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (args == "leave") { // test helper until the duel flow ends fights: the hosting player leaves the arena
		void* me = g_localPlayer != nullptr ? g_localPlayer(g_localIndex()) : nullptr;
		if (me == nullptr || g_threads == nullptr) {
			cmd->plugin->WriteConsoleMessage("arena leave: not in a game.");
			return D2RL::ConsoleCommandResult::Handled;
		}
		bool host = false;
		{
			std::lock_guard lock(g_pvpLock);
			host = g_game != nullptr;
		}
		if (!host) {
			cmd->plugin->WriteConsoleMessage("arena leave: only the player who hosts the game can use it.");
			return D2RL::ConsoleCommandResult::Handled;
		}
		g_leaveGuid = Field<uint32_t>(me, 0x08);
		QueueTick();
		cmd->plugin->WriteConsoleMessage("arena leave: back to Lut Gholein next to the arena's entrance.");
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (args == "lobby here" || (args.size() == 12 && args.rfind("lobby here ", 0) == 0 && args[11] >= '1' && args[11] <= '4')) {
		// 1.9.0 (host): try the team duel obelisk's spot next to you; 1.9.3: `arena lobby here 2` .. `4` for the other obelisks
		const int idx = args.size() == 12 ? args[11] - '1' : 0;
		void* me   = g_localPlayer != nullptr ? g_localPlayer(g_localIndex()) : nullptr;
		bool  host = false;
		{
			std::lock_guard lock(g_pvpLock);
			host = g_game != nullptr;
		}
		if (me == nullptr || !host || LevelOf(me) != g_config.entranceLevel) {
			cmd->plugin->WriteConsoleMessage("arena lobby here: only the host, standing in the entrance level (Lut Gholein).");
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (IsSinglePlayerGame()) {
			cmd->plugin->WriteConsoleMessage("arena lobby here: no team duel obelisks in a single player game; host a TCP/IP game.");
			return D2RL::ConsoleCommandResult::Handled;
		}
		int32_t xy[2] {};
		g_getCoords(me, xy);
		const int32_t x = xy[0] + 2, y = xy[1] + 2;
		g_lobbyHereIndex = idx;
		g_lobbyHere      = (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
		QueueTick();
		// the toml line with every spot as it will be (the tick applies it; same game thread rules as before)
		std::vector<std::pair<int32_t, int32_t>> spots = LobbySpots();
		if (static_cast<size_t>(idx) < spots.size()) {
			spots[idx] = { x, y };
		} else {
			spots.emplace_back(x, y);
		}
		std::string line;
		for (const auto& [sx, sy] : spots) {
			line += (line.empty() ? "" : ", ") + std::to_string(sx) + ", " + std::to_string(sy);
		}
		char msg[400] {};
		std::snprintf(msg, sizeof(msg), "arena lobby: obelisk %d at %d, %d. To keep them: [arena.lobby] spot = [%s] in arena.toml (an old one stays until the game restarts).",
		              static_cast<int>(std::min(static_cast<size_t>(idx), spots.size() - 1)) + 1, x, y, line.c_str());
		cmd->plugin->WriteConsoleMessage(msg);
		Log("%s", msg);
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (args == "where") { // the local player's level and sub-tile position (for [arena.entrance] / exit spots)
		void* me = g_localPlayer != nullptr ? g_localPlayer(g_localIndex()) : nullptr;
		if (me == nullptr) {
			cmd->plugin->WriteConsoleMessage("arena where: not in a game.");
			return D2RL::ConsoleCommandResult::Handled;
		}
		int32_t xy[2] {};
		g_getCoords(me, xy);
		int32_t ox = 0, oy = 0;
		void*   room = g_getRoom(me);
		void*   drlg = room != nullptr ? Field<void*>(room, kRoomDrlgRoom) : nullptr;
		void*   lvl  = drlg != nullptr ? Field<void*>(drlg, kDrlgRoomLevel) : nullptr;
		if (lvl != nullptr) {
			ox = Field<int32_t>(lvl, kLevelPos) * 5;
			oy = Field<int32_t>(lvl, kLevelPos + 4) * 5;
		}
		char where[200] {};
		std::snprintf(where, sizeof(where), "arena where: level %d, x %d, y %d (inside the level: %d, %d)", LevelOf(me), xy[0], xy[1], xy[0] - ox, xy[1] - oy);
		cmd->plugin->WriteConsoleMessage(where);
		Log("%s", where);
		return D2RL::ConsoleCommandResult::Handled;
	}
	char msg[512] {};
	std::snprintf(msg, sizeof(msg), "arena: active=%d pvp=%d levels: %s; gate jumps %u, trapdoor jumps %u, closed-partner messages %u, failed %u; "
	              "arena deaths %u, death screens blocked %u, network %s, countdown panel %s. `arena scores` lists the scores.",
	              g_installed, g_pvpInstalled, levels.empty() ? "none" : levels.c_str(), g_gateJumps.load(), g_trapJumps.load(), g_sealed.load(), g_failed.load(),
	              g_deathsHandled.load(), g_deathUiBlocked.load(), g_net != nullptr ? "on" : "off", g_panels == nullptr ? "off" : g_hudOn ? "on (HUD)" : "on (window)");
	cmd->plugin->WriteConsoleMessage(msg);
	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo kInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "arena",
	.name        = "Arena",
	.version     = "2.4.26",
	.author      = "Kain",
	.description = "Arena gates, paired trapdoors and PvP rules (no-loss respawn, teams, scores) for the restored Diablo II 1.10 arena maps.",
	.flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &kInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* ctx) noexcept -> bool {
	if (ctx == nullptr) {
		return false;
	}
	g_ctx = ctx;
	(void)ctx->EnsureConfig();
	std::vector<char> buf(16384);
	uint32_t          required = 0;
	if (!ctx->ReadConfig(buf.data(), static_cast<uint32_t>(buf.size()), &required) && required > buf.size()) {
		buf.resize(required + 1);
		(void)ctx->ReadConfig(buf.data(), static_cast<uint32_t>(buf.size()), &required);
	}
	ParseConfig(buf.data(), g_config);
	if (!g_config.enabled) {
		ctx->LogInfo("arena: disabled in config.");
		return true;
	}
	if (g_config.arenas.empty()) {
		ctx->LogWarn("arena: no [arena.level.<id>] sections in the config; nothing to do.");
		return true;
	}
	(void)ctx->RegisterConsoleCommand("arena", StatusCommand, "arena status; `arena scores` lists the scores of the current fights; `arena scoreboard` shows/hides the scoreboard; `arena duel <name> [gold]` / `arena wager <name>` / `arena accept` / `arena decline` / `arena cancel`; `arena where` prints your position; `arena leave` (host, test) leaves the arena.");
	const D2RL::LifecycleService* life = nullptr;
	if (ctx->QueryService(&life) == D2RL::ServiceQueryResult::Success && D2RL::HasLifecycleServiceField(life, D2RL::LifecycleServiceRequiredSize)) {
		const D2RL::Lifecycle::GameplayEventListener gameplay {
			.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
			.kind       = D2RL::Lifecycle::GameplayEventKind::GameLeft,
			.callback   = OnGameplayEvent,
		};
		D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
		if (life->registerGameplayEventListener(ctx, &gameplay, &handle) != D2RL::Lifecycle::Result::Success) {
			ctx->LogWarn("arena: no game-left listener; opened trapdoors are remembered until the game closes.");
		}
		const D2RL::Lifecycle::GameplayEventListener joined {
			.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
			.kind       = D2RL::Lifecycle::GameplayEventKind::GameJoined,
			.callback   = OnGameplayEvent,
		};
		D2RL::Lifecycle::ListenerHandle joinedHandle = D2RL::Lifecycle::InvalidHandle;
		(void)life->registerGameplayEventListener(ctx, &joined, &joinedHandle);
		const D2RL::Lifecycle::GameplayEventListener ready {
			.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
			.kind       = D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady,
			.callback   = OnGameplayEvent,
		};
		D2RL::Lifecycle::ListenerHandle readyHandle = D2RL::Lifecycle::InvalidHandle;
		if (life->registerGameplayEventListener(ctx, &ready, &readyHandle) != D2RL::Lifecycle::Result::Success) {
			ctx->LogWarn("arena: no local-player-ready listener; the countdown HUD opens with the first countdown.");
		}
	}
	if (!Install(ctx)) {
		return true; // stays loaded, does nothing (see the log)
	}
	const bool pvp = InstallPvp(ctx);
	char msg[192] {};
	std::snprintf(msg, sizeof(msg), "arena: gates and trapdoors installed for %zu arena level(s); PvP %s (network %s, countdown panel %s).",
	              g_config.arenas.size(), pvp ? "on" : "off", g_net != nullptr ? "on" : "off", g_panels == nullptr ? "off" : g_hudOn ? "on (HUD)" : "on (window)");
	ctx->LogInfo(msg);
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	std::lock_guard lock(g_stateLock);
	g_opened.clear();
}
