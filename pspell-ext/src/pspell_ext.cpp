// pspell-ext - D2RLoader plugin
//
// Two new item pSpells that cast a skill when the item is used (right-click in
// the inventory or the belt), like Diablo 1 skill scrolls. The item is consumed
// the same way a potion or a scroll is.
//
//   pSpell = target slot   (default 16, D2MOO pSpell 10 "cast on target")
//       Casts the calc1 skill on the nearest enemy within search_radius. With no
//       enemy in range it casts at a point front_distance subtiles in front of
//       the player, along the direction the player faces (the direction they
//       are travelling).
//   pSpell = position slot (default 17, D2MOO pSpell 11 "cast to coordinates")
//       Casts the calc1 skill at the player's position. (D2R's use request no
//       longer passes a position to the item spell.)
//
//   calc1 = skill id (skills.txt *Id). calc2 = skill level (blank or < 1 = 1).
//   The skill needs an ItemEffect in skills.txt (the same rule as chance-to-cast);
//   without one the item is not used.
//
// Neither works in town: the item is not used and the player gets the voice line
// the client plays for a skill that is not allowed in town (sound event 0x19).
//
// How it works: D2R splits D2MOO's SKILLITEM_pSpell_Handler in two. The use-item
// handler first calls SKILLITEM_pSpell_PrepareHandler (false = the item is not
// used), then moves the item, calls SKILLITEM_pSpell_DoHandler and consumes it.
// Both look up the item's pSpell in gpPSpellDescTable (20 records of
// {checkIdx, doIdx, flag}); pSpell 16-19 have empty records whose index 0 points
// at no function, and 20+ has no record (client and server treat the item as not
// usable). The plugin hooks the two lookups to serve empty records for 20-30, and
// both handlers at entry: for pSpell 16-30 it runs its own check and cast and never
// reaches the tables; every other pSpell goes to the original code.
//
// All RVAs are relative to the D2RLoader.exe image (ctx->exeBase), read from
// /d2rloader_full.dmp (notes/pspell-ext.md).

#include <D2RLPlugin/api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <type_traits>

// ---------------------------------------------------------------------------
// RE-supplied constants (D2RLoader.exe RVAs, offsets, entry bytes).
// ---------------------------------------------------------------------------
using ByteSpan = std::span<const uint8_t>;

namespace Rva {
// DATATBLS_GetItemsTxtRecord(uchar nTableSet, int nItemId) (gamble-tweaks).
inline constexpr uint64_t GetItemsTxtRecord = 0x314110;
// DATATBLS_GetItemPSpellDescriptor(uchar nTableSet, pItem) -> &gpPSpellDescTable[pSpell] or NULL.
inline constexpr uint64_t GetPSpellDescriptor = 0x300AE0;
// gpPSpellDescTable (data): 20 x {int checkIdx; int doIdx; int flag}.
inline constexpr uint64_t PSpellDescTable = 0x236FE30;
// SKILLITEM_pSpell_PrepareHandler(pGame, pUnit, pItem, pTargetItem) -> bool. Hooked.
inline constexpr uint64_t PrepareHandler = 0x5817F0;
// SKILLITEM_pSpell_DoHandler(pGame, pUnit, pItem, pTargetItem). Hooked.
inline constexpr uint64_t DoHandler = 0x581680;
// ITEMMODS_EvaluateItemFormula(uchar nTableSet, pUnit, pItem, uint nCalc).
inline constexpr uint64_t EvaluateItemFormula = 0x3B4880;
// DATATBLS_GetSkillsTxtRecord(uchar nTableSet, int nSkillId).
inline constexpr uint64_t GetSkillsTxtRecord = 0x097790;
// SKILLITEM_CastSkillOnTarget(pUnit, nSkillId, nSkillLevel, pTarget, a5).
inline constexpr uint64_t CastSkillOnTarget = 0x5896E0;
// SKILLITEM_CastSkillOnPosition(pUnit, nSkillId, nSkillLevel, nX, nY, a6).
inline constexpr uint64_t CastSkillOnPosition = 0x589820;
// UNITFINDS_GetNearestTestedUnit(uchar nTableSet, pUnit, nX, nY, nSize, pfnUnitTest).
inline constexpr uint64_t GetNearestTestedUnit = 0x3B28D0;
// STATLIST_GetUnitAlignment(pUnit): FF 25 redirect to D2RCore ReadWideUnitAlignment.
inline constexpr uint64_t GetUnitAlignment = 0x2F4190;
// DUNGEON_IsRoomInTown(pRoom).
inline constexpr uint64_t IsRoomInTown = 0x2F0750;
// SUNIT_AttachSound(pUnit, nSoundEvent, pUpdateUnit).
inline constexpr uint64_t AttachSound = 0x491960;
// STATES_CheckState(pUnit, nState) (finisher-ext).
inline constexpr uint64_t CheckState = 0x3351B0;
// SKILLS_TestLineOfSight(nX, nY, pUnit, nColMask) = D2MOO D2Common_11026; TRUE = clear.
inline constexpr uint64_t TestLineOfSight = 0x33A870;
// ITEMS_IsPSpellUsableWithoutTarget(pItem): descriptor exists and flag == 0. Hooked.
inline constexpr uint64_t IsPSpellUsableNoTarget = 0x308E80;
// maybe_D2CLIENT_UI_DrawHoverUnitLabel(pUnit): draws the name / health bar of the unit
// under the mouse or picked by controller targeting. Hooked (client) to remember it.
inline constexpr uint64_t DrawHoverUnitLabel = 0x0C0420;
// SUNIT_GetServerUnit(pGame, nUnitType, nUnitGUID).
inline constexpr uint64_t GetServerUnit = 0x48FE80;
// --- Red portal pSpell ---
// maybe_D2GAME_CreatePortalObject(pGame, pUnit, pRoom, x, y, nDestLevel, ppPortal, nObjectId,
// bPerm, bSkipTownCheck); same act only.
inline constexpr uint64_t CreatePortalObject = 0x432CE0;
// maybe_DATATBLS_GetLevelsTxtRecord(nTableSet, nLevelId); +0x0D = act.
inline constexpr uint64_t GetLevelsTxtRecord = 0x32C4A0;
// maybe_DUNGEON_GetLevelIdFromRoom(pRoom).
inline constexpr uint64_t GetLevelIdFromRoom = 0x2EFC10;
// maybe_DUNGEON_GetTownLevelIdFromActNo(nTableSet, nAct).
inline constexpr uint64_t GetTownLevelIdFromActNo = 0x2EF990;
// maybe_WAYPOINTS_GetWaypointNoFromLevelId(nTableSet, nLevelId, &wWaypointNo) (levels.txt +0xEC).
inline constexpr uint64_t GetWaypointNoFromLevelId = 0x3D6670;
// maybe_LEVEL_WarpUnit(pGame, pPlayer, nLevelId, nTileCalc): changes act when needed.
inline constexpr uint64_t WarpUnit = 0x488180;
// maybe_SUNIT_AllocUnitData(nType, nClassId, x, y, pGame, pRoom, 1, 1, 0, 0).
inline constexpr uint64_t AllocUnitData = 0x490410;
// maybe_D2GAME_GetRoom(pRoom, x, y).
inline constexpr uint64_t GetRoomAt = 0x38DFB0;
// maybe_COLLISION_GetFreeCoordinatesWithField(pRoom, pCoordIn, pCoordOut, nSize, nMask, nField, 0).
inline constexpr uint64_t GetFreeCoordinates = 0x3632F0;
// UNITS_ChangeAnimMode(pUnit, nMode).
inline constexpr uint64_t ChangeAnimMode = 0x34E5F0;
// UNITS_GetPlayerData(pUnit): +0x58 + 8 * difficulty = waypoint flags.
inline constexpr uint64_t GetPlayerData = 0x34B240;
// Object operate function 15 (town portal / red portal), gpObjOperateFnTable[15] at 0x1423a5238. Hooked.
inline constexpr uint64_t PortalOperate = 0x58F680;
// maybe_sub_6FCBDFE0(pGame, pUnit, pRoom, x, y, 0, 0): move a unit inside its act (portal operate).
inline constexpr uint64_t TeleportUnit = 0x491CA0;
// UNITS_GetRoom(pUnit).
inline constexpr uint64_t GetUnitRoom = 0x34B440;
// maybe_OBJECT_SetInteractType(pObject, uint8 nLevelId): a portal's destination (label and
// operate). ACT5Q7_CreateUberAncientsPortal calls it after CreatePortalObject, because the
// PermanentPortal init only knows hardcoded pairs (Rogue Encampment -> Cow Level, ...).
inline constexpr uint64_t SetInteractType = 0x34E9D0;
// Skill tome pSpell: the checks and the spend of D2GAME_PACKETCALLBACK_Rcv0x3B (0x1404b3ee0).
// D2GAME_CheckSkillIfUnitsClassSkill(pUnit, nSkillId): skills.txt charclass == player class.
inline constexpr uint64_t CheckClassSkill = 0x436BD0;
// SKILLS_CheckRequiredSkills(pUnit, nSkillId): reqlevel (+ hard level) and reqskill1-3.
inline constexpr uint64_t CheckRequiredSkills = 0x339330;
// SKILLS_CheckRequiredAttributes(pUnit, nSkillId): InGame flag, required level, str/dex/int/vit.
inline constexpr uint64_t CheckRequiredAttributes = 0x339C00;
// SKILLS_GetSkillById(pUnit, nSkillId, -1).
inline constexpr uint64_t GetSkillById = 0x33DCD0;
// SKILLS_GetSkillLevel(pUnit, pSkill, bBonus, 0); 0 = hard points (the 0x3B maxlvl test).
inline constexpr uint64_t GetSkillLevel = 0x33D1E0;
// SKILLS_EvaluateSkillFormula(nTableSet, pUnit, nCalc, nSkillId, nSkillLevel).
inline constexpr uint64_t EvaluateSkillFormula = 0x3B5160;
// STATLIST_GetUnitBaseStat(pUnit, nStat, nLayer) / STATLIST_AddUnitStat(pUnit, nStat, nValue, nLayer).
inline constexpr uint64_t GetUnitBaseStat = 0x2F48C0;
inline constexpr uint64_t AddUnitStat     = 0x2F34F0;
// STATLIST_GetUnitStat(pUnit, nStat, nLayer): FF 25 stub to D2RCore ReadWideUnitStat (ordinal 1243), which reads
// the combined stat array (statlist +0xA8 when extended) instead of the unit's own one (+0x30): item properties count.
inline constexpr uint64_t GetUnitStat     = 0x2F5020;
// sub_6FD136E0(nTableSet, pUnit, nSkillId): pay skpoints from STAT_SKILLPTS, SKILLS_AddSkill,
// refresh passives/pets, server start function, packet 0x21, passive state, refresh.
inline constexpr uint64_t SpendSkillPoint = 0x438670;
// Waypoint bit table (data): 0x70 x {u16 word index, u16 mask} (maybe_WAYPOINTS_ActivateWaypoint).
inline constexpr uint64_t WaypointBitTable = 0x2389010;
// Client data: local player slot (dword) and the per-slot player GUID array (dword[8], -1 = none).
inline constexpr uint64_t ClientLocalPlayerSlot = 0x2A23704;
inline constexpr uint64_t ClientPlayerGuids     = 0x2A238F0;
// --- Summon / ambush pSpells ---
// sub_6FC6E8D0(pGame, pRoom, pCoordList, nMonsterId, bChampionAllowed, u16 x, u16 y, nPlacement, u16 nFlags):
// one champion or unique monster plus 3-6 minions; the Evil Urn calls it (pGame, pRoom, 0, id, 1, 0, 0, 1, 0).
inline constexpr uint64_t SpawnBossPack = 0x499880;
// D2GAME_SpawnSuperUnique(pGame, pRoom, x, y, nSuperUnique, u16 nFlags) -> pMonster or NULL. Hooked
// (the game's own Diablo Clone check sits at its start: pGame+0x1A5D4 == 1 spawns monster 0x14D instead).
inline constexpr uint64_t SpawnSuperUnique = 0x499BA0;
// UNITS_GetMonStats2TxtRecordFromMonsterId(nTableSet, nMonsterId); +0xF0 & 4 = can walk (urn test).
inline constexpr uint64_t GetMonStats2TxtRecord = 0x0975B0;
// --- Message scroll pSpell ---
// QUESTS_SendScrollMessage(pGame, pPlayer, pUnit, u16 nString): packet 0x27 type 2 {GUID of pUnit, 1 message
// {menu 0, nString}}; the client's maybe_CLIENT_HandleScrollMessagePacket27 (0x14019a630) passes it to
// maybe_CLIENT_ShowScrollMessageById (0x140197bf0), which opens the NPC speech panel (ScrollSpeechPanel).
inline constexpr uint64_t SendScrollMessage = 0x517E90;
// GetNamespacedStringId({const char*, size_t} key) -> u16 string id: .maho stub (FF 25) to D2RCore ordinal 1203,
// the call the txt compiler makes for KEYTOWORD cells ("pspell-ext:Key" works the same way here).
inline constexpr uint64_t GetNamespacedStringId = 0x3E2B724;
} // namespace Rva

// Entry bytes checked before a function is used or hooked (whole instructions,
// no RIP-relative operands; read from /d2rloader_full.dmp).
namespace Expected {
// PUSH RDI; SUB RSP,0x30; MOV EDI,EDX
inline constexpr uint8_t kGetItemsTxtRecord[] = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x8B, 0xFA };
// MOV [RSP+8],RBX; MOV [RSP+0x10],RSI; MOV [RSP+0x18],RDI
inline constexpr uint8_t kGetPSpellDescriptor[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18 };
// MOV [RSP+8],RBX; MOV [RSP+0x10],RBP; MOV [RSP+0x18],RSI (hook: 15 bytes)
inline constexpr uint8_t kPrepareHandler[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 };
// MOV [RSP+0x18],RBX; MOV [RSP+0x20],RBP; PUSH RSI; PUSH RDI; PUSH R15; SUB RSP,0x30 (hook: 18 bytes)
inline constexpr uint8_t kDoHandler[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30 };
// MOV [RSP+8],RBX; MOV [RSP+0x10],RBP; MOV [RSP+0x18],RSI
inline constexpr uint8_t kEvaluateItemFormula[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 };
// MOV [RSP+8],RBX; MOV [RSP+0x20],RSI; PUSH RDI; SUB RSP,0x30
inline constexpr uint8_t kGetSkillsTxtRecord[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x30 };
// MOV [RSP+0x10],RBX; MOV [RSP+0x18],RBP; MOV [RSP+0x20],RSI
inline constexpr uint8_t kCastSkillOnTarget[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20 };
// MOV [RSP+0x10],RBX; MOV [RSP+0x18],RBP; PUSH RSI; PUSH RDI
inline constexpr uint8_t kCastSkillOnPosition[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57 };
// PUSH RBX; PUSH RBP; PUSH RSI; PUSH RDI; PUSH R13; PUSH R14; PUSH R15; SUB RSP,0x90
inline constexpr uint8_t kGetNearestTestedUnit[] = { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00 };
// JMP qword ptr [.maho slot] (D2RCore redirect stub, notes/maho-redirects.tsv)
inline constexpr uint8_t kGetUnitAlignment[] = { 0xFF, 0x25, 0x92, 0x61, 0xB3, 0x03 };
// SUB RSP,0x28; TEST RCX,RCX; JNZ +7
inline constexpr uint8_t kIsRoomInTown[] = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x07 };
// MOV [RSP+8],RBX; MOV [RSP+0x18],RSI; PUSH RDI; SUB RSP,0x20
inline constexpr uint8_t kAttachSound[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20 };
// MOV [RSP+8],RBX; MOV [RSP+0x10],RSI; PUSH RDI; SUB RSP,0x20
inline constexpr uint8_t kCheckState[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
// MOV [RSP+0x10],RBX; MOV [RSP+0x20],RBP; PUSH RSI; PUSH RDI
inline constexpr uint8_t kTestLineOfSight[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x57 };
// MOV [RSP+8],RBX; MOV [RSP+0x10],RSI; MOV [RSP+0x18],RDI (hook: 15 bytes)
inline constexpr uint8_t kIsPSpellUsableNoTarget[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18 };
// MOV [RSP+0x10],RBX; MOV [RSP+0x18],RSI; MOV [RSP+0x20],RDI (hook: 15 bytes)
inline constexpr uint8_t kDrawHoverUnitLabel[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20 };
// MOV [RSP+8],RBX; MOV [RSP+0x18],RSI; PUSH RDI; SUB RSP,0x20
inline constexpr uint8_t kGetServerUnit[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20 };
// MOV RAX,RSP; MOV [RAX+0x20],RBP; PUSH RSI; PUSH RDI; PUSH R13; PUSH R14; PUSH R15
inline constexpr uint8_t kCreatePortalObject[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x68, 0x20, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
// MOV [RSP+8],RBX; PUSH RBP; PUSH RSI; PUSH RDI; SUB RSP,0x30
inline constexpr uint8_t kGetLevelsTxtRecord[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30 };
// TEST RCX,RCX; JNZ +3; XOR EAX,EAX
inline constexpr uint8_t kGetLevelIdFromRoom[] = { 0x48, 0x85, 0xC9, 0x75, 0x03, 0x33, 0xC0 };
// MOV [RSP+8],RBX; PUSH RDI; SUB RSP,0x30
inline constexpr uint8_t kGetTownLevelIdFromActNo[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30 };
// MOV [RSP+8],RBX; MOV [RSP+0x18],RBP; PUSH RSI; PUSH RDI; PUSH R14
inline constexpr uint8_t kGetWaypointNoFromLevelId[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56 };
// MOV RAX,RSP; MOV [RAX+0x10],RBX; MOV [RAX+0x18],RBP; PUSH RSI; PUSH RDI
inline constexpr uint8_t kWarpUnit[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x56, 0x57 };
// PUSH RBP; PUSH RBX; PUSH RDI
inline constexpr uint8_t kAllocUnitData[] = { 0x40, 0x55, 0x53, 0x57 };
// MOV [RSP+0x10],RBX; PUSH RBP; PUSH RSI; PUSH RDI; SUB RSP,0x20
inline constexpr uint8_t kGetRoomAt[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20 };
// SUB RSP,0x58
inline constexpr uint8_t kGetFreeCoordinates[] = { 0x48, 0x83, 0xEC, 0x58 };
// MOV [RSP+0x18],RBX; PUSH RDI; SUB RSP,0x20
inline constexpr uint8_t kChangeAnimMode[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20 };
// PUSH RBX; SUB RSP,0x20
inline constexpr uint8_t kGetPlayerData[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };
// PUSH RBP; PUSH RBX; PUSH RSI; PUSH RDI; PUSH R12; PUSH R13; PUSH R14; PUSH R15
inline constexpr uint8_t kTeleportUnit[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
// PUSH RBX; SUB RSP,0x20; MOV RBX,RCX
inline constexpr uint8_t kGetUnitRoom[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };
// MOV [RSP+8],RBX; PUSH RDI; SUB RSP,0x20
inline constexpr uint8_t kSetInteractType[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
// MOV [RSP+0x10],RBX; PUSH RDI; SUB RSP,0x20
inline constexpr uint8_t kCheckClassSkill[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
// MOV [RSP+8],RBX; MOV [RSP+0x10],RBP; MOV [RSP+0x18],RSI; MOV [RSP+0x20],RDI
inline constexpr uint8_t kCheckRequiredSkills[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20 };
// MOV [RSP+0x10],RBP; MOV [RSP+0x18],RSI; PUSH RDI; SUB RSP,0x20
inline constexpr uint8_t kCheckRequiredAttributes[] = { 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20 };
// MOV [RSP+8],RBX; PUSH RDI; SUB RSP,0x20
inline constexpr uint8_t kGetSkillById[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
// JMP qword ptr [.maho slot] (D2RCore redirect stubs, like kGetUnitAlignment)
inline constexpr uint8_t kGetSkillLevel[]   = { 0xFF, 0x25, 0x8A, 0xD1, 0xAE, 0x03 };
inline constexpr uint8_t kGetUnitBaseStat[] = { 0xFF, 0x25, 0x4A, 0x59, 0xB3, 0x03 };
inline constexpr uint8_t kAddUnitStat[]     = { 0xFF, 0x25, 0x5A, 0x6D, 0xB3, 0x03 };
inline constexpr uint8_t kGetUnitStat[]     = { 0xFF, 0x25, 0xF2, 0x51, 0xB3, 0x03 };
// MOV [RSP+8],RBX; MOV [RSP+0x10],RBP; MOV [RSP+0x20],RSI; PUSH RDI; PUSH R14; PUSH R15
inline constexpr uint8_t kEvaluateSkillFormula[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57 };
// MOV [RSP+0x18],RBX; MOV [RSP+0x20],RBP; PUSH RDI; PUSH R14; PUSH R15; SUB RSP,0x30
inline constexpr uint8_t kSpendSkillPoint[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30 };
// MOV [RSP+8],RCX; PUSH RBP; PUSH RBX; PUSH RSI; PUSH RDI; PUSH R14; PUSH R15; MOV RBP,RSP (hook: 16 bytes)
inline constexpr uint8_t kPortalOperate[] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC };
// MOV [RSP+0x10],RDX; PUSH RBP; PUSH R12; PUSH R13; PUSH R15
inline constexpr uint8_t kSpawnBossPack[] = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57 };
// PUSH RBP; PUSH RBX; PUSH RSI; PUSH RDI; PUSH R14; PUSH R15; LEA RBP,[RSP-0xB8] (hook: 17 bytes)
inline constexpr uint8_t kSpawnSuperUnique[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x48, 0xFF, 0xFF, 0xFF };
// MOV [RSP+8],RBX; PUSH RBP; PUSH RSI; PUSH RDI; SUB RSP,0x30
inline constexpr uint8_t kGetMonStats2TxtRecord[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30 };
// MOV [RSP+8],RBX; MOV [RSP+0x20],RSI; PUSH RDI; SUB RSP,0x50
inline constexpr uint8_t kSendScrollMessage[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x50 };
// JMP qword ptr [0x143e2ae00] (jump table entry; the slot holds D2RCore GetNamespacedStringId)
inline constexpr uint8_t kGetNamespacedStringId[] = { 0xFF, 0x25, 0xD6, 0xF6, 0xFF, 0xFF };
} // namespace Expected

// gpPSpellDescTable has records for pSpell 0-19; 16-19 are empty. For 20-30 the
// plugin hooks the two lookups (DATATBLS_GetItemPSpellDescriptor and
// ITEMS_IsPSpellUsableWithoutTarget, both bound 0x13) and serves its own empty records.
namespace Table {
inline constexpr uint32_t DescCount      = 20;
inline constexpr uint32_t DescStride     = 12;
inline constexpr uint32_t FirstFreeSlot  = 16;
inline constexpr uint32_t FirstExtraSlot = 20;
inline constexpr uint32_t LastSlot       = 30;
} // namespace Table

// COLLIDE_DOOR | COLLIDE_MISSILE_BARRIER: SKILLS_SrvStartDispatch's mask for
// nLineOfSight 4 (D2MOO SKILLSLOS_PLAYER_FLYING), the path a missile takes.
inline constexpr uint32_t kLineOfSightMask = 0x804;

// D2GameStrc / D2UnitStrc / D2DynamicPathStrc / D2ItemsTxt / D2SkillsTxt offsets.
namespace Off {
inline constexpr size_t GameDataTableSet = 0x106; // byte; both pSpell handlers pass it to the table getters
inline constexpr size_t UnitType         = 0x00;  // dwUnitType
inline constexpr size_t UnitClassId      = 0x04;  // dwClassId
inline constexpr size_t UnitGuid         = 0x08;  // dwUnitId (SUNIT_GetServerUnit compares it)
inline constexpr size_t UnitAnimMode     = 0x0C;  // dwAnimMode
inline constexpr size_t UnitPath         = 0x38;  // pDynamicPath (players and monsters)
inline constexpr size_t UnitFlags        = 0x124; // dwFlags
inline constexpr size_t UnitFlagsEx      = 0x128; // dwFlagEx (UNITS_SetFlagEx)
inline constexpr size_t UnitDataTableSet = 0x1BD; // byte, maybe_UNITS_GetDataTableSet (d2unit.md)
inline constexpr size_t ItemsTxtPSpell   = 0x94;  // D2ItemsTxt dwPSpell (DATATBLS_GetItemPSpellDescriptor)
inline constexpr size_t PathPosX         = 0x02;  // wPosX (PATH_GetXPosition)
inline constexpr size_t PathPosY         = 0x06;  // wPosY (PATH_GetYPosition)
inline constexpr size_t PathRoom         = 0x20;  // pRoom (PATH_GetRoom)
inline constexpr size_t PathDirection    = 0x80;  // fDirection, 0..64 (PATH_GetDirection)
// D2ItemsTxt dwCalc[0] / dwCalc[1]: pSpell 9 pairs wStat[3] +0x9E with dwCalc[3] +0xA4,
// pSpell 15 reads calc1 at +0xA4.
inline constexpr size_t ItemsTxtCalc1 = 0xA4;
inline constexpr size_t ItemsTxtCalc2 = 0xA8;
inline constexpr size_t ItemsTxtCalc3 = 0xAC;
// D2SkillsTxt wItemEffect (SKILLITEM_HandleItemEffectSkill refuses 0).
inline constexpr size_t SkillsTxtItemEffect = 0x20A;
// Red portal pSpell.
inline constexpr size_t GameDifficulty     = 0x104; // byte; quest/waypoint data index (NPC travel handler)
inline constexpr size_t PlayerDataWaypoints = 0x58; // + 8 * difficulty: waypoint flag words (NPC travel handler)
inline constexpr size_t LevelsTxtAct       = 0x0D; // byte
inline constexpr size_t ObjectPathX        = 0x10; // static path (unit +0x38) x / y, as the portal code reads them
inline constexpr size_t ObjectPathY        = 0x14;
// Skill tome pSpell (D2SkillsTxt, stride 0x2EC).
inline constexpr size_t SkillsTxtMaxLevel  = 0x184; // int16 wMaxLvl; <= 0 means 20 (Rcv0x3B)
inline constexpr size_t SkillsTxtSkPoints  = 0x210; // dwSkPoints formula, -1 = cost 1 (sub_6FD136E0)
// Summon / ambush pSpells: the Evil Urn's monster pick (OBJECTS_OperateFunction68_EvilUrn).
inline constexpr size_t GameMonsterRegions = 0x1D0; // D2MonsterRegionStrc* per level id (MONSTERREGION_GetMonsterRegionFromLevelId)
inline constexpr size_t MonRegCount        = 0x10;  // byte nMonCount
inline constexpr size_t MonRegData         = 0x14;  // D2MonRegDataStrc[nMonCount], int16 nMonHcIdx first
inline constexpr size_t MonRegDataStride   = 0x34;
inline constexpr size_t MonStats2ModeFlags = 0xF0;  // byte; & gdwBitMasks[MONMODE_WALK] (4)
} // namespace Off

inline constexpr uint32_t kUnitTypePlayer  = 0;
inline constexpr uint32_t kUnitTypeMonster = 1;
// dwFlags bits (D2MOO Units.h). CANBEATTACKED / ISVALIDTARGET: UNITFINDS_TestUnit's
// getters for find flags 0x80 / 0x400; TARGETABLE by the same bit layout;
// ISDEAD: SUNIT_IsDead.
inline constexpr uint32_t kUnitFlagTargetable    = 0x00000002;
inline constexpr uint32_t kUnitFlagCanBeAttacked = 0x00000004;
inline constexpr uint32_t kUnitFlagIsValidTarget = 0x00000008;
inline constexpr uint32_t kUnitFlagIsDead        = 0x00010000;
// dwFlagEx bit the chance-to-cast event functions set on the caster around
// SKILLITEM_CastSkillOnTarget/Position. Its only reader, maybe_PLAYERPETS_AddPetRecord,
// marks pets summoned while it is set (record flag 2): item-event summons.
inline constexpr uint32_t kUnitFlagExItemEventCast = 0x10000000;
// Monster modes treated as dead (SUNIT_IsDead: MONMODE_DEATH 0, MONMODE_DEAD 0xC).
inline constexpr uint32_t kMonModeDeath = 0x00;
inline constexpr uint32_t kMonModeDead  = 0x0C;
inline constexpr int32_t  kAlignmentEvil = 0; // UNIT_ALIGNMENT_EVIL
// Unit sound events. The client plays 0x19 when a skill without InTown is used in
// town, and 0x14 when an item can't be used (also SKILLITEM_pSpell_PrepareHandler's own refusal).
inline constexpr int32_t kSoundTownRefused = 0x19;
inline constexpr int32_t kSoundCannotUse   = 0x14;
// STATE_UNINTERRUPTABLE: SKILLITEM_CastSkillOnTarget/Position refuse while it is on.
inline constexpr int32_t kStateUninterruptable = 0x36;
// The game sends skill ids as 16-bit values and skill levels as 8-bit values.
inline constexpr int32_t kMaxSkillId    = 0x7FFF;
inline constexpr int32_t kMaxSkillLevel = 255;
inline constexpr uint8_t kMonStats2CanWalk = 0x04; // gdwBitMasks[2] at 0x141d996d8, the urn's walk test

// ---------------------------------------------------------------------------
// Config.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kDefaultTargetSlot   = 16;
inline constexpr uint32_t kDefaultPositionSlot = 17;
inline constexpr uint32_t kDefaultPortalSlot   = 18;
inline constexpr uint32_t kDefaultTomeSlot     = 19;
inline constexpr uint32_t kDefaultSummonSlot   = 20;
inline constexpr uint32_t kDefaultAmbushSlot   = 21;
inline constexpr uint32_t kDefaultMessageSlot  = 22;
inline constexpr const char* kDefaultMessageFormat = "scrollMsg%04d";
inline constexpr uint32_t kMinSearchRadius     = 1;
inline constexpr uint32_t kMaxSearchRadius     = 100;
inline constexpr uint32_t kMinFrontDistance    = 1;
inline constexpr uint32_t kMaxFrontDistance    = 50;
inline constexpr uint32_t kMinSummonDistance   = 1;
inline constexpr uint32_t kMaxSummonDistance   = 100;

struct Config {
	bool     enabled       = true;
	uint32_t targetSlot    = kDefaultTargetSlot;
	bool     targetSlotSet = false;
	uint32_t positionSlot  = kDefaultPositionSlot;
	bool     positionSlotSet = false;
	uint32_t portalSlot    = kDefaultPortalSlot;
	bool     portalSlotSet = false; // portal_pspell given in the toml (a clash is then reported)
	uint32_t tomeSlot      = kDefaultTomeSlot;
	bool     tomeSlotSet   = false;
	uint32_t summonSlot    = kDefaultSummonSlot;
	bool     summonSlotSet = false;
	uint32_t ambushSlot    = kDefaultAmbushSlot;
	bool     ambushSlotSet = false;
	uint32_t messageSlot   = kDefaultMessageSlot;
	bool     messageSlotSet = false;
	std::string messageFormat = kDefaultMessageFormat; // string key built from calc1
	uint32_t messageStat   = 0; // itemstatcost row read when calc1 is blank / 0; 0 = off
	uint32_t searchRadius  = 40; // subtiles
	uint32_t frontDistance = 5;  // subtiles
	uint32_t summonDistance = 20; // subtiles, summon_pspell packs
	bool     lineOfSight   = true;
	bool     useHoverTarget = true; // target slot: prefer the monster under the mouse / controller target
	bool     diagnostics   = false; // info and warning lines in the log
};

static Config g_config;

namespace {

auto trim(const std::string& s) -> std::string {
	const size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return "";
	}
	const size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

auto parseBool(const std::string& v, bool& out) -> bool {
	const std::string t = trim(v);
	if (t == "true" || t == "1") {
		out = true;
		return true;
	}
	if (t == "false" || t == "0") {
		out = false;
		return true;
	}
	return false;
}

// Plain decimal integer, optionally quoted. Returns false for anything else.
auto parseInt(const std::string& v, long& out) -> bool {
	std::string t = trim(v);
	if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'') && t.back() == t.front()) {
		t = trim(t.substr(1, t.size() - 2));
	}
	if (t.empty()) {
		return false;
	}
	char* end        = nullptr;
	const long value = std::strtol(t.c_str(), &end, 10);
	if (end == nullptr || *end != '\0') {
		return false;
	}
	out = value;
	return true;
}

// A quoted or bare string value.
auto parseString(const std::string& v) -> std::string {
	std::string t = trim(v);
	if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'') && t.back() == t.front()) {
		t = t.substr(1, t.size() - 2);
	}
	return t;
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

} // namespace

// A message key format: 1-48 characters of letters, digits, '_', '-' and '.', with exactly
// one integer conversion %d, %u, %Nd or %0Nd (N 1-9) and no other '%'.
inline auto IsSafeMessageFormat(const std::string& f) -> bool {
	if (f.empty() || f.size() > 48) {
		return false;
	}
	int conversions = 0;
	for (size_t i = 0; i < f.size(); ++i) {
		const char c = f[i];
		if (c == '%') {
			size_t j = i + 1;
			if (j < f.size() && f[j] == '0') {
				++j;
			}
			if (j < f.size() && f[j] >= '1' && f[j] <= '9') {
				++j;
			}
			if (j >= f.size() || (f[j] != 'd' && f[j] != 'u')) {
				return false;
			}
			++conversions;
			i = j;
		} else if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.')) {
			return false;
		}
	}
	return conversions == 1;
}

// Out-of-range pSpell values fall back to 16 (target) and 17 (position). If the
// two end up equal, the position slot moves to 17, or to 16 when the target
// slot is 17.
template <typename WarnFn>
void NormalizeConfig(Config& cfg, WarnFn warn) {
	char msg[192] {};
	if (cfg.targetSlot < Table::FirstFreeSlot || cfg.targetSlot > Table::LastSlot) {
		std::snprintf(msg, sizeof(msg), "pspell-ext: target_pspell %u is outside %u-%u; using %u.", cfg.targetSlot, Table::FirstFreeSlot, Table::LastSlot, kDefaultTargetSlot);
		warn(msg);
		cfg.targetSlot = kDefaultTargetSlot;
	}
	if (cfg.positionSlot < Table::FirstFreeSlot || cfg.positionSlot > Table::LastSlot) {
		std::snprintf(msg, sizeof(msg), "pspell-ext: position_pspell %u is outside %u-%u; using %u.", cfg.positionSlot, Table::FirstFreeSlot, Table::LastSlot,
		              kDefaultPositionSlot);
		warn(msg);
		cfg.positionSlot = kDefaultPositionSlot;
	}
	if (cfg.positionSlot == cfg.targetSlot) {
		const uint32_t moved = cfg.targetSlot == kDefaultPositionSlot ? kDefaultTargetSlot : kDefaultPositionSlot;
		std::snprintf(msg, sizeof(msg), "pspell-ext: target_pspell and position_pspell are both %u; position_pspell moved to %u.", cfg.targetSlot, moved);
		warn(msg);
		cfg.positionSlot = moved;
	}
	// Portal, tome, summon, then ambush: out of range -> default; taken -> the next free slot from the
	// default up (wrapping), reported only when the key was set in the toml.
	auto placeSlot = [&](uint32_t& slot, bool wasSet, uint32_t def, const char* key, auto isTaken) {
		if (slot < Table::FirstFreeSlot || slot > Table::LastSlot) {
			std::snprintf(msg, sizeof(msg), "pspell-ext: %s %u is outside %u-%u; using %u.", key, slot, Table::FirstFreeSlot, Table::LastSlot, def);
			warn(msg);
			slot = def;
		}
		if (isTaken(slot)) {
			uint32_t moved = Table::FirstFreeSlot;
			for (uint32_t s = def; s <= Table::LastSlot + def - Table::FirstFreeSlot; ++s) {
				const uint32_t candidate = Table::FirstFreeSlot + (s - Table::FirstFreeSlot) % (Table::LastSlot - Table::FirstFreeSlot + 1);
				if (!isTaken(candidate)) {
					moved = candidate;
					break;
				}
			}
			if (wasSet) {
				std::snprintf(msg, sizeof(msg), "pspell-ext: %s %u is already used; moved to %u.", key, slot, moved);
				warn(msg);
			}
			slot = moved;
		}
	};
	placeSlot(cfg.portalSlot, cfg.portalSlotSet, kDefaultPortalSlot, "portal_pspell",
	          [&](uint32_t s) { return s == cfg.targetSlot || s == cfg.positionSlot; });
	placeSlot(cfg.tomeSlot, cfg.tomeSlotSet, kDefaultTomeSlot, "tome_pspell",
	          [&](uint32_t s) { return s == cfg.targetSlot || s == cfg.positionSlot || s == cfg.portalSlot; });
	placeSlot(cfg.summonSlot, cfg.summonSlotSet, kDefaultSummonSlot, "summon_pspell",
	          [&](uint32_t s) { return s == cfg.targetSlot || s == cfg.positionSlot || s == cfg.portalSlot || s == cfg.tomeSlot; });
	placeSlot(cfg.ambushSlot, cfg.ambushSlotSet, kDefaultAmbushSlot, "ambush_pspell", [&](uint32_t s) {
		return s == cfg.targetSlot || s == cfg.positionSlot || s == cfg.portalSlot || s == cfg.tomeSlot || s == cfg.summonSlot;
	});
	placeSlot(cfg.messageSlot, cfg.messageSlotSet, kDefaultMessageSlot, "message_pspell", [&](uint32_t s) {
		return s == cfg.targetSlot || s == cfg.positionSlot || s == cfg.portalSlot || s == cfg.tomeSlot || s == cfg.summonSlot || s == cfg.ambushSlot;
	});
	if (!IsSafeMessageFormat(cfg.messageFormat)) {
		std::snprintf(msg, sizeof(msg), "pspell-ext: message_format needs exactly one %%d (letters, digits, _ - . only); using %s.", kDefaultMessageFormat);
		warn(msg);
		cfg.messageFormat = kDefaultMessageFormat;
	}
	if (cfg.searchRadius < kMinSearchRadius || cfg.searchRadius > kMaxSearchRadius) {
		const uint32_t clamped = cfg.searchRadius < kMinSearchRadius ? kMinSearchRadius : kMaxSearchRadius;
		std::snprintf(msg, sizeof(msg), "pspell-ext: search_radius %u clamped to %u.", cfg.searchRadius, clamped);
		warn(msg);
		cfg.searchRadius = clamped;
	}
	if (cfg.frontDistance < kMinFrontDistance || cfg.frontDistance > kMaxFrontDistance) {
		const uint32_t clamped = cfg.frontDistance < kMinFrontDistance ? kMinFrontDistance : kMaxFrontDistance;
		std::snprintf(msg, sizeof(msg), "pspell-ext: front_distance %u clamped to %u.", cfg.frontDistance, clamped);
		warn(msg);
		cfg.frontDistance = clamped;
	}
	if (cfg.summonDistance < kMinSummonDistance || cfg.summonDistance > kMaxSummonDistance) {
		const uint32_t clamped = cfg.summonDistance < kMinSummonDistance ? kMinSummonDistance : kMaxSummonDistance;
		std::snprintf(msg, sizeof(msg), "pspell-ext: summon_distance %u clamped to %u.", cfg.summonDistance, clamped);
		warn(msg);
		cfg.summonDistance = clamped;
	}
}

// Minimal reader for the [pspell-ext] table (scalar keys only), then
// NormalizeConfig. Bad values are reported and keep their defaults.
template <typename WarnFn>
void ParseConfig(const char* toml, Config& out, WarnFn warn) {
	if (toml != nullptr) {
		const std::string text(toml);
		std::string section;
		size_t pos = 0;
		while (pos < text.size()) {
			const size_t nl  = text.find('\n', pos);
			std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
			pos              = (nl == std::string::npos) ? text.size() : nl + 1;
			line             = trim(stripComment(line));
			if (line.empty()) {
				continue;
			}
			if (line.front() == '[' && line.back() == ']' && line.find('=') == std::string::npos) {
				section = trim(line.substr(1, line.size() - 2));
				continue;
			}
			if (section != "pspell-ext") {
				continue;
			}
			const size_t eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			const std::string key = trim(line.substr(0, eq));
			const std::string val = trim(line.substr(eq + 1));

			auto readUnsigned = [&](uint32_t& field) {
				long value = 0;
				if (!parseInt(val, value)) {
					char msg[160] {};
					std::snprintf(msg, sizeof(msg), "pspell-ext: '%s' must be a whole number; keeping %u.", key.c_str(), field);
					warn(msg);
					return;
				}
				// Out-of-range values are reported and corrected by NormalizeConfig
				// (negative -> 0, which is below every allowed range).
				field = value < 0 ? 0U : (value > 0x7FFFFFFFL ? 0x7FFFFFFFU : static_cast<uint32_t>(value));
			};

			if (key == "enabled") {
				if (!parseBool(val, out.enabled)) {
					warn("pspell-ext: 'enabled' must be true or false; keeping the default.");
				}
			} else if (key == "line_of_sight") {
				if (!parseBool(val, out.lineOfSight)) {
					warn("pspell-ext: 'line_of_sight' must be true or false; keeping the default.");
				}
			} else if (key == "diagnostics") {
				if (!parseBool(val, out.diagnostics)) {
					warn("pspell-ext: 'diagnostics' must be true or false; keeping the default.");
				}
			} else if (key == "use_hover_target") {
				if (!parseBool(val, out.useHoverTarget)) {
					warn("pspell-ext: 'use_hover_target' must be true or false; keeping the default.");
				}
			} else if (key == "target_pspell") {
				readUnsigned(out.targetSlot);
				out.targetSlotSet = true;
			} else if (key == "position_pspell") {
				readUnsigned(out.positionSlot);
				out.positionSlotSet = true;
			} else if (key == "portal_pspell") {
				readUnsigned(out.portalSlot);
				out.portalSlotSet = true;
			} else if (key == "tome_pspell") {
				readUnsigned(out.tomeSlot);
				out.tomeSlotSet = true;
			} else if (key == "summon_pspell") {
				readUnsigned(out.summonSlot);
				out.summonSlotSet = true;
			} else if (key == "ambush_pspell") {
				readUnsigned(out.ambushSlot);
				out.ambushSlotSet = true;
			} else if (key == "message_pspell") {
				readUnsigned(out.messageSlot);
				out.messageSlotSet = true;
			} else if (key == "message_format") {
				out.messageFormat = parseString(val);
			} else if (key == "message_stat") {
				readUnsigned(out.messageStat);
			} else if (key == "search_radius") {
				readUnsigned(out.searchRadius);
			} else if (key == "front_distance") {
				readUnsigned(out.frontDistance);
			} else if (key == "summon_distance") {
				readUnsigned(out.summonDistance);
			}
		}
	}
	NormalizeConfig(out, warn);
}

// Offset of a point `distance` subtiles away along a D2 unit direction (0..64,
// 64 steps per turn). D2MOO PATH_GetDirectionVector gives direction 8 for +Y,
// 24 for -X, 40 for -Y and 56 for +X, so the angle from +X is (dir + 8) / 64 turns.
inline void DirectionOffset(float direction, int32_t distance, int32_t& dx, int32_t& dy) {
	constexpr double kStep = 6.283185307179586 / 64.0;
	const double angle     = (static_cast<double>(direction) + 8.0) * kStep;
	dx                     = static_cast<int32_t>(std::lround(std::cos(angle) * distance));
	dy                     = static_cast<int32_t>(std::lround(std::sin(angle) * distance));
}

// ---------------------------------------------------------------------------
// Native functions.
// ---------------------------------------------------------------------------
// The two hooked handlers. Prepare returns false when the item must not be used.
using PrepareHandlerFn       = bool(__fastcall*)(void* pGame, void* pUnit, void* pItem, void* pTargetItem);
using DoHandlerFn            = void(__fastcall*)(void* pGame, void* pUnit, void* pItem, void* pTargetItem);
using GetItemsTxtRecordFn    = const uint8_t*(__fastcall*)(uint8_t nTableSet, int32_t nItemId);
using GetPSpellDescriptorFn  = const uint8_t*(__fastcall*)(uint8_t nTableSet, void* pItem);
using EvaluateItemFormulaFn  = int32_t(__fastcall*)(uint8_t nTableSet, void* pUnit, void* pItem, uint32_t nCalc);
using GetSkillsTxtRecordFn   = const uint8_t*(__fastcall*)(uint8_t nTableSet, int32_t nSkillId);
using CastSkillOnTargetFn    = int32_t(__fastcall*)(void* pUnit, int32_t nSkillId, int32_t nSkillLevel, void* pTarget, int32_t a5);
using CastSkillOnPositionFn  = int32_t(__fastcall*)(void* pUnit, int32_t nSkillId, int32_t nSkillLevel, int32_t nX, int32_t nY, int32_t a6);
// D2R passes the table set through to the test callback (CL, then candidate, origin).
using UnitTestFn             = int32_t(__fastcall*)(uint8_t nTableSet, void* pCandidate, void* pOrigin);
using GetNearestTestedUnitFn = void*(__fastcall*)(uint8_t nTableSet, void* pUnit, int32_t nX, int32_t nY, int32_t nSize, UnitTestFn pfnUnitTest);
using GetUnitAlignmentFn     = int32_t(__fastcall*)(void* pUnit);
using IsRoomInTownFn         = int32_t(__fastcall*)(void* pRoom);
using AttachSoundFn          = void(__fastcall*)(void* pUnit, int32_t nSoundEvent, void* pUpdateUnit);
using CheckStateFn           = int32_t(__fastcall*)(void* pUnit, int32_t nState);
using TestLineOfSightFn      = int32_t(__fastcall*)(int32_t nX, int32_t nY, void* pUnit, uint32_t nColMask);
using IsUsableNoTargetFn     = bool(__fastcall*)(void* pItem);
using DrawHoverUnitLabelFn   = void(__fastcall*)(void* pUnit, void* a2, void* a3, void* a4);
using GetServerUnitFn        = void*(__fastcall*)(void* pGame, uint32_t nUnitType, uint32_t nUnitGuid);
using CreatePortalObjectFn   = int32_t(__fastcall*)(void* pGame, void* pUnit, void* pRoom, int32_t nX, int32_t nY, int32_t nDestLevel, void** ppPortal, int32_t nObjectId,
                                                  int32_t bPerm, int32_t bSkipTownCheck);
using GetLevelsTxtRecordFn   = const uint8_t*(__fastcall*)(uint8_t nTableSet, int32_t nLevelId);
using GetLevelIdFromRoomFn   = int32_t(__fastcall*)(void* pRoom);
using GetTownLevelIdFn       = int32_t(__fastcall*)(uint8_t nTableSet, uint8_t nAct);
using GetWaypointNoFn        = int32_t(__fastcall*)(uint8_t nTableSet, int32_t nLevelId, uint16_t* pWaypointNo);
using WarpUnitFn             = void(__fastcall*)(void* pGame, void* pPlayer, int32_t nLevelId, int32_t nTileCalc);
using AllocUnitDataFn        = void*(__fastcall*)(int32_t nUnitType, int32_t nClassId, int32_t nX, int32_t nY, void* pGame, void* pRoom, int32_t a7, int32_t a8, int32_t a9,
                                                int32_t a10);
using GetRoomAtFn            = void*(__fastcall*)(void* pRoom, int32_t nX, int32_t nY);
using GetFreeCoordinatesFn   = void*(__fastcall*)(void* pRoom, int32_t* pCoordIn, int32_t* pCoordOut, int32_t nSize, int32_t nMask, int32_t nField, int32_t a7);
using ChangeAnimModeFn       = void(__fastcall*)(void* pUnit, int32_t nMode);
using GetPlayerDataFn        = uint8_t*(__fastcall*)(void* pUnit);
using PortalOperateFn        = int32_t(__fastcall*)(void* pOp, int32_t nOperate);
using TeleportUnitFn         = int32_t(__fastcall*)(void* pGame, void* pUnit, void* pRoom, int32_t nX, int32_t nY, int32_t a6, int32_t a7);
using GetUnitRoomFn          = void*(__fastcall*)(void* pUnit);
using SetInteractTypeFn      = void(__fastcall*)(void* pObject, uint8_t nLevelId);
using SkillCheckFn           = int32_t(__fastcall*)(void* pUnit, int32_t nSkillId);
using GetSkillByIdFn         = void*(__fastcall*)(void* pUnit, int32_t nSkillId, int32_t nOwnerGuid);
using GetSkillLevelFn        = int32_t(__fastcall*)(void* pUnit, void* pSkill, int32_t bBonus, uint8_t a4);
using EvaluateSkillFormulaFn = int32_t(__fastcall*)(uint8_t nTableSet, void* pUnit, uint32_t nCalc, int32_t nSkillId, int32_t nSkillLevel);
using GetUnitBaseStatFn      = int32_t(__fastcall*)(void* pUnit, int32_t nStat, int32_t nLayer);
using AddUnitStatFn          = void(__fastcall*)(void* pUnit, int32_t nStat, int32_t nValue, int32_t nLayer);
using SpendSkillPointFn      = int32_t(__fastcall*)(uint8_t nTableSet, void* pUnit, int32_t nSkillId);
using SpawnBossPackFn        = void*(__fastcall*)(void* pGame, void* pRoom, void* pCoordList, int32_t nMonsterId, int32_t bChampionAllowed, uint16_t nX, uint16_t nY,
                                                int32_t nPlacement, uint16_t nFlags);
using SpawnSuperUniqueFn     = void*(__fastcall*)(void* pGame, void* pRoom, int32_t nX, int32_t nY, int32_t nSuperUnique, uint16_t nFlags);
using GetMonStats2TxtRecordFn = const uint8_t*(__fastcall*)(uint8_t nTableSet, int32_t nMonsterId);
using SendScrollMessageFn    = void(__fastcall*)(void* pGame, void* pPlayer, void* pUnit, uint16_t nString);
struct StringKeyView {
	const char* data;
	size_t      length;
};
using GetNamespacedStringIdFn = uint16_t(__fastcall*)(const StringKeyView* key);

static PrepareHandlerFn       g_originalPrepare      = nullptr;
static DoHandlerFn            g_originalDo           = nullptr;
static GetItemsTxtRecordFn    g_getItemsTxtRecord    = nullptr;
static GetPSpellDescriptorFn  g_getPSpellDescriptor  = nullptr;
static EvaluateItemFormulaFn  g_evaluateItemFormula  = nullptr;
static GetSkillsTxtRecordFn   g_getSkillsTxtRecord   = nullptr;
static CastSkillOnTargetFn    g_castSkillOnTarget    = nullptr;
static CastSkillOnPositionFn  g_castSkillOnPosition  = nullptr;
static GetNearestTestedUnitFn g_getNearestTestedUnit = nullptr;
static GetUnitAlignmentFn     g_getUnitAlignment     = nullptr;
static IsRoomInTownFn         g_isRoomInTown         = nullptr;
static AttachSoundFn          g_attachSound          = nullptr;
static CheckStateFn           g_checkState           = nullptr;
static TestLineOfSightFn      g_testLineOfSight      = nullptr;
static GetPSpellDescriptorFn  g_originalGetDesc      = nullptr;
static IsUsableNoTargetFn     g_originalIsUsable     = nullptr;
static DrawHoverUnitLabelFn   g_originalDrawHover    = nullptr;
static GetServerUnitFn        g_getServerUnit        = nullptr;
static uintptr_t              g_exeBase              = 0;
static CreatePortalObjectFn   g_createPortalObject   = nullptr;
static GetLevelsTxtRecordFn   g_getLevelsTxtRecord   = nullptr;
static GetLevelIdFromRoomFn   g_getLevelIdFromRoom   = nullptr;
static GetTownLevelIdFn       g_getTownLevelId       = nullptr;
static GetWaypointNoFn        g_getWaypointNo        = nullptr;
static WarpUnitFn             g_warpUnit             = nullptr;
static AllocUnitDataFn        g_allocUnitData        = nullptr;
static GetRoomAtFn            g_getRoomAt            = nullptr;
static GetFreeCoordinatesFn   g_getFreeCoordinates   = nullptr;
static ChangeAnimModeFn       g_changeAnimMode       = nullptr;
static GetPlayerDataFn        g_getPlayerData        = nullptr;
static PortalOperateFn        g_originalPortalOperate = nullptr;
static TeleportUnitFn         g_teleportUnit         = nullptr;
static GetUnitRoomFn          g_getUnitRoom          = nullptr;
static SetInteractTypeFn      g_setInteractType      = nullptr;
static SkillCheckFn           g_checkClassSkill      = nullptr;
static SkillCheckFn           g_checkRequiredSkills  = nullptr;
static SkillCheckFn           g_checkRequiredAttributes = nullptr;
static GetSkillByIdFn         g_getSkillById         = nullptr;
static GetSkillLevelFn        g_getSkillLevel        = nullptr;
static EvaluateSkillFormulaFn g_evaluateSkillFormula = nullptr;
static GetUnitBaseStatFn      g_getUnitBaseStat      = nullptr;
static GetUnitBaseStatFn      g_getUnitStat          = nullptr; // same signature, full value
static AddUnitStatFn          g_addUnitStat          = nullptr;
static SpendSkillPointFn      g_spendSkillPoint      = nullptr;
static SpawnBossPackFn        g_spawnBossPack        = nullptr;
static SpawnSuperUniqueFn     g_originalSpawnSuperUnique = nullptr;
static GetMonStats2TxtRecordFn g_getMonStats2TxtRecord = nullptr;
static SendScrollMessageFn    g_sendScrollMessage    = nullptr;
static GetNamespacedStringIdFn g_getNamespacedStringId = nullptr;

// Last monster the client drew a hover label / health bar for (mouse hover or
// controller target), for the local player. Written on the client thread,
// read on the server thread when a target-slot item is used.
static std::atomic<uint32_t> g_hoverMonsterGuid { 0xFFFFFFFFu };
static std::atomic<uint32_t> g_hoverPlayerGuid { 0xFFFFFFFFu };
static std::atomic<int64_t>  g_hoverTimeMs { 0 };
// A label not redrawn within this time no longer counts (the cursor moved away).
inline constexpr int64_t kHoverMaxAgeMs = 250;
// Network reports are refreshed every 100 ms while aiming; allow for latency.
inline constexpr int64_t kRemoteAimMaxAgeMs = 400;
static uintptr_t              g_descTable            = 0;

// Empty descriptor records the descriptor hook hands out for pSpell 20-30
// ({checkIdx 0, doIdx 0, flag 0}, like the game's empty 16-19). Read-only for the game.
alignas(4) static const uint8_t g_extraDesc[Table::LastSlot - Table::FirstExtraSlot + 1][Table::DescStride] {};

static const D2RL::PluginContext* g_ctx         = nullptr;
static bool                       g_slotsActive = false;

// Info and warning lines reach the log only with `diagnostics = true`; errors (the plugin
// can't work) always do.
static void DiagInfo(const D2RL::PluginContext* ctx, const char* msg) {
	if (g_config.diagnostics && ctx != nullptr) {
		ctx->LogInfo(msg);
	}
}

static void DiagWarn(const D2RL::PluginContext* ctx, const char* msg) {
	if (g_config.diagnostics && ctx != nullptr) {
		ctx->LogWarn(msg);
	}
}
// Smallest Skills row count over the loaded banks; 0 until tables load.
static std::atomic<uint32_t> g_skillRowLimit { 0 };
static std::atomic<uint32_t> g_castCount { 0 };
static std::atomic<uint32_t> g_townRefusals { 0 };
static std::atomic<uint32_t> g_failCount { 0 };
static std::atomic<bool>     g_warnedBadSkill { false };
static std::atomic<bool>     g_warnedNoItemEffect { false };
static std::atomic<uint32_t> g_hoverCasts { 0 };
// The game's pSpell 16-19 descriptor records are empty (read after the data tables
// load). Until then, or if they are not, only the plugin's 20-30 records are used.
static std::atomic<bool>     g_gameSlotsEmpty { false };
static std::atomic<bool>     g_warnedCastFailed { false };

static auto ReadU32(const void* base, size_t offset) -> uint32_t {
	uint32_t value = 0;
	std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
	return value;
}

static auto ReadI32(const void* base, size_t offset) -> int32_t {
	int32_t value = 0;
	std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
	return value;
}

static auto ReadU16(const void* base, size_t offset) -> uint16_t {
	uint16_t value = 0;
	std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
	return value;
}

static auto ReadF32(const void* base, size_t offset) -> float {
	float value = 0.0F;
	std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
	return value;
}

static auto ReadPtr(const void* base, size_t offset) -> void* {
	void* value = nullptr;
	std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
	return value;
}

static void WarnOnce(std::atomic<bool>& flag, const char* message) {
	if (!flag.exchange(true) && g_ctx != nullptr) {
		DiagWarn(g_ctx, message);
	}
}

static auto UnitPath(void* pUnit) -> void* {
	return pUnit != nullptr ? ReadPtr(pUnit, Off::UnitPath) : nullptr;
}

static auto UnitPosition(void* pUnit, int32_t& x, int32_t& y) -> bool {
	const void* path = UnitPath(pUnit);
	if (path == nullptr) {
		return false;
	}
	x = ReadU16(path, Off::PathPosX);
	y = ReadU16(path, Off::PathPosY);
	return true;
}

// Same rule as the game's skill start check (SKILLS_SrvStartDispatch, D2MOO
// sub_6FD12950): no room, or a town room, refuses.
static auto IsInTown(void* pUnit) -> bool {
	const void* path = UnitPath(pUnit);
	void* room       = path != nullptr ? ReadPtr(path, Off::PathRoom) : nullptr;
	return room == nullptr || g_isRoomInTown(room) != 0;
}

struct SkillUse {
	int32_t skillId = 0;
	int32_t level   = 1;
};

static auto GameTableSet(void* pGame) -> uint8_t {
	return static_cast<const uint8_t*>(pGame)[Off::GameDataTableSet];
}

// items.txt pSpell 20-30 of a non-book item, else 0. The hooked lookups only
// reach this after the game's own lookup found no record.
static auto ExtraSlotFromItemsTxt(uint8_t nTableSet, void* pItem) -> uint32_t {
	if (pItem == nullptr || g_getItemsTxtRecord == nullptr) {
		return 0;
	}
	const uint8_t* record = g_getItemsTxtRecord(nTableSet, ReadI32(pItem, Off::UnitClassId));
	if (record == nullptr) {
		return 0;
	}
	const int32_t pSpell = ReadI32(record, Off::ItemsTxtPSpell);
	return pSpell >= static_cast<int32_t>(Table::FirstExtraSlot) && pSpell <= static_cast<int32_t>(Table::LastSlot) ? static_cast<uint32_t>(pSpell) : 0;
}

// DATATBLS_GetItemPSpellDescriptor hook: the game's record for 0-19, the plugin's
// empty record for 20-30, NULL otherwise.
static auto __fastcall HookGetDescriptor(uint8_t nTableSet, void* pItem) -> const uint8_t* {
	if (g_originalGetDesc == nullptr) {
		return nullptr;
	}
	if (const uint8_t* desc = g_originalGetDesc(nTableSet, pItem); desc != nullptr) {
		return desc;
	}
	const uint32_t slot = ExtraSlotFromItemsTxt(nTableSet, pItem);
	return slot != 0 ? g_extraDesc[slot - Table::FirstExtraSlot] : nullptr;
}

// ITEMS_IsPSpellUsableWithoutTarget hook (an inlined copy of the lookup with the
// same 0x13 bound): pSpell 20-30 items are usable without a target item.
static auto __fastcall HookIsUsableNoTarget(void* pItem) -> bool {
	if (g_originalIsUsable == nullptr) {
		return false;
	}
	if (g_originalIsUsable(pItem)) {
		return true;
	}
	return pItem != nullptr && ExtraSlotFromItemsTxt(static_cast<const uint8_t*>(pItem)[Off::UnitDataTableSet], pItem) != 0;
}

// The item's pSpell when it is one of the free 16-30 values, else 0. Uses the
// game's own (hooked) lookup, so books.txt pSpells work the same way. All free
// values are guarded (not only the configured two): the game has no code behind them.
static auto FreeSlotOf(void* pGame, void* pItem) -> uint32_t {
	if (!g_slotsActive || pGame == nullptr || pItem == nullptr) {
		return 0;
	}
	const auto desc = reinterpret_cast<uintptr_t>(g_getPSpellDescriptor(GameTableSet(pGame), pItem));
	const auto extra = reinterpret_cast<uintptr_t>(&g_extraDesc[0][0]);
	if (desc >= extra && desc < extra + sizeof(g_extraDesc)) {
		return Table::FirstExtraSlot + static_cast<uint32_t>((desc - extra) / Table::DescStride);
	}
	if (desc < g_descTable || desc >= g_descTable + Table::DescCount * Table::DescStride) {
		return 0;
	}
	const auto pSpell = static_cast<uint32_t>((desc - g_descTable) / Table::DescStride);
	return pSpell >= Table::FirstFreeSlot && g_gameSlotsEmpty.load(std::memory_order_acquire) ? pSpell : 0;
}

static auto IsConfiguredSlot(uint32_t slot) -> bool {
	return g_config.enabled
	    && (slot == g_config.targetSlot || slot == g_config.positionSlot || slot == g_config.portalSlot || slot == g_config.tomeSlot || slot == g_config.summonSlot
	        || slot == g_config.ambushSlot || slot == g_config.messageSlot);
}

// calc1 = skill id, calc2 = skill level, both evaluated the way the game
// evaluates every other misc.txt calc column. The skill needs an ItemEffect,
// otherwise SKILLITEM_HandleItemEffectSkill would refuse the cast after the item
// is already gone.
static auto ReadItemSkill(void* pGame, void* pUnit, void* pItem, SkillUse& use) -> bool {
	const uint8_t bank    = GameTableSet(pGame);
	const uint8_t* record = g_getItemsTxtRecord(bank, ReadI32(pItem, Off::UnitClassId));
	if (record == nullptr) {
		return false;
	}
	// A blank calc column is 0xFFFFFFFF and evaluates to 0 (handle past the
	// formula count), so a blank calc2 gives level 1 below.
	const int32_t skillId = g_evaluateItemFormula(bank, pUnit, pItem, ReadU32(record, Off::ItemsTxtCalc1));
	const int32_t level   = g_evaluateItemFormula(bank, pUnit, pItem, ReadU32(record, Off::ItemsTxtCalc2));
	// GetSkillsTxtRecord asserts on an id past the table, so check the row count first.
	const uint32_t limit = g_skillRowLimit.load(std::memory_order_acquire);
	if (skillId < 1 || skillId > kMaxSkillId || limit == 0 || static_cast<uint32_t>(skillId) >= limit) {
		WarnOnce(g_warnedBadSkill, "pspell-ext: an item's calc1 is not a valid skill id; the item was not used.");
		return false;
	}
	const uint8_t* skill = g_getSkillsTxtRecord(bank, skillId);
	if (skill == nullptr || ReadU16(skill, Off::SkillsTxtItemEffect) == 0) {
		WarnOnce(g_warnedNoItemEffect, "pspell-ext: an item's calc1 skill has no ItemEffect in skills.txt; the item was not used.");
		return false;
	}
	use.skillId = skillId;
	use.level   = level < 1 ? 1 : (level > kMaxSkillLevel ? kMaxSkillLevel : level);
	return true;
}

// Prepare stage for both pSpells. false = the item stays (the game's own refusal path).
static auto CanUse(void* pGame, void* pUnit, void* pItem) -> bool {
	if (pUnit == nullptr || ReadU32(pUnit, Off::UnitType) != kUnitTypePlayer) {
		return false;
	}
	if (IsInTown(pUnit)) {
		g_attachSound(pUnit, kSoundTownRefused, nullptr);
		g_townRefusals.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	SkillUse use;
	// The cast would be refused after the item is gone, so refuse the use instead.
	if (g_checkState(pUnit, kStateUninterruptable) != 0 || !ReadItemSkill(pGame, pUnit, pItem, use)) {
		g_attachSound(pUnit, kSoundCannotUse, nullptr);
		g_failCount.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	return true;
}

// Hostile, attackable, living monster; with checkLineOfSight also a clear missile path.
static auto IsHostileMonster(void* pCandidate, void* pOrigin, bool checkLineOfSight) -> bool {
	if (pCandidate == nullptr || pCandidate == pOrigin || ReadU32(pCandidate, Off::UnitType) != kUnitTypeMonster) {
		return false;
	}
	const uint32_t flags = ReadU32(pCandidate, Off::UnitFlags);
	constexpr uint32_t kNeeded = kUnitFlagTargetable | kUnitFlagCanBeAttacked | kUnitFlagIsValidTarget;
	if ((flags & kNeeded) != kNeeded || (flags & kUnitFlagIsDead) != 0) {
		return false;
	}
	const uint32_t mode = ReadU32(pCandidate, Off::UnitAnimMode);
	if (mode == kMonModeDeath || mode == kMonModeDead) {
		return false;
	}
	if (g_getUnitAlignment(pCandidate) != kAlignmentEvil) {
		return false;
	}
	// Clear missile path from the candidate to the player (D2MOO D2Common_11026 order:
	// target coords first, the unit whose room and position start the ray second).
	int32_t x = 0;
	int32_t y = 0;
	return !checkLineOfSight || (UnitPosition(pCandidate, x, y) && g_testLineOfSight(x, y, pOrigin, kLineOfSightMask) != 0);
}

// UNITFINDS_GetNearestTestedUnit callback.
static auto __fastcall IsHostileTarget(uint8_t nTableSet, void* pCandidate, void* pOrigin) -> int32_t {
	(void)nTableSet;
	return IsHostileMonster(pCandidate, pOrigin, g_config.lineOfSight) ? 1 : 0;
}

static auto NowMs() -> int64_t {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// GUID of the client's local player, or 0xFFFFFFFF (client data, same GUIDs as the server).
static auto ClientLocalPlayerGuid() -> uint32_t {
	const uint32_t slot = ReadU32(reinterpret_cast<const void*>(g_exeBase), Rva::ClientLocalPlayerSlot);
	return slot < 8 ? ReadU32(reinterpret_cast<const void*>(g_exeBase), Rva::ClientPlayerGuids + slot * 4ULL) : 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Aim target sharing (TCP/IP). Every client sends the monster it is aiming at to
// the host over a D2RLoader plugin channel; the host keeps the latest one per
// player. The player whose client runs in the host process also uses the
// in-process copy above (single player has no network peer).
// ---------------------------------------------------------------------------
namespace Net {
inline constexpr uint16_t kChannelId = 1;
inline constexpr uint64_t kCompatibilityToken = 0x50535053'4B000001ULL; // "PSPSK" v1
inline constexpr uint16_t kMsgAimTarget = 1;
inline constexpr uint32_t kMsgVersion = 1;
inline constexpr int64_t  kResendMs = 100;  // refresh while the same monster stays aimed at
inline constexpr int64_t  kConnectRetryMs = 1000;
inline constexpr int64_t  kBindingKeepMs = 10000; // a fresh entry stays bound to its peer
struct AimTargetMsg {
	uint32_t version;
	uint32_t playerGuid;
	uint32_t monsterGuid;
};
static_assert(sizeof(AimTargetMsg) == 12);
} // namespace Net

static const D2RL::NetworkService* g_net = nullptr;
static D2RL::Network::ChannelHandle g_channel = D2RL::Network::InvalidChannelHandle;
static std::atomic<uint32_t> g_netState { static_cast<uint32_t>(D2RL::Network::ConnectionState::Disconnected) };
static std::atomic<int64_t>  g_lastConnectTryMs { 0 };
static std::atomic<int64_t>  g_lastSendMs { 0 };
static std::atomic<uint32_t> g_lastSentMonster { 0xFFFFFFFFu };
static std::atomic<uint32_t> g_aimMsgsReceived { 0 };

struct RemoteAim {
	uint32_t                     monsterGuid;
	int64_t                      timeMs;
	D2RL::Network::PeerHandle    peer;
};
static std::mutex                             g_remoteAimMutex;
static std::unordered_map<uint32_t, RemoteAim> g_remoteAim; // key: player GUID

// Client side, from the hover hook (UI thread): connect when needed, then send.
static void SendAimTarget(uint32_t playerGuid, uint32_t monsterGuid) {
	if (g_net == nullptr || g_channel == D2RL::Network::InvalidChannelHandle || playerGuid == 0xFFFFFFFFu) {
		return;
	}
	const int64_t now = NowMs();
	if (monsterGuid == g_lastSentMonster.load(std::memory_order_relaxed) && now - g_lastSendMs.load(std::memory_order_relaxed) < Net::kResendMs) {
		return;
	}
	const Net::AimTargetMsg msg { Net::kMsgVersion, playerGuid, monsterGuid };
	const D2RL::Network::Result sent = g_net->sendToHost(g_ctx, g_channel, Net::kMsgAimTarget, &msg, sizeof(msg));
	if (sent == D2RL::Network::Result::Success) {
		g_lastSentMonster.store(monsterGuid, std::memory_order_relaxed);
		g_lastSendMs.store(now, std::memory_order_relaxed);
		g_netState.store(static_cast<uint32_t>(D2RL::Network::ConnectionState::Connected), std::memory_order_relaxed);
		return;
	}
	// Not connected yet (or the host dropped us): connect, at most once a second.
	// Busy means the host's transport packet hasn't been processed yet; retry later.
	if (sent != D2RL::Network::Result::RateLimited && now - g_lastConnectTryMs.load(std::memory_order_relaxed) >= Net::kConnectRetryMs) {
		g_lastConnectTryMs.store(now, std::memory_order_relaxed);
		(void)g_net->connectToHost(g_ctx, g_channel);
	}
}

// Host side: a client reported its aim target.
static void __cdecl OnHostMessage(const D2RL::PluginContext*, D2RL::Network::ChannelHandle, D2RL::Network::PeerHandle peer, uint16_t messageId, const void* data,
                                  uint32_t size, void*) noexcept {
	if (messageId != Net::kMsgAimTarget || data == nullptr || size < sizeof(Net::AimTargetMsg)) {
		return;
	}
	Net::AimTargetMsg msg {};
	std::memcpy(&msg, data, sizeof(msg));
	if (msg.version != Net::kMsgVersion || msg.playerGuid == 0xFFFFFFFFu) {
		return;
	}
	const int64_t now = NowMs();
	const std::lock_guard<std::mutex> lock(g_remoteAimMutex);
	auto it = g_remoteAim.find(msg.playerGuid);
	// A player GUID stays bound to the peer that reported it while its entry is fresh,
	// so one client cannot overwrite another player's target.
	if (it != g_remoteAim.end() && it->second.peer != peer && now - it->second.timeMs < Net::kBindingKeepMs) {
		return;
	}
	g_remoteAim[msg.playerGuid] = RemoteAim { msg.monsterGuid, now, peer };
	g_aimMsgsReceived.fetch_add(1, std::memory_order_relaxed);
}

static void __cdecl OnConnectionState(const D2RL::PluginContext*, const D2RL::Network::ConnectionEvent* event, void*) noexcept {
	if (!D2RL::Network::HasConnectionEventField(event, D2RL::Network::ConnectionEventRequiredSize)) {
		return;
	}
	if (event->peer == D2RL::Network::InvalidPeerHandle) {
		// Client side: our own connection to the host (status display only).
		g_netState.store(static_cast<uint32_t>(event->state), std::memory_order_relaxed);
		return;
	}
	// Host side: forget a peer's aim targets when it leaves.
	if (event->state == D2RL::Network::ConnectionState::Disconnected || event->state == D2RL::Network::ConnectionState::Rejected) {
		const std::lock_guard<std::mutex> lock(g_remoteAimMutex);
		for (auto it = g_remoteAim.begin(); it != g_remoteAim.end();) {
			it = it->second.peer == event->peer ? g_remoteAim.erase(it) : std::next(it);
		}
	}
}

static void RegisterAimChannel(const D2RL::PluginContext* ctx) {
	if (ctx->QueryService(&g_net) != D2RL::ServiceQueryResult::Success || !D2RL::HasNetworkServiceField(g_net, D2RL::NetworkServiceRequiredSize)) {
		g_net = nullptr;
		DiagWarn(ctx, "pspell-ext: NetworkService missing; aim targets only work for the player on the host.");
		return;
	}
	const D2RL::Network::ChannelRegistration reg {
		.structSize         = D2RL::Network::ChannelRegistrationSize,
		.localChannelId     = Net::kChannelId,
		.compatibilityToken = Net::kCompatibilityToken,
		.hostMessage        = OnHostMessage,
		.clientMessage      = nullptr,
		.connectionState    = OnConnectionState,
	};
	if (g_net->registerChannel(ctx, &reg, &g_channel) != D2RL::Network::Result::Success) {
		g_channel = D2RL::Network::InvalidChannelHandle;
		DiagWarn(ctx, "pspell-ext: could not register the aim-target channel; aim targets only work for the player on the host.");
	}
}

// maybe_D2CLIENT_UI_DrawHoverUnitLabel hook (client thread): remember the monster
// and tell the host.
static void __fastcall HookDrawHoverUnitLabel(void* pUnit, void* a2, void* a3, void* a4) {
	if (pUnit != nullptr && ReadU32(pUnit, Off::UnitType) == kUnitTypeMonster) {
		const uint32_t monsterGuid = ReadU32(pUnit, Off::UnitGuid);
		const uint32_t playerGuid  = ClientLocalPlayerGuid();
		g_hoverMonsterGuid.store(monsterGuid, std::memory_order_relaxed);
		g_hoverPlayerGuid.store(playerGuid, std::memory_order_relaxed);
		g_hoverTimeMs.store(NowMs(), std::memory_order_release);
		if (g_config.useHoverTarget) {
			SendAimTarget(playerGuid, monsterGuid);
		}
	}
	if (g_originalDrawHover != nullptr) {
		g_originalDrawHover(pUnit, a2, a3, a4);
	}
}

// GUID of the monster this player is aiming at, if reported recently: the
// in-process copy for the host's own player, else the network report.
static auto AimTargetGuid(uint32_t playerGuid) -> uint32_t {
	const int64_t now = NowMs();
	if (g_hoverPlayerGuid.load(std::memory_order_relaxed) == playerGuid && now - g_hoverTimeMs.load(std::memory_order_acquire) <= kHoverMaxAgeMs) {
		return g_hoverMonsterGuid.load(std::memory_order_relaxed);
	}
	const std::lock_guard<std::mutex> lock(g_remoteAimMutex);
	const auto it = g_remoteAim.find(playerGuid);
	return it != g_remoteAim.end() && now - it->second.timeMs <= kRemoteAimMaxAgeMs ? it->second.monsterGuid : 0xFFFFFFFFu;
}

// The monster this player is aiming at (mouse hover or controller target) if it is
// a living hostile monster.
static auto FindHoverTarget(void* pGame, void* pUnit) -> void* {
	if (!g_config.useHoverTarget || g_getServerUnit == nullptr) {
		return nullptr;
	}
	const uint32_t monsterGuid = AimTargetGuid(ReadU32(pUnit, Off::UnitGuid));
	if (monsterGuid == 0xFFFFFFFFu) {
		return nullptr;
	}
	void* target = g_getServerUnit(pGame, kUnitTypeMonster, monsterGuid);
	// The player picked it: hostility and life are checked, range and line of sight are not.
	return target != nullptr && IsHostileMonster(target, pUnit, false) ? target : nullptr;
}

static auto FindNearestEnemy(void* pGame, void* pUnit) -> void* {
	int32_t x = 0;
	int32_t y = 0;
	if (!UnitPosition(pUnit, x, y)) {
		return nullptr;
	}
	return g_getNearestTestedUnit(GameTableSet(pGame), pUnit, x, y, static_cast<int32_t>(g_config.searchRadius), &IsHostileTarget);
}

static auto PointInFront(void* pUnit, uint32_t distance, int32_t& x, int32_t& y) -> bool {
	const void* path = UnitPath(pUnit);
	if (path == nullptr) {
		return false;
	}
	int32_t dx = 0;
	int32_t dy = 0;
	DirectionOffset(ReadF32(path, Off::PathDirection), static_cast<int32_t>(distance), dx, dy);
	x = ReadU16(path, Off::PathPosX) + dx;
	y = ReadU16(path, Off::PathPosY) + dy;
	return x >= 0 && y >= 0;
}

static auto CastResult(int32_t result) -> int32_t {
	if (result != 0) {
		g_castCount.fetch_add(1, std::memory_order_relaxed);
		return result;
	}
	g_failCount.fetch_add(1, std::memory_order_relaxed);
	WarnOnce(g_warnedCastFailed, "pspell-ext: a skill item cast failed after the item was used (the player could not act).");
	return 0;
}

// Sets the caster's item-event-cast flagEx bit for the duration of one cast,
// like the game's chance-to-cast event functions.
class ItemEventCastScope {
public:
	explicit ItemEventCastScope(void* pUnit) : m_flags(static_cast<uint8_t*>(pUnit) + Off::UnitFlagsEx) {
		Write(ReadU32(m_flags, 0) | kUnitFlagExItemEventCast);
	}
	~ItemEventCastScope() { Write(ReadU32(m_flags, 0) & ~kUnitFlagExItemEventCast); }
	ItemEventCastScope(const ItemEventCastScope&)            = delete;
	ItemEventCastScope& operator=(const ItemEventCastScope&) = delete;

private:
	void Write(uint32_t value) { std::memcpy(m_flags, &value, sizeof(value)); }
	uint8_t* m_flags;
};

// Target slot (D2MOO pSpell 10 with calc1 skill): nearest enemy, else a point in front.
static void CastOnNearest(void* pGame, void* pUnit, void* pItem) {
	SkillUse use;
	if (!ReadItemSkill(pGame, pUnit, pItem, use)) {
		return;
	}
	void* enemy = FindHoverTarget(pGame, pUnit);
	if (enemy != nullptr) {
		g_hoverCasts.fetch_add(1, std::memory_order_relaxed);
	} else {
		enemy = FindNearestEnemy(pGame, pUnit);
	}
	if (enemy != nullptr) {
		const ItemEventCastScope scope(pUnit);
		CastResult(g_castSkillOnTarget(pUnit, use.skillId, use.level, enemy, 0));
		return;
	}
	int32_t x = 0;
	int32_t y = 0;
	if (PointInFront(pUnit, g_config.frontDistance, x, y)) {
		const ItemEventCastScope scope(pUnit);
		CastResult(g_castSkillOnPosition(pUnit, use.skillId, use.level, x, y, 0));
	}
}

// Position slot (D2MOO pSpell 11 with calc1 skill): the player's position.
static void CastAtPlayer(void* pGame, void* pUnit, void* pItem) {
	SkillUse use;
	int32_t x = 0;
	int32_t y = 0;
	if (ReadItemSkill(pGame, pUnit, pItem, use) && UnitPosition(pUnit, x, y)) {
		const ItemEventCastScope scope(pUnit);
		CastResult(g_castSkillOnPosition(pUnit, use.skillId, use.level, x, y, 0));
	}
}

// ---------------------------------------------------------------------------
// Red portal pSpell: calc1 = level it must be used in (0 / blank = anywhere),
// calc2 = destination level.
// Same act: the game's own CreatePortalObject (two linked red portals, like the
// Cow Level cube recipe; the town restriction is skipped).
// Another act: allowed when the player could waypoint there (that act's town
// waypoint is active on this difficulty; no act 5 in a classic game). The game
// can't link portals across acts, so the plugin places a red portal here and,
// the first time someone goes through, a return portal where they arrive; the
// portal operate hook moves players between the two with LEVEL_WarpUnit.
// ---------------------------------------------------------------------------
inline constexpr int32_t kObjectRedPortal   = 0x3C; // objects.txt PortalPermanent (60)
inline constexpr int32_t kObjModeOpened     = 2;    // the portal operate function requires it
inline constexpr int32_t kPortalArrivalTile = 11;   // LEVEL_WarpUnit tile used by the Baal portal / link portals
inline constexpr uint32_t kNoUnit           = 0xFFFFFFFFu;

struct PortalPair {
	void*    game;
	uint32_t nearGuid; // in the level it was opened from
	uint32_t farGuid;  // return portal in the destination, kNoUnit until first use
	int32_t  srcLevel;
	int32_t  destLevel;
	uint8_t  destAct;
};
static std::mutex              g_portalMutex;
static std::vector<PortalPair> g_portalPairs;
static std::atomic<uint32_t>   g_portalsOpened { 0 };
static std::atomic<uint32_t>   g_portalRefusals { 0 };
static std::atomic<uint32_t>   g_levelRowLimit { 0 };
static std::atomic<bool>       g_warnedBadPortalLevel { false };

static auto LevelAct(uint8_t bank, int32_t level, uint8_t& act) -> bool {
	const uint32_t limit = g_levelRowLimit.load(std::memory_order_acquire);
	if (level < 1 || limit == 0 || static_cast<uint32_t>(level) >= limit) {
		return false;
	}
	const uint8_t* record = g_getLevelsTxtRecord(bank, level);
	if (record == nullptr) {
		return false;
	}
	act = record[Off::LevelsTxtAct];
	return true;
}

// Waypoint rule (NPC travel / waypoint code): the act is open when its town
// waypoint is active for this player on this difficulty; a classic game has no act 5.
static auto IsActOpenForPlayer(void* pGame, void* pPlayer, uint8_t act) -> bool {
	const uint8_t bank = GameTableSet(pGame);
	if (bank == 1 && act == 4) {
		return false;
	}
	const uint8_t* playerData = g_getPlayerData(pPlayer);
	if (playerData == nullptr) {
		return false;
	}
	const uint8_t difficulty = static_cast<const uint8_t*>(pGame)[Off::GameDifficulty];
	const void* waypoints    = ReadPtr(playerData, Off::PlayerDataWaypoints + difficulty * 8ULL);
	uint16_t waypointNo      = 0xFF;
	if (waypoints == nullptr || g_getWaypointNo(bank, g_getTownLevelId(bank, act), &waypointNo) == 0 || waypointNo >= 0x70) {
		return false;
	}
	const void* bits    = reinterpret_cast<const void*>(g_exeBase + Rva::WaypointBitTable);
	const uint16_t word = ReadU16(bits, waypointNo * 4ULL);
	const uint16_t mask = ReadU16(bits, waypointNo * 4ULL + 2);
	return (ReadU16(waypoints, word * 2ULL) & mask) != 0;
}

static auto ObjectPosition(void* pObject, int32_t& x, int32_t& y) -> bool {
	const void* path = pObject != nullptr ? ReadPtr(pObject, Off::UnitPath) : nullptr;
	if (path == nullptr) {
		return false;
	}
	x = ReadI32(path, Off::ObjectPathX);
	y = ReadI32(path, Off::ObjectPathY);
	return true;
}

// A red portal object near x/y in pRoom's act, already open (mode 2), showing destLevel.
static auto SpawnRedPortal(void* pGame, void* pRoom, int32_t x, int32_t y, int32_t destLevel) -> void* {
	int32_t coord[2] = { x, y };
	if (pRoom == nullptr || g_getFreeCoordinates(pRoom, coord, coord, 3, 0x3E01, 0xC01, 0) == nullptr) {
		return nullptr;
	}
	void* room = g_getRoomAt(pRoom, coord[0], coord[1]);
	if (room == nullptr) {
		return nullptr;
	}
	void* portal = g_allocUnitData(2, kObjectRedPortal, coord[0], coord[1], pGame, room, 1, 1, 0, 0);
	if (portal != nullptr) {
		g_changeAnimMode(portal, kObjModeOpened);
		g_setInteractType(portal, static_cast<uint8_t>(destLevel));
	}
	return portal;
}

static auto RefusePortal(void* pUnit) -> bool {
	g_attachSound(pUnit, kSoundCannotUse, nullptr);
	g_portalRefusals.fetch_add(1, std::memory_order_relaxed);
	return false;
}

// Prepare stage of the portal pSpell (the game's own town portal also opens its
// portal there). false = the item stays.
static auto OpenPortal(void* pGame, void* pUnit, void* pItem) -> bool {
	if (pUnit == nullptr || ReadU32(pUnit, Off::UnitType) != kUnitTypePlayer) {
		return false;
	}
	const uint8_t bank    = GameTableSet(pGame);
	const uint8_t* record = g_getItemsTxtRecord(bank, ReadI32(pItem, Off::UnitClassId));
	const void* path      = UnitPath(pUnit);
	void* room            = path != nullptr ? ReadPtr(path, Off::PathRoom) : nullptr;
	int32_t x = 0;
	int32_t y = 0;
	if (record == nullptr || room == nullptr || !UnitPosition(pUnit, x, y)) {
		return RefusePortal(pUnit);
	}
	const int32_t requiredLevel = g_evaluateItemFormula(bank, pUnit, pItem, ReadU32(record, Off::ItemsTxtCalc1));
	const int32_t destLevel     = g_evaluateItemFormula(bank, pUnit, pItem, ReadU32(record, Off::ItemsTxtCalc2));
	const int32_t hereLevel     = g_getLevelIdFromRoom(room);
	uint8_t destAct = 0;
	uint8_t hereAct = 0;
	if (!LevelAct(bank, destLevel, destAct) || !LevelAct(bank, hereLevel, hereAct)) {
		WarnOnce(g_warnedBadPortalLevel, "pspell-ext: a portal item's calc2 is not a level id in levels.txt; the item was not used.");
		return RefusePortal(pUnit);
	}
	// The portal's destination is a byte (SetInteractType).
	if ((requiredLevel > 0 && hereLevel != requiredLevel) || destLevel == hereLevel || destLevel > 0xFF || hereLevel > 0xFF) {
		return RefusePortal(pUnit);
	}
	if (destAct == hereAct) {
		void* portal = nullptr;
		if (g_createPortalObject(pGame, pUnit, room, x, y, destLevel, &portal, kObjectRedPortal, 0, 1) == 0) {
			return RefusePortal(pUnit);
		}
		// Like the Uber Ancients cube portal: the init only knows hardcoded pairs, so set the
		// real destination (the far portal already points back here, CreateLinkPortal).
		if (portal != nullptr) {
			g_setInteractType(portal, static_cast<uint8_t>(destLevel));
		}
		g_portalsOpened.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	if (!IsActOpenForPlayer(pGame, pUnit, destAct)) {
		return RefusePortal(pUnit);
	}
	void* nearPortal = SpawnRedPortal(pGame, room, x, y, destLevel);
	if (nearPortal == nullptr) {
		return RefusePortal(pUnit);
	}
	{
		const std::lock_guard<std::mutex> lock(g_portalMutex);
		std::erase_if(g_portalPairs, [pGame](const PortalPair& p) { return p.game != pGame; }); // pairs of an older game
		g_portalPairs.push_back(PortalPair { pGame, ReadU32(nearPortal, Off::UnitGuid), kNoUnit, hereLevel, destLevel, destAct });
	}
	g_portalsOpened.fetch_add(1, std::memory_order_relaxed);
	return true;
}

// Object operate function 15 hook (town portal / red portal): the plugin's
// cross-act portals; every other portal goes to the game.
static auto __fastcall HookPortalOperate(void* pOp, int32_t nOperate) -> int32_t {
	void* game   = pOp != nullptr ? ReadPtr(pOp, 0x00) : nullptr; // D2ObjOperateFnStrc: pGame, pObject, pPlayer
	void* object = pOp != nullptr ? ReadPtr(pOp, 0x08) : nullptr;
	void* player = pOp != nullptr ? ReadPtr(pOp, 0x10) : nullptr;
	if (game != nullptr && object != nullptr && player != nullptr && ReadU32(player, Off::UnitType) == kUnitTypePlayer && g_slotsActive) {
		const uint32_t guid = ReadU32(object, Off::UnitGuid);
		PortalPair pair {};
		bool found  = false;
		bool isNear = false;
		{
			const std::lock_guard<std::mutex> lock(g_portalMutex);
			for (const PortalPair& p : g_portalPairs) {
				if (p.game == game && (p.nearGuid == guid || p.farGuid == guid)) {
					pair   = p;
					isNear = p.nearGuid == guid;
					found  = true;
					break;
				}
			}
		}
		if (found) {
			if (isNear) {
				// Same rule for every player who walks in, not only the one who opened it.
				if (!IsActOpenForPlayer(game, player, pair.destAct)) {
					g_attachSound(player, kSoundCannotUse, nullptr);
					return 0;
				}
				g_attachSound(player, 8, nullptr); // the game's portal-enter sound
				g_warpUnit(game, player, pair.destLevel, kPortalArrivalTile);
				// Return portal where the first traveller arrives (the act exists now);
				// made again if the old one is gone.
				if (pair.farGuid == kNoUnit || g_getServerUnit(game, 2, pair.farGuid) == nullptr) {
					int32_t x = 0;
					int32_t y = 0;
					void* farPortal = UnitPosition(player, x, y) ? SpawnRedPortal(game, g_getUnitRoom(player), x + 3, y, pair.srcLevel) : nullptr;
					if (farPortal != nullptr) {
						const std::lock_guard<std::mutex> lock(g_portalMutex);
						for (PortalPair& p : g_portalPairs) {
							if (p.game == game && p.nearGuid == pair.nearGuid) {
								p.farGuid = ReadU32(farPortal, Off::UnitGuid);
							}
						}
					}
				}
			} else {
				g_attachSound(player, 8, nullptr);
				g_warpUnit(game, player, pair.srcLevel, kPortalArrivalTile);
				// Then next to the portal it came from, if that is still loaded.
				int32_t x = 0;
				int32_t y = 0;
				void* nearPortal = g_getServerUnit(game, 2, pair.nearGuid);
				void* nearRoom   = nearPortal != nullptr ? g_getUnitRoom(nearPortal) : nullptr;
				int32_t coord[2] {};
				if (nearRoom != nullptr && ObjectPosition(nearPortal, x, y)) {
					coord[0] = x + 2;
					coord[1] = y + 2;
					if (g_getFreeCoordinates(nearRoom, coord, coord, 3, 0x3E01, 0xC01, 0) != nullptr) {
						if (void* room = g_getRoomAt(nearRoom, coord[0], coord[1]); room != nullptr) {
							(void)g_teleportUnit(game, player, room, coord[0], coord[1], 0, 0);
						}
					}
				}
			}
			return 1;
		}
	}
	return g_originalPortalOperate != nullptr ? g_originalPortalOperate(pOp, nOperate) : 0;
}

// ---------------------------------------------------------------------------
// Skill tome pSpell: calc1 = skill id, +1 hard point, like spending a skill point.
// ---------------------------------------------------------------------------
inline constexpr int32_t kStatSkillPoints = 5;  // STAT_SKILLPTS
inline constexpr int32_t kDefaultMaxLevel = 20; // Rcv0x3B: maxlvl <= 0
static std::atomic<uint32_t> g_tomesRead { 0 };
static std::atomic<uint32_t> g_tomeRefusals { 0 };
static std::atomic<bool>     g_warnedBadTomeSkill { false };

static auto RefuseTome(void* pUnit) -> bool {
	g_attachSound(pUnit, kSoundCannotUse, nullptr);
	g_tomeRefusals.fetch_add(1, std::memory_order_relaxed);
	return false;
}

// Prepare stage of the tome pSpell. The checks are the ones D2GAME_PACKETCALLBACK_Rcv0x3B
// makes before it spends a point (own class skill, required level + hard level,
// required skills and attributes, maxlvl). The game's own spend routine then adds
// the level; the plugin pays its skpoints cost in STAT_SKILLPTS first, so the player's
// unspent points end where they started. false = the item stays.
static auto ReadTome(void* pGame, void* pUnit, void* pItem) -> bool {
	if (pUnit == nullptr || ReadU32(pUnit, Off::UnitType) != kUnitTypePlayer) {
		return false;
	}
	const uint8_t bank    = GameTableSet(pGame);
	const uint8_t* record = g_getItemsTxtRecord(bank, ReadI32(pItem, Off::UnitClassId));
	if (record == nullptr) {
		return RefuseTome(pUnit);
	}
	const int32_t skillId = g_evaluateItemFormula(bank, pUnit, pItem, ReadU32(record, Off::ItemsTxtCalc1));
	const uint32_t limit  = g_skillRowLimit.load(std::memory_order_acquire);
	if (skillId < 1 || skillId > kMaxSkillId || limit == 0 || static_cast<uint32_t>(skillId) >= limit) {
		WarnOnce(g_warnedBadTomeSkill, "pspell-ext: a tome item's calc1 is not a valid skill id; the item was not used.");
		return RefuseTome(pUnit);
	}
	const uint8_t* skill = g_getSkillsTxtRecord(bank, skillId);
	if (skill == nullptr) {
		return RefuseTome(pUnit);
	}
	// Own class only: the save keeps hard points of the class's skills alone (the "if"
	// section loops SKILLS_GetClassSkillId), so a classless point would vanish on reload.
	if (g_checkClassSkill(pUnit, skillId) == 0) {
		return RefuseTome(pUnit);
	}
	if (g_checkRequiredSkills(pUnit, skillId) == 0 || g_checkRequiredAttributes(pUnit, skillId) == 0) {
		return RefuseTome(pUnit);
	}
	void* owned = g_getSkillById(pUnit, skillId, -1);
	if (owned != nullptr) {
		int32_t maxLevel = static_cast<int16_t>(ReadU16(skill, Off::SkillsTxtMaxLevel));
		maxLevel         = maxLevel <= 0 ? kDefaultMaxLevel : maxLevel;
		if (g_getSkillLevel(pUnit, owned, 0, 0) >= maxLevel) {
			return RefuseTome(pUnit);
		}
	}
	// The spend routine's own cost: 1, or the skpoints formula at the current level (with bonuses).
	int32_t cost = 1;
	if (const uint32_t calc = ReadU32(skill, Off::SkillsTxtSkPoints); calc != 0xFFFFFFFFu) {
		const int32_t level = owned != nullptr ? g_getSkillLevel(pUnit, owned, 1, 0) : 0;
		cost                = g_evaluateSkillFormula(bank, pUnit, calc, skillId, level);
		cost                = cost < 0 ? 0 : cost;
	}
	const int32_t before = g_getUnitBaseStat(pUnit, kStatSkillPoints, 0);
	if (cost > 0) {
		g_addUnitStat(pUnit, kStatSkillPoints, cost, 0);
	}
	const bool learned = g_spendSkillPoint(bank, pUnit, skillId) != 0;
	// Paid on success; on failure the routine refunded (or never took) it.
	if (const int32_t now = g_getUnitBaseStat(pUnit, kStatSkillPoints, 0); now != before) {
		g_addUnitStat(pUnit, kStatSkillPoints, before - now, 0);
	}
	if (!learned) {
		return RefuseTome(pUnit);
	}
	g_tomesRead.fetch_add(1, std::memory_order_relaxed);
	return true;
}

// ---------------------------------------------------------------------------
// Summon and ambush pSpells: Evil Urn style packs (one champion or unique monster
// and its minions, hostile). calc1 = monstats.txt row (0 / blank = the urn's pick for
// the level), calc2 = 1 for no champion roll (always unique), calc3 = number of packs.
// Summon: in front of the player, not in town. Ambush: queued for this game; the next
// superunique that spawns brings the packs along (like the Diablo Clone event, but
// the superunique stays).
// ---------------------------------------------------------------------------
inline constexpr uint32_t kMaxPacks       = 10;
inline constexpr size_t   kMaxAmbushQueue = 8; // per game

struct PackSpec {
	int32_t  monsterId = 0; // 0 = urn pick for the level the packs spawn in
	bool     unique    = false;
	uint32_t count     = 1;
};

struct PendingAmbush {
	void*    game;
	PackSpec spec;
};

static std::mutex                 g_ambushMutex;
static std::vector<PendingAmbush> g_pendingAmbush;
static std::atomic<uint32_t>      g_monsterRowLimit { 0 };
static std::atomic<uint32_t>      g_packsSpawned { 0 };
static std::atomic<uint32_t>      g_summonRefusals { 0 };
static std::atomic<uint32_t>      g_ambushesQueued { 0 };
static std::atomic<uint32_t>      g_ambushesSprung { 0 };
static std::atomic<uint32_t>      g_ambushRefusals { 0 };
static std::atomic<bool>          g_warnedBadMonster { false };

// calc1-3 of the item. false = calc1 is not a monstats row.
static auto ReadPackSpec(void* pGame, void* pUnit, void* pItem, PackSpec& spec) -> bool {
	const uint8_t bank    = GameTableSet(pGame);
	const uint8_t* record = g_getItemsTxtRecord(bank, ReadI32(pItem, Off::UnitClassId));
	if (record == nullptr) {
		return false;
	}
	const int32_t monsterId = g_evaluateItemFormula(bank, pUnit, pItem, ReadU32(record, Off::ItemsTxtCalc1));
	const int32_t unique    = g_evaluateItemFormula(bank, pUnit, pItem, ReadU32(record, Off::ItemsTxtCalc2));
	const int32_t count     = g_evaluateItemFormula(bank, pUnit, pItem, ReadU32(record, Off::ItemsTxtCalc3));
	const uint32_t limit    = g_monsterRowLimit.load(std::memory_order_acquire);
	// The pack spawner takes monstats rows only (the preset spawner reads larger ids as superuniques).
	if (monsterId < 0 || limit == 0 || static_cast<uint32_t>(monsterId) >= limit) {
		WarnOnce(g_warnedBadMonster, "pspell-ext: a summon/ambush item's calc1 is not a monstats.txt row; the item was not used.");
		return false;
	}
	spec.monsterId = monsterId;
	spec.unique    = unique == 1;
	spec.count     = count < 1 ? 1U : (count > static_cast<int32_t>(kMaxPacks) ? kMaxPacks : static_cast<uint32_t>(count));
	return true;
}

// The Evil Urn's pick: the first monster of the level's monster region that can walk; -1 if none.
static auto UrnMonster(void* pGame, int32_t levelId) -> int32_t {
	const uint32_t limit = g_levelRowLimit.load(std::memory_order_acquire);
	const void* regions  = ReadPtr(pGame, Off::GameMonsterRegions);
	if (levelId < 1 || limit == 0 || static_cast<uint32_t>(levelId) >= limit || regions == nullptr) {
		return -1;
	}
	const auto* region = static_cast<const uint8_t*>(ReadPtr(regions, static_cast<size_t>(levelId) * sizeof(void*)));
	if (region == nullptr) {
		return -1;
	}
	const uint8_t bank  = GameTableSet(pGame);
	const uint32_t rows = g_monsterRowLimit.load(std::memory_order_acquire);
	for (uint32_t i = 0; i < region[Off::MonRegCount]; ++i) {
		const int32_t id = static_cast<int16_t>(ReadU16(region, Off::MonRegData + i * Off::MonRegDataStride));
		if (id < 0 || static_cast<uint32_t>(id) >= rows) {
			continue;
		}
		const uint8_t* stats2 = g_getMonStats2TxtRecord(bank, id);
		if (stats2 != nullptr && (stats2[Off::MonStats2ModeFlags] & kMonStats2CanWalk) != 0) {
			return id;
		}
	}
	return -1;
}

// spec.count packs at x/y in pRoom (the game finds free spots nearby). Returns how many spawned.
static auto SpawnPacks(void* pGame, void* pRoom, int32_t x, int32_t y, const PackSpec& spec) -> uint32_t {
	if (pRoom == nullptr || x <= 0 || y <= 0 || x > 0xFFFF || y > 0xFFFF) {
		return 0;
	}
	const int32_t monsterId = spec.monsterId != 0 ? spec.monsterId : UrnMonster(pGame, g_getLevelIdFromRoom(pRoom));
	if (monsterId < 0) {
		return 0;
	}
	uint32_t spawned = 0;
	for (uint32_t i = 0; i < spec.count; ++i) {
		// The urn's arguments, with a position instead of a random spot in the room.
		if (g_spawnBossPack(pGame, pRoom, nullptr, monsterId, spec.unique ? 0 : 1, static_cast<uint16_t>(x), static_cast<uint16_t>(y), 1, 0) != nullptr) {
			++spawned;
		}
	}
	g_packsSpawned.fetch_add(spawned, std::memory_order_relaxed);
	return spawned;
}

static auto RefusePack(void* pUnit, std::atomic<uint32_t>& counter) -> bool {
	g_attachSound(pUnit, kSoundCannotUse, nullptr);
	counter.fetch_add(1, std::memory_order_relaxed);
	return false;
}

// Prepare stage of the summon pSpell: packs summon_distance subtiles in front of the
// player (else at the player). false = the item stays.
static auto Summon(void* pGame, void* pUnit, void* pItem) -> bool {
	if (pUnit == nullptr || ReadU32(pUnit, Off::UnitType) != kUnitTypePlayer) {
		return false;
	}
	if (IsInTown(pUnit)) {
		g_attachSound(pUnit, kSoundTownRefused, nullptr);
		g_townRefusals.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	PackSpec spec;
	const void* path = UnitPath(pUnit);
	void* room       = path != nullptr ? ReadPtr(path, Off::PathRoom) : nullptr;
	int32_t x = 0;
	int32_t y = 0;
	if (room == nullptr || !ReadPackSpec(pGame, pUnit, pItem, spec) || !UnitPosition(pUnit, x, y)) {
		return RefusePack(pUnit, g_summonRefusals);
	}
	// A far point can be in a wall or outside the loaded rooms: step back towards the player.
	constexpr uint32_t kStepBack = 5;
	for (uint32_t distance = g_config.summonDistance;; distance = distance > kStepBack ? distance - kStepBack : 0) {
		int32_t fx = x;
		int32_t fy = y;
		void* spawnRoom = room;
		if (distance != 0 && PointInFront(pUnit, distance, fx, fy)) {
			spawnRoom = g_getRoomAt(room, fx, fy);
		}
		if (spawnRoom != nullptr && SpawnPacks(pGame, spawnRoom, fx, fy, spec) != 0) {
			return true;
		}
		if (distance == 0) {
			return RefusePack(pUnit, g_summonRefusals);
		}
	}
}

// Prepare stage of the ambush pSpell: queue the packs for the next superunique of this
// game. Works in town. false = the item stays.
static auto QueueAmbush(void* pGame, void* pUnit, void* pItem) -> bool {
	if (pUnit == nullptr || ReadU32(pUnit, Off::UnitType) != kUnitTypePlayer) {
		return false;
	}
	PackSpec spec;
	if (!ReadPackSpec(pGame, pUnit, pItem, spec)) {
		return RefusePack(pUnit, g_ambushRefusals);
	}
	{
		const std::lock_guard<std::mutex> lock(g_ambushMutex);
		std::erase_if(g_pendingAmbush, [pGame](const PendingAmbush& p) { return p.game != pGame; }); // left over from an older game
		if (g_pendingAmbush.size() >= kMaxAmbushQueue) {
			return RefusePack(pUnit, g_ambushRefusals);
		}
		g_pendingAmbush.push_back(PendingAmbush { pGame, spec });
	}
	g_ambushesQueued.fetch_add(1, std::memory_order_relaxed);
	return true;
}

// D2GAME_SpawnSuperUnique hook: the game's spawn first; when it made the boss, the
// oldest queued ambush of this game spawns where the boss stands.
static auto __fastcall HookSpawnSuperUnique(void* pGame, void* pRoom, int32_t nX, int32_t nY, int32_t nSuperUnique, uint16_t nFlags) -> void* {
	void* boss = g_originalSpawnSuperUnique != nullptr ? g_originalSpawnSuperUnique(pGame, pRoom, nX, nY, nSuperUnique, nFlags) : nullptr;
	if (boss == nullptr || pGame == nullptr || !g_slotsActive || !g_config.enabled || ReadU32(boss, Off::UnitType) != kUnitTypeMonster) {
		return boss;
	}
	PackSpec spec;
	{
		const std::lock_guard<std::mutex> lock(g_ambushMutex);
		const auto it = std::find_if(g_pendingAmbush.begin(), g_pendingAmbush.end(), [pGame](const PendingAmbush& p) { return p.game == pGame; });
		if (it == g_pendingAmbush.end()) {
			return boss;
		}
		spec = it->spec;
		g_pendingAmbush.erase(it);
	}
	int32_t x  = 0;
	int32_t y  = 0;
	void* room = g_getUnitRoom(boss);
	if (UnitPosition(boss, x, y) && SpawnPacks(pGame, room != nullptr ? room : pRoom, x, y, spec) != 0) {
		g_ambushesSprung.fetch_add(1, std::memory_order_relaxed);
	}
	return boss;
}

static_assert(std::is_same_v<decltype(&HookSpawnSuperUnique), SpawnSuperUniqueFn>);

// ---------------------------------------------------------------------------
// Message scroll pSpell: calc1 = message number. Right-click shows the string
// message_format % calc1 (pspell-ext' own strings) in the NPC speech panel at the
// top of the screen, through the game's quest scroll message (packet 0x27, so TCP/IP
// clients get it too, in their own language). The item is kept.
// ---------------------------------------------------------------------------
static const D2RL::LocalizationService* g_localization = nullptr;
static std::atomic<uint32_t> g_messagesShown { 0 };
static std::atomic<uint32_t> g_messageMissing { 0 };
static std::atomic<bool>     g_warnedBadMessage { false };
static std::atomic<bool>     g_warnedMissingMessage { false };
static std::atomic<bool>     g_warnedNoLocalization { false };
static std::atomic<bool>     g_warnedBadMessageStat { false };
inline constexpr uint32_t kMaxStatId = 0x8000; // ReadWideUnitBaseStat bound

// The message's local key ("scrollMsg0013"), or "" when there is no number: calc1, else
// (calc1 blank or 0, like the 1.10f ScrollItem) the item's message_stat, e.g. rolled by an
// automagic affix so one base item can carry different messages.
static auto MessageKey(void* pGame, void* pUnit, void* pItem) -> std::string {
	const uint8_t bank    = GameTableSet(pGame);
	const uint8_t* record = g_getItemsTxtRecord(bank, ReadI32(pItem, Off::UnitClassId));
	int32_t number        = record != nullptr ? g_evaluateItemFormula(bank, pUnit, pItem, ReadU32(record, Off::ItemsTxtCalc1)) : 0;
	if (number < 1 && g_config.messageStat != 0) {
		// D2RCore ReadWideUnitStat: ids >= 0x8000 give 0, any other id is looked up in the item's combined
		// stats (its own plus attached lists: properties live there, not in the base array - in game the base
		// read gave 0) and gives 0 when absent. No row check: the banks differ in row count.
		if (g_config.messageStat >= kMaxStatId) {
			WarnOnce(g_warnedBadMessageStat, "pspell-ext: message_stat is not a stat id (0-32767); stat messages are off.");
		} else {
			number = g_getUnitStat(pItem, static_cast<int32_t>(g_config.messageStat), 0);
		}
	}
	if (number < 1) {
		return "";
	}
	char key[64] {};
	// The format was checked by IsSafeMessageFormat (exactly one integer conversion).
#pragma warning(suppress : 4774)
	std::snprintf(key, sizeof(key), g_config.messageFormat.c_str(), number);
	return key;
}

static auto RefuseMessage(void* pUnit) -> bool {
	g_attachSound(pUnit, kSoundCannotUse, nullptr);
	g_messageMissing.fetch_add(1, std::memory_order_relaxed);
	return false;
}

// Prepare stage of the message pSpell. Always false: the item stays and Do never runs.
static auto ShowMessage(void* pGame, void* pUnit, void* pItem) -> bool {
	if (pUnit == nullptr || ReadU32(pUnit, Off::UnitType) != kUnitTypePlayer) {
		return false;
	}
	const std::string key = MessageKey(pGame, pUnit, pItem);
	if (key.empty()) {
		WarnOnce(g_warnedBadMessage, "pspell-ext: a message item has no message number (calc1 below 1 and no message_stat value); nothing shown.");
		return RefuseMessage(pUnit);
	}
	// The id lookup gives a placeholder for an unknown key, so check the key exists first.
	if (g_localization == nullptr) {
		WarnOnce(g_warnedNoLocalization, "pspell-ext: LocalizationService missing; message items can't be read.");
		return RefuseMessage(pUnit);
	}
	uint32_t required = 0;
	const D2RL::Localization::Result found = g_localization->getStringByKey(g_ctx, key.c_str(), nullptr, 0, &required);
	if (found != D2RL::Localization::Result::Success && found != D2RL::Localization::Result::BufferTooSmall) {
		if (!g_warnedMissingMessage.exchange(true) && g_ctx != nullptr) {
			const std::string msg = "pspell-ext: no string '" + key + "' in pspell-ext' strings.json; nothing shown (logged once).";
			DiagWarn(g_ctx, msg.c_str());
		}
		return RefuseMessage(pUnit);
	}
	const std::string qualified = "pspell-ext:" + key; // PluginInfo.id: the strings.json namespace
	const StringKeyView view { qualified.c_str(), qualified.size() };
	g_sendScrollMessage(pGame, pUnit, pUnit, g_getNamespacedStringId(&view));
	g_messagesShown.fetch_add(1, std::memory_order_relaxed);
	return false;
}

// SKILLITEM_pSpell_PrepareHandler hook. Our pSpells never reach gpPSpellPrepareTable
// (their check index 0 is not a function).
static auto __fastcall HookPrepare(void* pGame, void* pUnit, void* pItem, void* pTargetItem) -> bool {
	if (const uint32_t slot = FreeSlotOf(pGame, pItem); slot != 0) {
		if (IsConfiguredSlot(slot)) {
			if (slot == g_config.portalSlot) {
				return OpenPortal(pGame, pUnit, pItem);
			}
			if (slot == g_config.tomeSlot) {
				return ReadTome(pGame, pUnit, pItem);
			}
			if (slot == g_config.summonSlot) {
				return Summon(pGame, pUnit, pItem);
			}
			if (slot == g_config.messageSlot) {
				return ShowMessage(pGame, pUnit, pItem);
			}
			return slot == g_config.ambushSlot ? QueueAmbush(pGame, pUnit, pItem) : CanUse(pGame, pUnit, pItem);
		}
		// An unconfigured 16-30 item, or the plugin is disabled: never let it reach the table.
		if (pUnit != nullptr) {
			g_attachSound(pUnit, kSoundCannotUse, nullptr);
		}
		return false;
	}
	return g_originalPrepare != nullptr && g_originalPrepare(pGame, pUnit, pItem, pTargetItem);
}

// SKILLITEM_pSpell_DoHandler hook. Runs only after the prepare stage said yes;
// the game consumes the item afterwards whatever happens here.
static void __fastcall HookDo(void* pGame, void* pUnit, void* pItem, void* pTargetItem) {
	const uint32_t slot = FreeSlotOf(pGame, pItem);
	if (slot == 0) {
		if (g_originalDo != nullptr) {
			g_originalDo(pGame, pUnit, pItem, pTargetItem);
		}
		return;
	}
	if (!IsConfiguredSlot(slot) || pUnit == nullptr || ReadU32(pUnit, Off::UnitType) != kUnitTypePlayer) {
		return;
	}
	if (slot == g_config.targetSlot) {
		CastOnNearest(pGame, pUnit, pItem);
	} else if (slot == g_config.positionSlot) {
		CastAtPlayer(pGame, pUnit, pItem);
	}
	// Portal, tome, summon and ambush slots: done in the prepare stage.
}

static_assert(std::is_same_v<decltype(&HookPrepare), PrepareHandlerFn>);
static_assert(std::is_same_v<decltype(&HookDo), DoHandlerFn>);
static_assert(std::is_same_v<decltype(&IsHostileTarget), UnitTestFn>);
static_assert(std::is_same_v<decltype(&HookGetDescriptor), GetPSpellDescriptorFn>);
static_assert(std::is_same_v<decltype(&HookIsUsableNoTarget), IsUsableNoTargetFn>);
static_assert(std::is_same_v<decltype(&HookDrawHoverUnitLabel), DrawHoverUnitLabelFn>);

// ---------------------------------------------------------------------------
// Skills row limit (calc1 validation), rebuilt whenever tables load.
// ---------------------------------------------------------------------------
static const D2RL::DataTableService* g_dataTables = nullptr;

// Reads the game's pSpell 16-19 descriptor records. At plugin load they don't hold
// their final values yet (the load-time check failed on the first run), so the
// result that counts is the one taken after the data tables load.
static auto CheckGameSlotsEmpty(const D2RL::PluginContext* ctx, const char* when) -> bool {
	if (g_descTable == 0) {
		return false;
	}
	constexpr size_t kBytes = (Table::FirstExtraSlot - Table::FirstFreeSlot) * Table::DescStride;
	uint8_t bytes[kBytes] {};
	std::memcpy(bytes, reinterpret_cast<const void*>(g_descTable + Table::FirstFreeSlot * Table::DescStride), kBytes);
	bool empty = true;
	for (const uint8_t b : bytes) {
		empty = empty && b == 0;
	}
	if (!empty) {
		char hex[kBytes * 3 + 1] {};
		for (size_t i = 0; i < kBytes; ++i) {
			std::snprintf(hex + i * 3, 4, "%02X ", bytes[i]);
		}
		char msg[256] {};
		std::snprintf(msg, sizeof(msg), "pspell-ext: game pSpell 16-19 records are not empty (%s): %s", when, hex);
		DiagWarn(ctx, msg);
	}
	return empty;
}

static void __cdecl OnDataTablesLoaded(const D2RL::PluginContext* ctx, const D2RL::Lifecycle::DataTablesLoadedEvent* ev, void*) noexcept {
	if (ctx == nullptr || g_dataTables == nullptr || !D2RL::Lifecycle::HasDataTablesLoadedEventField(ev, D2RL::Lifecycle::DataTablesLoadedEventRequiredSize)) {
		return;
	}
	const bool gameSlotsEmpty = CheckGameSlotsEmpty(ctx, "after table load");
	g_gameSlotsEmpty.store(gameSlotsEmpty, std::memory_order_release);
	if (!gameSlotsEmpty) {
		DiagWarn(ctx, "pspell-ext: pSpell 16-19 are left to the game; only 20-30 work.");
	}
	// Smallest row count over the loaded banks.
	auto rowLimit = [ctx](D2RL::DataTables::TableId table) -> uint32_t {
		uint32_t limit = 0;
		for (const D2RL::DataTables::Bank bank : { D2RL::DataTables::Bank::Classic, D2RL::DataTables::Bank::Lod, D2RL::DataTables::Bank::Rotw }) {
			D2RL::DataTables::TableView view { .structSize = D2RL::DataTables::TableViewSize };
			if (g_dataTables->getTable(ctx, bank, table, &view) == D2RL::DataTables::Result::Success && view.rowCount != 0) {
				limit = (limit == 0 || view.rowCount < limit) ? view.rowCount : limit;
			}
		}
		return limit;
	};
	const uint32_t skills = rowLimit(D2RL::DataTables::TableId::Skills);
	const uint32_t levels   = rowLimit(D2RL::DataTables::TableId::Levels);
	const uint32_t monsters = rowLimit(D2RL::DataTables::TableId::MonStats);
	g_skillRowLimit.store(skills, std::memory_order_release);
	g_levelRowLimit.store(levels, std::memory_order_release);
	g_monsterRowLimit.store(monsters, std::memory_order_release);
	char msg[160] {};
	std::snprintf(msg, sizeof(msg), "pspell-ext: skill ids below %u, level ids below %u and monster ids below %u are accepted.", skills, levels, monsters);
	DiagInfo(ctx, msg);
}

// ---------------------------------------------------------------------------
// Installation.
// ---------------------------------------------------------------------------
struct NativeRef {
	const char* name;
	uint64_t    rva;
	ByteSpan    expected;
};

static constexpr std::array<NativeRef, 47> kNatives { {
	{ "STATLIST_GetUnitStat (ReadWideUnitStat stub)", Rva::GetUnitStat, ByteSpan { Expected::kGetUnitStat } },
	{ "QUESTS_SendScrollMessage", Rva::SendScrollMessage, ByteSpan { Expected::kSendScrollMessage } },
	{ "GetNamespacedStringId (D2RCore stub)", Rva::GetNamespacedStringId, ByteSpan { Expected::kGetNamespacedStringId } },
	{ "sub_6FC6E8D0 (boss pack)", Rva::SpawnBossPack, ByteSpan { Expected::kSpawnBossPack } },
	{ "D2GAME_SpawnSuperUnique", Rva::SpawnSuperUnique, ByteSpan { Expected::kSpawnSuperUnique } },
	{ "UNITS_GetMonStats2TxtRecordFromMonsterId", Rva::GetMonStats2TxtRecord, ByteSpan { Expected::kGetMonStats2TxtRecord } },
	{ "D2GAME_CheckSkillIfUnitsClassSkill", Rva::CheckClassSkill, ByteSpan { Expected::kCheckClassSkill } },
	{ "SKILLS_CheckRequiredSkills", Rva::CheckRequiredSkills, ByteSpan { Expected::kCheckRequiredSkills } },
	{ "SKILLS_CheckRequiredAttributes", Rva::CheckRequiredAttributes, ByteSpan { Expected::kCheckRequiredAttributes } },
	{ "SKILLS_GetSkillById", Rva::GetSkillById, ByteSpan { Expected::kGetSkillById } },
	{ "SKILLS_GetSkillLevel", Rva::GetSkillLevel, ByteSpan { Expected::kGetSkillLevel } },
	{ "SKILLS_EvaluateSkillFormula", Rva::EvaluateSkillFormula, ByteSpan { Expected::kEvaluateSkillFormula } },
	{ "STATLIST_GetUnitBaseStat", Rva::GetUnitBaseStat, ByteSpan { Expected::kGetUnitBaseStat } },
	{ "STATLIST_AddUnitStat", Rva::AddUnitStat, ByteSpan { Expected::kAddUnitStat } },
	{ "sub_6FD136E0 (spend skill point)", Rva::SpendSkillPoint, ByteSpan { Expected::kSpendSkillPoint } },
	{ "maybe_OBJECT_SetInteractType", Rva::SetInteractType, ByteSpan { Expected::kSetInteractType } },
	{ "maybe_D2GAME_CreatePortalObject", Rva::CreatePortalObject, ByteSpan { Expected::kCreatePortalObject } },
	{ "maybe_DATATBLS_GetLevelsTxtRecord", Rva::GetLevelsTxtRecord, ByteSpan { Expected::kGetLevelsTxtRecord } },
	{ "maybe_DUNGEON_GetLevelIdFromRoom", Rva::GetLevelIdFromRoom, ByteSpan { Expected::kGetLevelIdFromRoom } },
	{ "maybe_DUNGEON_GetTownLevelIdFromActNo", Rva::GetTownLevelIdFromActNo, ByteSpan { Expected::kGetTownLevelIdFromActNo } },
	{ "maybe_WAYPOINTS_GetWaypointNoFromLevelId", Rva::GetWaypointNoFromLevelId, ByteSpan { Expected::kGetWaypointNoFromLevelId } },
	{ "maybe_LEVEL_WarpUnit", Rva::WarpUnit, ByteSpan { Expected::kWarpUnit } },
	{ "maybe_SUNIT_AllocUnitData", Rva::AllocUnitData, ByteSpan { Expected::kAllocUnitData } },
	{ "maybe_D2GAME_GetRoom", Rva::GetRoomAt, ByteSpan { Expected::kGetRoomAt } },
	{ "maybe_COLLISION_GetFreeCoordinatesWithField", Rva::GetFreeCoordinates, ByteSpan { Expected::kGetFreeCoordinates } },
	{ "UNITS_ChangeAnimMode", Rva::ChangeAnimMode, ByteSpan { Expected::kChangeAnimMode } },
	{ "UNITS_GetPlayerData", Rva::GetPlayerData, ByteSpan { Expected::kGetPlayerData } },
	{ "OBJECTS_OperateFunction15_Portal", Rva::PortalOperate, ByteSpan { Expected::kPortalOperate } },
	{ "maybe_sub_6FCBDFE0 (teleport)", Rva::TeleportUnit, ByteSpan { Expected::kTeleportUnit } },
	{ "UNITS_GetRoom", Rva::GetUnitRoom, ByteSpan { Expected::kGetUnitRoom } },
	{ "maybe_D2CLIENT_UI_DrawHoverUnitLabel", Rva::DrawHoverUnitLabel, ByteSpan { Expected::kDrawHoverUnitLabel } },
	{ "SUNIT_GetServerUnit", Rva::GetServerUnit, ByteSpan { Expected::kGetServerUnit } },
	{ "STATES_CheckState", Rva::CheckState, ByteSpan { Expected::kCheckState } },
	{ "SKILLS_TestLineOfSight", Rva::TestLineOfSight, ByteSpan { Expected::kTestLineOfSight } },
	{ "ITEMS_IsPSpellUsableWithoutTarget", Rva::IsPSpellUsableNoTarget, ByteSpan { Expected::kIsPSpellUsableNoTarget } },
	{ "DATATBLS_GetItemsTxtRecord", Rva::GetItemsTxtRecord, ByteSpan { Expected::kGetItemsTxtRecord } },
	{ "DATATBLS_GetItemPSpellDescriptor", Rva::GetPSpellDescriptor, ByteSpan { Expected::kGetPSpellDescriptor } },
	{ "SKILLITEM_pSpell_PrepareHandler", Rva::PrepareHandler, ByteSpan { Expected::kPrepareHandler } },
	{ "SKILLITEM_pSpell_DoHandler", Rva::DoHandler, ByteSpan { Expected::kDoHandler } },
	{ "ITEMMODS_EvaluateItemFormula", Rva::EvaluateItemFormula, ByteSpan { Expected::kEvaluateItemFormula } },
	{ "DATATBLS_GetSkillsTxtRecord", Rva::GetSkillsTxtRecord, ByteSpan { Expected::kGetSkillsTxtRecord } },
	{ "SKILLITEM_CastSkillOnTarget", Rva::CastSkillOnTarget, ByteSpan { Expected::kCastSkillOnTarget } },
	{ "SKILLITEM_CastSkillOnPosition", Rva::CastSkillOnPosition, ByteSpan { Expected::kCastSkillOnPosition } },
	{ "UNITFINDS_GetNearestTestedUnit", Rva::GetNearestTestedUnit, ByteSpan { Expected::kGetNearestTestedUnit } },
	{ "STATLIST_GetUnitAlignment", Rva::GetUnitAlignment, ByteSpan { Expected::kGetUnitAlignment } },
	{ "DUNGEON_IsRoomInTown", Rva::IsRoomInTown, ByteSpan { Expected::kIsRoomInTown } },
	{ "SUNIT_AttachSound", Rva::AttachSound, ByteSpan { Expected::kAttachSound } },
} };

static auto Expect(const D2RL::PluginContext* ctx, uint64_t rva, ByteSpan bytes, const char* what) -> bool {
	if (ctx->CheckExpectedBytes(rva, bytes.data(), static_cast<uint32_t>(bytes.size()))) {
		return true;
	}
	char msg[192] {};
	std::snprintf(msg, sizeof(msg), "pspell-ext: unexpected bytes at RVA 0x%llx (%s); nothing was changed.", static_cast<unsigned long long>(rva), what);
	ctx->LogError(msg);
	return false;
}


static auto StageHook(const D2RL::PluginContext* ctx, const D2RL::MutationService* mut, D2RL::Mutations::TransactionHandle tx, uint64_t rva, ByteSpan expected,
                      void* target, D2RL::Mutations::OperationHandle& op) -> bool {
	const D2RL::Mutations::InlineHookRequest hook {
		.structSize   = D2RL::Mutations::InlineHookRequestSize,
		.rva          = rva,
		.expected     = expected.data(),
		.expectedSize = static_cast<uint32_t>(expected.size()),
		.target       = target,
	};
	if (mut->stageInlineHook(ctx, tx, &hook, &op) == D2RL::Mutations::Result::Success) {
		return true;
	}
	char msg[128] {};
	std::snprintf(msg, sizeof(msg), "pspell-ext: staging the hook at RVA 0x%llx failed; nothing was changed.", static_cast<unsigned long long>(rva));
	ctx->LogError(msg);
	return false;
}

static auto InstallSlots(const D2RL::PluginContext* ctx) -> bool {
	bool bytesOk = true;
	for (const NativeRef& ref : kNatives) {
		bytesOk = Expect(ctx, ref.rva, ref.expected, ref.name) && bytesOk;
	}
	if (!bytesOk) {
		return false;
	}

	const D2RL::MutationService* mut = nullptr;
	if (ctx->QueryService(&mut) != D2RL::ServiceQueryResult::Success || !D2RL::HasMutationServiceField(mut, D2RL::MutationServiceRequiredSize)) {
		ctx->LogError("pspell-ext: MutationService unavailable; cannot add the pSpells.");
		return false;
	}

	g_getItemsTxtRecord    = reinterpret_cast<GetItemsTxtRecordFn>(ctx->exeBase + Rva::GetItemsTxtRecord);
	g_getPSpellDescriptor  = reinterpret_cast<GetPSpellDescriptorFn>(ctx->exeBase + Rva::GetPSpellDescriptor);
	g_evaluateItemFormula  = reinterpret_cast<EvaluateItemFormulaFn>(ctx->exeBase + Rva::EvaluateItemFormula);
	g_getSkillsTxtRecord   = reinterpret_cast<GetSkillsTxtRecordFn>(ctx->exeBase + Rva::GetSkillsTxtRecord);
	g_castSkillOnTarget    = reinterpret_cast<CastSkillOnTargetFn>(ctx->exeBase + Rva::CastSkillOnTarget);
	g_castSkillOnPosition  = reinterpret_cast<CastSkillOnPositionFn>(ctx->exeBase + Rva::CastSkillOnPosition);
	g_getNearestTestedUnit = reinterpret_cast<GetNearestTestedUnitFn>(ctx->exeBase + Rva::GetNearestTestedUnit);
	g_getUnitAlignment     = reinterpret_cast<GetUnitAlignmentFn>(ctx->exeBase + Rva::GetUnitAlignment);
	g_isRoomInTown         = reinterpret_cast<IsRoomInTownFn>(ctx->exeBase + Rva::IsRoomInTown);
	g_attachSound          = reinterpret_cast<AttachSoundFn>(ctx->exeBase + Rva::AttachSound);
	g_checkState           = reinterpret_cast<CheckStateFn>(ctx->exeBase + Rva::CheckState);
	g_testLineOfSight      = reinterpret_cast<TestLineOfSightFn>(ctx->exeBase + Rva::TestLineOfSight);
	g_getServerUnit        = reinterpret_cast<GetServerUnitFn>(ctx->exeBase + Rva::GetServerUnit);
	g_exeBase              = static_cast<uintptr_t>(ctx->exeBase);
	g_createPortalObject   = reinterpret_cast<CreatePortalObjectFn>(ctx->exeBase + Rva::CreatePortalObject);
	g_getLevelsTxtRecord   = reinterpret_cast<GetLevelsTxtRecordFn>(ctx->exeBase + Rva::GetLevelsTxtRecord);
	g_getLevelIdFromRoom   = reinterpret_cast<GetLevelIdFromRoomFn>(ctx->exeBase + Rva::GetLevelIdFromRoom);
	g_getTownLevelId       = reinterpret_cast<GetTownLevelIdFn>(ctx->exeBase + Rva::GetTownLevelIdFromActNo);
	g_getWaypointNo        = reinterpret_cast<GetWaypointNoFn>(ctx->exeBase + Rva::GetWaypointNoFromLevelId);
	g_warpUnit             = reinterpret_cast<WarpUnitFn>(ctx->exeBase + Rva::WarpUnit);
	g_allocUnitData        = reinterpret_cast<AllocUnitDataFn>(ctx->exeBase + Rva::AllocUnitData);
	g_getRoomAt            = reinterpret_cast<GetRoomAtFn>(ctx->exeBase + Rva::GetRoomAt);
	g_getFreeCoordinates   = reinterpret_cast<GetFreeCoordinatesFn>(ctx->exeBase + Rva::GetFreeCoordinates);
	g_changeAnimMode       = reinterpret_cast<ChangeAnimModeFn>(ctx->exeBase + Rva::ChangeAnimMode);
	g_getPlayerData        = reinterpret_cast<GetPlayerDataFn>(ctx->exeBase + Rva::GetPlayerData);
	g_teleportUnit         = reinterpret_cast<TeleportUnitFn>(ctx->exeBase + Rva::TeleportUnit);
	g_getUnitRoom          = reinterpret_cast<GetUnitRoomFn>(ctx->exeBase + Rva::GetUnitRoom);
	g_setInteractType      = reinterpret_cast<SetInteractTypeFn>(ctx->exeBase + Rva::SetInteractType);
	g_checkClassSkill      = reinterpret_cast<SkillCheckFn>(ctx->exeBase + Rva::CheckClassSkill);
	g_checkRequiredSkills  = reinterpret_cast<SkillCheckFn>(ctx->exeBase + Rva::CheckRequiredSkills);
	g_checkRequiredAttributes = reinterpret_cast<SkillCheckFn>(ctx->exeBase + Rva::CheckRequiredAttributes);
	g_getSkillById         = reinterpret_cast<GetSkillByIdFn>(ctx->exeBase + Rva::GetSkillById);
	g_getSkillLevel        = reinterpret_cast<GetSkillLevelFn>(ctx->exeBase + Rva::GetSkillLevel);
	g_evaluateSkillFormula = reinterpret_cast<EvaluateSkillFormulaFn>(ctx->exeBase + Rva::EvaluateSkillFormula);
	g_getUnitBaseStat      = reinterpret_cast<GetUnitBaseStatFn>(ctx->exeBase + Rva::GetUnitBaseStat);
	g_getUnitStat          = reinterpret_cast<GetUnitBaseStatFn>(ctx->exeBase + Rva::GetUnitStat);
	g_addUnitStat          = reinterpret_cast<AddUnitStatFn>(ctx->exeBase + Rva::AddUnitStat);
	g_spendSkillPoint      = reinterpret_cast<SpendSkillPointFn>(ctx->exeBase + Rva::SpendSkillPoint);
	g_spawnBossPack        = reinterpret_cast<SpawnBossPackFn>(ctx->exeBase + Rva::SpawnBossPack);
	g_getMonStats2TxtRecord = reinterpret_cast<GetMonStats2TxtRecordFn>(ctx->exeBase + Rva::GetMonStats2TxtRecord);
	g_sendScrollMessage    = reinterpret_cast<SendScrollMessageFn>(ctx->exeBase + Rva::SendScrollMessage);
	g_getNamespacedStringId = reinterpret_cast<GetNamespacedStringIdFn>(ctx->exeBase + Rva::GetNamespacedStringId);
	g_descTable            = static_cast<uintptr_t>(ctx->exeBase + Rva::PSpellDescTable);
	// Diagnostic only; the decision is made again in OnDataTablesLoaded.
	(void)CheckGameSlotsEmpty(ctx, "at plugin load");

	D2RL::Mutations::TransactionHandle tx = D2RL::Mutations::InvalidTransactionHandle;
	if (mut->beginTransaction(ctx, &tx) != D2RL::Mutations::Result::Success) {
		ctx->LogError("pspell-ext: beginTransaction failed.");
		return false;
	}
	D2RL::Mutations::OperationHandle prepareOp = D2RL::Mutations::InvalidOperationHandle;
	D2RL::Mutations::OperationHandle doOp      = D2RL::Mutations::InvalidOperationHandle;
	D2RL::Mutations::OperationHandle descOp    = D2RL::Mutations::InvalidOperationHandle;
	D2RL::Mutations::OperationHandle usableOp  = D2RL::Mutations::InvalidOperationHandle;
	D2RL::Mutations::OperationHandle hoverOp   = D2RL::Mutations::InvalidOperationHandle;
	D2RL::Mutations::OperationHandle portalOp  = D2RL::Mutations::InvalidOperationHandle;
	D2RL::Mutations::OperationHandle superOp   = D2RL::Mutations::InvalidOperationHandle;
	if (!StageHook(ctx, mut, tx, Rva::PrepareHandler, ByteSpan { Expected::kPrepareHandler }, reinterpret_cast<void*>(&HookPrepare), prepareOp)
	    || !StageHook(ctx, mut, tx, Rva::DoHandler, ByteSpan { Expected::kDoHandler }, reinterpret_cast<void*>(&HookDo), doOp)
	    || !StageHook(ctx, mut, tx, Rva::GetPSpellDescriptor, ByteSpan { Expected::kGetPSpellDescriptor }, reinterpret_cast<void*>(&HookGetDescriptor), descOp)
	    || !StageHook(ctx, mut, tx, Rva::IsPSpellUsableNoTarget, ByteSpan { Expected::kIsPSpellUsableNoTarget }, reinterpret_cast<void*>(&HookIsUsableNoTarget),
	                  usableOp)
	    || !StageHook(ctx, mut, tx, Rva::DrawHoverUnitLabel, ByteSpan { Expected::kDrawHoverUnitLabel }, reinterpret_cast<void*>(&HookDrawHoverUnitLabel),
	                  hoverOp)
	    || !StageHook(ctx, mut, tx, Rva::PortalOperate, ByteSpan { Expected::kPortalOperate }, reinterpret_cast<void*>(&HookPortalOperate), portalOp)
	    || !StageHook(ctx, mut, tx, Rva::SpawnSuperUnique, ByteSpan { Expected::kSpawnSuperUnique }, reinterpret_cast<void*>(&HookSpawnSuperUnique), superOp)) {
		mut->cancelTransaction(ctx, tx);
		return false;
	}

	D2RL::Mutations::CommitResult result { .structSize = D2RL::Mutations::CommitResultSize };
	if (mut->commit(ctx, tx, &result) != D2RL::Mutations::Result::Success || result.result != D2RL::Mutations::Result::Success) {
		char msg[160] {};
		std::snprintf(msg, sizeof(msg), "pspell-ext: commit failed (result %u, reason %u); original bytes restored.", static_cast<unsigned>(result.result),
		              static_cast<unsigned>(result.failureReason));
		ctx->LogError(msg);
		return false;
	}
	// Until the originals are set the hooks refuse (prepare, usable), find nothing
	// (descriptor) or do nothing (do); g_slotsActive stays false meanwhile.
	auto original = [&](D2RL::Mutations::OperationHandle op) -> void* {
		void* fn = nullptr;
		return mut->getInlineHookOriginal(ctx, tx, op, &fn) == D2RL::Mutations::Result::Success ? fn : nullptr;
	};
	g_originalGetDesc  = reinterpret_cast<GetPSpellDescriptorFn>(original(descOp));
	g_originalIsUsable = reinterpret_cast<IsUsableNoTargetFn>(original(usableOp));
	g_originalDrawHover = reinterpret_cast<DrawHoverUnitLabelFn>(original(hoverOp));
	g_originalPortalOperate = reinterpret_cast<PortalOperateFn>(original(portalOp));
	g_originalSpawnSuperUnique = reinterpret_cast<SpawnSuperUniqueFn>(original(superOp));
	g_originalPrepare  = reinterpret_cast<PrepareHandlerFn>(original(prepareOp));
	g_originalDo       = reinterpret_cast<DoHandlerFn>(original(doOp));
	if (g_originalPrepare == nullptr || g_originalDo == nullptr || g_originalGetDesc == nullptr || g_originalIsUsable == nullptr
	    || g_originalDrawHover == nullptr || g_originalPortalOperate == nullptr || g_originalSpawnSuperUnique == nullptr) {
		ctx->LogError("pspell-ext: hooks committed but the original handlers are missing; item spells will not work.");
		return false;
	}
	g_slotsActive = true;

	char msg[256] {};
	std::snprintf(msg, sizeof(msg),
	              "pspell-ext: pSpell %u (nearest enemy), %u (position), %u (red portal), %u (skill tome), %u (summon), %u (ambush) and %u (message) added.",
	              g_config.targetSlot, g_config.positionSlot, g_config.portalSlot, g_config.tomeSlot, g_config.summonSlot, g_config.ambushSlot, g_config.messageSlot);
	DiagInfo(ctx, msg);
	return true;
}

static void __cdecl OnGameLeft(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent* ev, void*) noexcept {
	if (D2RL::Lifecycle::HasGameplayEventField(ev, D2RL::Lifecycle::GameplayEventRequiredSize) && ev->kind == D2RL::Lifecycle::GameplayEventKind::GameLeft) {
		const std::lock_guard<std::mutex> lock(g_ambushMutex);
		g_pendingAmbush.clear();
	}
}

static auto RegisterTablesListener(const D2RL::PluginContext* ctx) -> bool {
	if (ctx->QueryService(&g_dataTables) != D2RL::ServiceQueryResult::Success || !D2RL::HasDataTableServiceField(g_dataTables, D2RL::DataTableServiceRequiredSize)) {
		ctx->LogError("pspell-ext: DataTableService missing.");
		return false;
	}
	const D2RL::LifecycleService* life = nullptr;
	if (ctx->QueryService(&life) != D2RL::ServiceQueryResult::Success || !D2RL::HasLifecycleServiceField(life, D2RL::LifecycleServiceRequiredSize)) {
		ctx->LogError("pspell-ext: LifecycleService missing.");
		return false;
	}
	const D2RL::Lifecycle::DataTablesLoadedListener listener {
		.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize,
		.callback   = OnDataTablesLoaded,
	};
	D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
	if (life->registerDataTablesLoadedListener(ctx, &listener, &handle) != D2RL::Lifecycle::Result::Success) {
		ctx->LogError("pspell-ext: failed to register the tables-loaded listener.");
		return false;
	}
	// Queued ambushes belong to one game: drop them when it ends.
	const D2RL::Lifecycle::GameplayEventListener left {
		.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
		.kind       = D2RL::Lifecycle::GameplayEventKind::GameLeft,
		.callback   = OnGameLeft,
	};
	D2RL::Lifecycle::ListenerHandle leftHandle = D2RL::Lifecycle::InvalidHandle;
	if (life->registerGameplayEventListener(ctx, &left, &leftHandle) != D2RL::Lifecycle::Result::Success) {
		DiagWarn(ctx, "pspell-ext: could not register the game-left listener; old ambushes are dropped when one is read in a new game.");
	}
	return true;
}

// ---------------------------------------------------------------------------
static auto StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* cmd, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (cmd == nullptr || cmd->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	size_t pending = 0;
	{
		const std::lock_guard<std::mutex> lock(g_ambushMutex);
		pending = g_pendingAmbush.size();
	}
	char msg[896] {};
	std::snprintf(msg, sizeof(msg),
	              "pspell-ext: enabled=%d active=%d diagnostics=%d target_pspell=%u position_pspell=%u search_radius=%u front_distance=%u skill_limit=%u casts=%u "
	              "town_refused=%u failed=%u slots16_19=%d hover_casts=%u net_channel=%d net_state=%u aim_msgs=%u portal_pspell=%u portals=%u portal_refused=%u "
	              "tome_pspell=%u tomes=%u tome_refused=%u summon_pspell=%u ambush_pspell=%u monster_limit=%u packs=%u summon_refused=%u ambush_queued=%u "
	              "ambush_pending=%zu ambush_sprung=%u ambush_refused=%u message_pspell=%u messages=%u message_refused=%u",
	              g_config.enabled, g_slotsActive, g_config.diagnostics, g_config.targetSlot, g_config.positionSlot, g_config.searchRadius, g_config.frontDistance,
	              g_skillRowLimit.load(std::memory_order_relaxed), g_castCount.load(std::memory_order_relaxed), g_townRefusals.load(std::memory_order_relaxed),
	              g_failCount.load(std::memory_order_relaxed), g_gameSlotsEmpty.load(std::memory_order_relaxed), g_hoverCasts.load(std::memory_order_relaxed),
	              g_channel != D2RL::Network::InvalidChannelHandle, g_netState.load(std::memory_order_relaxed), g_aimMsgsReceived.load(std::memory_order_relaxed),
	              g_config.portalSlot, g_portalsOpened.load(std::memory_order_relaxed), g_portalRefusals.load(std::memory_order_relaxed),
	              g_config.tomeSlot, g_tomesRead.load(std::memory_order_relaxed), g_tomeRefusals.load(std::memory_order_relaxed), g_config.summonSlot,
	              g_config.ambushSlot, g_monsterRowLimit.load(std::memory_order_relaxed), g_packsSpawned.load(std::memory_order_relaxed),
	              g_summonRefusals.load(std::memory_order_relaxed), g_ambushesQueued.load(std::memory_order_relaxed), pending,
	              g_ambushesSprung.load(std::memory_order_relaxed), g_ambushRefusals.load(std::memory_order_relaxed), g_config.messageSlot,
	              g_messagesShown.load(std::memory_order_relaxed), g_messageMissing.load(std::memory_order_relaxed));
	cmd->plugin->WriteConsoleMessage(msg);
	return D2RL::ConsoleCommandResult::Handled;
}

// ---------------------------------------------------------------------------
static constexpr D2RL::PluginInfo kInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "pspell-ext",
	.name        = "pSpell Extender",
	.version     = "0.1.0",
	.author      = "Kain",
	.description = "Adds item pSpells that cast the calc1 skill on the nearest enemy or at a position, like Diablo 1 skill scrolls.",
	// Gameplay rule run by the host; it puts native function pointers in a game table.
	// Shared: every client runs the hover hook (aim target for TCP/IP) and the
	// pSpell 20-30 usable checks; the host runs the item spells.
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

	(void)ctx->EnsureConfig();
	std::array<char, 8192> buf {};
	uint32_t required = 0;
	if (ctx->ReadConfig(buf.data(), static_cast<uint32_t>(buf.size()), &required)) {
		// Held until the whole toml is read: `diagnostics` may come after the line a warning is about.
		std::vector<std::string> warnings;
		ParseConfig(buf.data(), g_config, [&warnings](const char* msg) { warnings.emplace_back(msg); });
		DiagInfo(ctx, "pspell-ext loading.");
		for (const std::string& w : warnings) {
			DiagWarn(ctx, w.c_str());
		}
		// A toml written by an older version lacks the newer slot keys (the loader never
		// rewrites an existing file): name them so modders know what to add.
		std::string missing;
		for (const auto& [key, set] : { std::pair { "target_pspell", g_config.targetSlotSet }, std::pair { "position_pspell", g_config.positionSlotSet },
		                                std::pair { "portal_pspell", g_config.portalSlotSet }, std::pair { "tome_pspell", g_config.tomeSlotSet },
		                                std::pair { "summon_pspell", g_config.summonSlotSet }, std::pair { "ambush_pspell", g_config.ambushSlotSet },
		                                std::pair { "message_pspell", g_config.messageSlotSet } }) {
			if (!set) {
				missing += missing.empty() ? key : std::string(", ") + key;
			}
		}
		if (!missing.empty()) {
			const std::string msg = "pspell-ext: the toml has no " + missing + " key; using the value(s) shown below. Add the key(s) to move a pSpell.";
			DiagInfo(ctx, msg.c_str());
		}
	} else {
		DiagWarn(ctx, "pspell-ext: could not read config; using defaults.");
		NormalizeConfig(g_config, [ctx](const char* msg) { DiagWarn(ctx, msg); });
	}
	{
		char msg[400] {};
		std::snprintf(msg, sizeof(msg),
		              "pspell-ext: enabled=%d target_pspell=%u position_pspell=%u portal_pspell=%u tome_pspell=%u summon_pspell=%u ambush_pspell=%u message_pspell=%u "
		              "message_format=%s message_stat=%u search_radius=%u front_distance=%u summon_distance=%u line_of_sight=%d use_hover_target=%d diagnostics=%d",
		              g_config.enabled, g_config.targetSlot, g_config.positionSlot, g_config.portalSlot, g_config.tomeSlot, g_config.summonSlot, g_config.ambushSlot,
		              g_config.messageSlot, g_config.messageFormat.c_str(), g_config.messageStat, g_config.searchRadius, g_config.frontDistance, g_config.summonDistance,
		              g_config.lineOfSight, g_config.useHoverTarget, g_config.diagnostics);
		DiagInfo(ctx, msg);
	}

	(void)ctx->RegisterConsoleCommand("pspell-ext", StatusCommand, "Show pspell-ext status.");

	// The hooks go in even when disabled: they then refuse every pSpell 16-30 item,
	// which the game would otherwise send to a table entry that is not code.
	RegisterAimChannel(ctx);
	if (ctx->QueryService(&g_localization) != D2RL::ServiceQueryResult::Success
	    || !D2RL::HasLocalizationServiceField(g_localization, D2RL::LocalizationServiceRequiredSize) || g_localization->getStringByKey == nullptr) {
		g_localization = nullptr;
		DiagWarn(ctx, "pspell-ext: LocalizationService missing; message items will be refused.");
	}
	if (!RegisterTablesListener(ctx) || !InstallSlots(ctx)) {
		return false;
	}
	if (!g_config.enabled) {
		DiagInfo(ctx, "pspell-ext: disabled in config; items with pSpell 16-30 are refused.");
	}

	DiagInfo(ctx, "pspell-ext loaded.");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	// D2RLoader restores the table entries when the plugin unloads.
	g_slotsActive = false;
}
