# Gamble RE notes (gamble-tweaks)

Filled from Ghidra on 2026-10-21. Per the workspace Claude.md the game is
D2RLoader.exe and code is analysed in `/d2rloader_full.dmp` (D2R.exe is never
used). Full evidence, plate comments and the D2MOO mapping are in the workspace
`notes/gamble.md` and `notes/d2moo-map.md`. Every RVA below is checked at runtime
with the expected bytes before any write.

## Image
- D2RLoader.exe image base in the dump: 0x140000000 (RVA = address - 0x140000000)
- Build: the one captured in /d2rloader_full.dmp. If the bytes differ on another
  build, the plugin logs "unexpected bytes" and skips that feature.

## GAMBLE_BUILD (fills the gamble window) - feature A
- D2GAME_STORES_FillGamble, RVA 0x541880 (D2MOO SUnitNpc.cpp D2GAME_STORES_FillGamble_6FCCA9F0)
- Evidence: DATATBLS_GetGambleDataTables, cached 'rin '/'amu ' ids, ilvl clvl+-5 clamp 5..99, 14 items
- Ring/amulet forced-slot site: RVA 0x541a25 `83 FD 02` (CMP EBP,2; then JGE 0x541a39)
- Plan: `83 FD 02` -> `83 FD 00` (no slot is forced; gamble.txt alone fills the window)
- Do not touch 0x541a2a `8B 1D 14 DC 56 02` / 0x541a32 `0F 44 1D 07 DC 56 02` (RIP-relative loads of the cached amu/rin ids)
- x64 signature: `void __fastcall(void* pGame, D2UnitStrc* pNpc, D2UnitStrc* pPlayer, void* pNpcRecord)`

## GAMBLE_PRICE - features B, C
- ITEMS_GetTransactionCost, RVA 0x36f0c0 (D2MOO Items.cpp ITEMS_GetTransactionCost / ITEMS_CalculateTransactionCost)
- First 15 bytes: `4C 89 4C 24 20 | 44 89 44 24 18 | 48 89 54 24 10` (three MOVs, no RIP-relative)
- x64 signature: `int __fastcall(D2UnitStrc* pPlayer, D2UnitStrc* pItem, int nDifficulty, void* pQuestFlags, int nVendorId, int nTransactionType)`; gamble = type 2
- Called by the client (tooltip price text, buy packet cost) and the server (cost check); the server rejects a mismatch, so the hook must be deterministic
- Base code from the item arg: table mode = byte unit+0x1BD, class id = dword unit+0x04, record = DATATBLS_GetItemsTxtRecord (RVA 0x314110, `D2ItemsTxt* __fastcall(uchar nMode, int nItemId)`), code = dwNormCode (+0x84) or dwCode (+0x80) when 0 - the same record the game prices from
- Vanilla gamble price: old item format -> flat `gamble cost` of the normal-tier row; otherwise a formula (rings/amulets: `gamble cost`), then minus item_reducedprices (stat 87, capped 99) percent
- Ring/amulet path (FUN_140376b10): normal-tier record (dwNormCode or dwCode) -> DATATBLS_GetItemIdFromItemCode (RVA 0x313ef0, `40 53 48 83 EC 20 8B DA`, `int __fastcall(uchar nMode, uint dwCode)`) -> DATATBLS_GetItemsTxtRecord -> dwGambleCost +0xDC; the caller then subtracts item_reducedprices. Old-format path: same lookup at 0x36f36b, `MOV EAX,[RAX+0xDC]` at 0x36f384, JMP 0x36f323 (returns with no reduction)
- `use_item_gamble_cost`: the hook prices every base like rings/amulets (this lookup; reduced-prices only when not ignored). Plugin `price` > 0 wins; gamble cost 0 -> game result
- Vendor reduction on gambling: JZ at RVA 0x36f268 `0F 84 B2 00 00 00` skips it when the stat is 0; only reached from the gamble branch. Plan: `E9 B3 00 00 00 90` (always skip)

## GET_UNIT_STAT - feature C
- ReadWideUnitStat, RVA 0x2f5020 (`FF 25 F2 51 B3 03`, thunk into D2RCore 0xc0de5831de0)
- Signature: `int __fastcall(D2UnitStrc* pUnit, uint nStatId, uint nLayer)`
- Evidence: same calls as D2MOO STATLIST_UnitGetStatValue inside the cost function: (item, 70 quantity, 0) and (player, 87 item_reducedprices, 0) capped 99

## REDUCED_VALUE - feature D
- Tag point: D2GAME_ITEMS_Identify, RVA 0x46e8c0, inline hook; first 15 bytes `48 89 6C 24 20 | 41 54 | 41 56 | 41 57 | 48 83 EC 50` (no RIP-relative)
- x64 signature: `void __fastcall(void* pGame, D2UnitStrc* pPlayer, D2UnitStrc* pItem, char bFlag)`
- Gamble-only caller: maybe_D2GAME_NPC_BuyItem, CALL at RVA 0x53e6ad `E8 0E 02 F3 FF` behind `CMP byte [RBP+0xE0],0` (bGamble); the hook tags only when it returns to RVA 0x53e6b2. Identify then sends the item to all clients, so the client gets the stat
- Stat write = the item deserializer's own sequence (ITEMS_DecodeItemBitstreamCompact 0x3780d0):
  - STATLIST_GetStatListFromUnitStateOrFlag RVA 0x2f5b80 `void* (pUnit, 0, 0x40)`
  - if NULL: STATLIST_AllocStatList RVA 0x2f7300 `void* (0x40, NULL, 4, unitId)`, maybe_UNITS_GetUnitId RVA 0x34a330 `uint (pUnit, file, line)`, AttachWideStatList RVA 0x2f2cb0 `(pUnit, pList, 1)`
  - SetWideListStat RVA 0x2f7c00 `(uchar nMode, pList, statId, value, layer)`
- The 0x40 list is what ITEMS_SerializeItemComplete saves; the player save holds only the player's base stats
- Quality: dword at pItemData (unit+0x10) + 0: 4 magic, 5 set, 6 rare, 7 unique
- Sell: ITEMS_GetTransactionCost type 1; hook result = original - original * min(item stat, 100) / 100, floor 1

## Item/record struct
- D2UnitStrc: +0x04 class id, +0x1BD data-table mode (byte)
- D2ItemsTxt (0x1C0 bytes): +0x80 dwCode, +0x84 dwNormCode, +0x88 dwUberCode, +0x8C dwUltraCode, +0xDC dwGambleCost, +0x10D level
- Rings/amulets have norm/uber/ultra codes = 0 (checked in the dump), so their normal-tier key is their own code ('rin', 'amu')
