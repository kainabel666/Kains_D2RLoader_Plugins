# gamble-tweaks — setup checklist


## 1. Feature B — fixed gamble price per base

Prices are looked up by the **normal-tier** code: the `cap` row also prices War
Hat (`xap`) and Shako (`uap`). The first match wins:

1. **Plugin table price.** Config: `use_constant_gamble_price = true`.
   - Edit the plugin's table, `data/global/excel/d2rloader/gamble-tweaks/gamble.txt`.
     Put it in your mod to replace the built-in default (`data\gamble.txt` in this
     folder is an editable copy).
   - The file is tab-separated with columns `code`, `gamble`, `price`. Example row:
     `cap	1	25000`
   - `price` 0 means "no fixed price here; fall through to the next option".
2. **The base's own `gamble cost`.** Config: `use_item_gamble_cost = true`.
   - Every base is priced by the `gamble cost` column of its normal-tier row in
     weapons.txt / armor.txt / misc.txt, the same way the game prices rings and
     amulets.
   - Set or adjust that column in your mod. The dump showed `cap` already has 3016.
   - A base whose `gamble cost` is 0 or blank keeps the game's formula price.
3. **The game's formula price**, if neither of the above applies.

## 2. Feature C — new stat lowers gamble prices

1. Add a new row to your mod's `ItemStatCost.txt` (see section 7 for how).
2. Set `gamble_cost_reduction_stat_id` to that row's id.
3. Give players a way to get the stat: add a `Properties.txt` row whose `stat1` is
   the new stat, then use that property on affixes, uniques, sets, runewords or charms.
4. Options:
   - `gamble_cost_reduction_is_percent = true`: the stat is a percentage
     (`false` = flat gold).
   - `gamble_cost_reduction_cap_percent = 50`: the most it can take off.
5. The value is the player's total from all equipped/active sources.
6. `ignore_vendor_reduction_for_gamble = true` (default): the normal reduced-prices
   stat (`item_reducedprices`) no longer lowers gamble prices. Normal buying and
   selling still use it.

## 3. Feature D — "reduced value" on gambled items

Every item bought from a gambler gets this stat. Its sell price to vendors drops
by that percent, after the game has finished its own calculation. Only the item's
own value counts; it is not added to the character's save.

1. Add a new `ItemStatCost.txt` row for it (see section 7). It must have:
   - `Save Bits` **7 or more**, so the value (up to 100) is saved on the item;
   - `Save Add` 0;
   - `Saved` **0**, so it is never written to the character;
   - a row id **below 512**, because items save stat ids in 9 bits.
   - To see it on the item, also fill the description columns (`descpriority`,
     `descfunc`, `descval`, `descstrpos`, `descstrneg`). Add the matching strings
     to your mod's string files.
2. Set `reduced_value_stat_id` to that row's id.
3. Change the per-quality values if you want different numbers:
   `reduced_value_magic` 25, `reduced_value_rare` 33, `reduced_value_set` 33,
   `reduced_value_unique` 50 (each 0-100; 0 means that quality gets no stat).
4. Notes:
   - Items are tagged when bought, not while they sit in the gamble window.
   - Items gambled before the feature was on, or with a different
     `reduced_value_stat_id`, don't get the stat afterwards.
   - A gambled item socketed into another item (e.g. a jewel) would also lower
     the host item's sell price, because socketed stats count toward the item.

## 4. Adding an ItemStatCost.txt row

- Add rows at the **end** of the file so existing stat ids don't move. The new
  row's id is its position (rows count from 0); it is the number to put in the config.
- The easiest start is to copy an existing row with a percent description, such as
  `item_reducedprices`. Then rename the `Stat` column and change the strings, and set
  the save columns as listed above. Check `Saved` in the copy: feature D needs it at 0.
- Features C and D each need their **own** row.

## 5. Test plan (throwaway character)

1. A: with `rin`/`amu` removed from gamble.txt, open a gambler several times. No
   ring or amulet appears.
2. B, table price: plugin row `cap	1	25000`. Cap, War Hat and Shako all cost 25000.
   Set `price` 0 and they go back to the `gamble cost` / formula price.
3. B, gamble cost: set `gamble cost` 12345 on the `cap` row of armor.txt (plugin
   price 0). Cap, War Hat and Shako all cost 12345.
4. C: wear an item with the new stat at 20. Gamble prices drop 20%. At 80 they
   drop only 50%. `item_reducedprices` no longer changes gamble prices.
5. D:
   1. Note what a magic item normally sells for, then gamble one.
   2. Its tooltip shows the stat (25), and it sells for 25% less.
   3. Rare and set items show 33; unique items show 50.
   4. Save, quit and reload: the stat is still on the item.
   5. Non-gambled items sell as before.
