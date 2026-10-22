# Handoff: gamble-tweaks plugin (D2RLoader)

For continuing this in Claude Code. The skeleton compiles and loads today; the
reverse-engineering to activate features A/B/C is the remaining work.

## Mission
A D2RLoader SDK plugin with three gamble changes, each independently toggled in
`config/gamble-tweaks.toml`:
- **A** — stop hard-coding ring + amulet into the gamble window; show only what
  `gamble.txt` lists.
- **B** — per-base **constant** gamble price from the plugin's `gamble` table
  (`price` column; 0 = use the game formula).
- **C** — gamble cost reduced by a **new ItemStatCost.txt stat** (row id from config),
  not the normal "reduce vendor prices" stat.

## Boundary — do not cross
- Offsets come from **static** Ghidra analysis of the **retail D2R.exe** build the
  loader runs (per `Claude.md`, that is where hooks go). Logic/naming reference:
  the public **D2MOO** project (ThePhrozenKeep/D2MOO, 1.10f) — read it for function
  shapes only.
- **Do not** unpack, decrypt, or dump the game image embedded in `D2RLoader.exe`
  (it is a packed resource; the gamble code is not statically present there). This
  plugin does not need it. If retail bytes don't match the loader's mapped game,
  get the pinned build's offsets from the loader author/community — don't unpack.
- Never write an address/RVA/byte sequence you didn't read with a Ghidra tool in
  the session. Rename only with evidence + plate comments. Verify every RVA at
  runtime with `CheckExpectedBytes` before any write.

## Environment (from Claude.md)
- Ghidra 12.1.4 at `M:\Tools\RE\ghidra_12.1.4_PUBLIC`, GhidraMCP 7.0.0 on port 8089.
  Pass `program=` on every Ghidra call.
- D2R install: `G:\Diablo II Resurrected` (offline / D2RLoader only).
- SDK: `third_party/PluginSDK` (read headers before using a service). Plugin ABI 4.
- Build: Visual Studio Community (MSVC), CMake + Ninja, x64. Not MSYS2/GCC.

## Repo layout
```
gamble-tweaks/
  CMakeLists.txt              # add_subdirectory(third_party/PluginSDK); embeds config
  config/gamble-tweaks.toml   # embedded default; per-feature toggles + [d2rl] match
  data/gamble.txt             # editable copy of the default plugin table (TSV, LF)
  src/gamble_tweaks.cpp       # plugin; Tier-1 live, A/B/C stubbed behind zero RVAs
  src/gamble_tweaks.rc        # ABI manifest resource
  notes/gamble.md             # RE worksheet — fill from Ghidra
  third_party/PluginSDK/      # <- place the SDK here (not included)
```

## What works now (no RE needed)
- Loads as Shared + NativeHooks, ABI 4.
- Reads/parses `config/gamble-tweaks.toml` (`[gamble-tweaks]` keys).
- Registers the plugin-owned `gamble` table (`code` Ascii(8), `gamble` Dword,
  `price` Dword) and ships its default TXT from memory at
  `data/global/excel/d2rloader/gamble-tweaks/gamble.txt` (a mod MPQ can override it).
- Rebuilds an in-memory snapshot (`g_entries`, FourCC + gamble + price) on every
  `DataTablesLoaded`. `FindEntry(code)` is ready for the hooks.
- `gamble-tweaks` console command prints status.
- `InstallHooksAndPatches` is a no-op while all `Rva::*` are 0 — plugin loads clean.

## Remaining work (activate A/B/C)
Everything is marked `TODO(RE)` in `src/gamble_tweaks.cpp`. Order:

1. **Find GAMBLE_PRICE and GAMBLE_BUILD** in D2R.exe (see `notes/gamble.md`). Use
   the `gamble.txt` / gambling UI strings and D2MOO's store/gamble + item-pricing
   shapes. Record RVAs, whole-instruction expected bytes (≥14 for the inline hook),
   RIP-relative operands, and signatures in `notes/gamble.md`.
2. **Feature A**: set `Rva::GambleWindowInsert` + `GambleWindowInsertLen` and fill
   `kGambleWindowExpected[]` with the real bytes at the ring/amulet insert site.
   The staged byte patch NOPs it. Acceptance: with `gamble_window_strict_gambletxt`
   on and `rin`/`amu` removed from `gamble.txt`, the window shows neither; with them
   present, it shows them.
3. **Feature B**: set `Rva::GamblePrice`, fill `kGamblePriceExpected[]`, correct the
   `GamblePriceFn` signature, and after a successful commit wire
   `g_originalGamblePrice = (GamblePriceFn)getInlineHookOriginal(priceOp)`. In
   `HookGamblePrice`, resolve the base code from the item arg (needs the item/record
   offset) and return `FindEntry(code)->price` when non-zero. Acceptance: a base with
   `price=25000` always costs 25000 to gamble; `price=0` matches vanilla.
4. **Feature C**: in the same hook, when `gamble_cost_reduction_stat_id != 0`, read
   the unit's total of that stat via the game's stat-getter (resolve `g_getUnitStat`,
   or read the stat list via its struct) and apply percent/flat reduction; ensure the
   vendor-reduction stat does not also apply when `ignore_vendor_reduction_for_gamble`
   is set. Acceptance: the vendor-reduction stat no longer changes gamble cost; the
   new stat does, by the configured amount.

## Build & test loop
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
copy build\d2rl-gamble-tweaks.dll  "G:\Diablo II Resurrected\d2rloader\plugins\"
```
In game: run `gamble-tweaks` in the console to confirm config + row count, then open
the gambling screen. Test on a disposable character (pricing/economy adjacent).
Keep `CheckExpectedBytes` guards; if commit fails it auto-rolls-back.

## Open decisions
1. Per-base fields: keep them in the plugin `gamble` table (current), or mirror into
   a repurposed real column in weapons/armor/misc.txt (needs the compiled row offset)?
2. Feature C: percent vs flat reduction, and a stacking cap.
3. Confirm by observation whether newly-gambleable bases already roll magic+ (decides
   whether a separate quality hook is also wanted — see `claude/gamble-all-items-plan.md`).

## Status (2026-10-21, Claude Code)

RE done (see notes/gamble.md here and the workspace notes/gamble.md). Builds with
`build.cmd` (MSVC 19.51 x64 + Ninja from M:\Tools\Visual Studio) with no warnings;
exports D2RLoaderGetPluginInfo / LoadPlugin / UnloadPlugin. Not yet run in game.

Decisions: analysis per Claude.md (dump, not retail D2R.exe); feature B keyed by
the normal-tier code; feature C percent, capped at 50 (config
`gamble_cost_reduction_cap_percent`).

Implemented:
- A: byte patch RVA 0x541a25 `83 FD 02` -> `83 FD 00` (no forced ring/amulet slots).
- Vendor discount off for gambling (`ignore_vendor_reduction_for_gamble`): byte
  patch RVA 0x36f268 `0F 84 B2 00 00 00` -> `E9 B3 00 00 00 90`.
- B + C: inline hook on ITEMS_GetTransactionCost (RVA 0x36f0c0, 15 bytes); type 2
  only; constant price by normal-tier code, then the stat reduction; result >= 1.
- B (0.3.0) `use_item_gamble_cost`: every base priced by the "gamble cost" column of
  its normal-tier row, like rings/amulets (DATATBLS_GetItemIdFromItemCode RVA
  0x313ef0 -> ItemsTxt +0xDC). Plugin `price` > 0 wins; gamble cost 0 -> game price.
- D (0.4.0) `reduced_value_stat_id`: inline hook on D2GAME_ITEMS_Identify (RVA
  0x46e8c0) tags an item bought from a gambler (return address 0x53e6b2) with the
  stat in its saved magic list (magic 25 / rare 33 / set 33 / unique 50); the
  price hook takes that percent off the item's sell price (type 1) after the
  game's calculation. ItemStatCost row: Save Bits >= 7, Saved = 0, id < 512.
- Each feature checks its expected bytes first and is skipped (logged) on mismatch.

Not implemented: "refill ring/amulet like the others". Rings/amulets have no
norm/uber/ultra codes, so removing the game's ring/amulet refill case would make
the same-family refill come back empty and assert. The game already refills a
bought ring with a new ring (its whole family). A refill with any gamble.txt base
would need a hook - not done.

Test plan (disposable character):
1. Copy build\d2rl-gamble-tweaks.dll to `G:\Diablo II Resurrected\d2rloader\plugins\`.
2. Start, run `gamble-tweaks` in the console: expect rows=2, window=1,
   noVendor=1, priceHook=1. Check the plugin log for "unexpected bytes".
3. A: remove rin/amu from the game's gamble.txt (mod data) -> no ring/amulet in
   the window; with them present they appear only at random.
4. B: add `cap<TAB>1<TAB>25000` to the plugin gamble table -> Cap/War Hat/Shako
   all show and cost 25000; price 0 -> vanilla.
4b. use_item_gamble_cost: set `gamble cost` 12345 on the cap row in armor.txt (plugin
   price 0) -> Cap/War Hat/Shako all cost 12345; console shows itemCost=1.
5. C: add a stat row to ItemStatCost.txt, give it to an item, set
   gamble_cost_reduction_stat_id -> price drops by the value in percent, never
   more than 50%; item_reducedprices no longer changes gamble prices.
6. D: add a "reduced value" row to ItemStatCost.txt (Save Bits 7, Saved 0, a
   descfunc/descstr so it shows), set reduced_value_stat_id. Note an item's sell
   price, gamble a magic item, compare: the tooltip shows the stat (25), the sell
   price is 25% under what vanilla gives. Rare/set 33, unique 50. Save, quit,
   reload: the stat is still on the item. The character screen and save are
   unchanged. A non-gambled item sells as before. Console shows active=1.
