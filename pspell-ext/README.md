# pSpell Extender (pspell-ext)

Gives items new right-click effects through their `pSpell` column, in the style of Diablo 1
scrolls and books. A right-click (inventory or belt) can:

- **cast a skill** at an enemy or at the player,
- **open a red portal** to any level,
- **teach a skill point**,
- **summon monster packs** like an Act 5 Evil Urn, now or at the next superunique,
- **show a message** in the NPC speech box.

Message scrolls (`message_pspell`): Originally designed by Joel Falcou for 1.10f.
Skill tomes (`tome_pspell`): inspired by Havvoric's SpellBook for 1.10f.

## Install

1. Copy `d2rl-pspell-ext.dll` and the folder `d2rl-pspell-ext.mpq` into your mod's
   `d2rloader\plugins\` folder. The folder holds the plugin's texts (tooltips, item names,
   messages); keep the two together.
2. Start the game once. The plugin writes `d2rloader\config\pspell-ext.toml`.
3. Change the toml if you need to (see **Settings**), then restart the game. Settings are read
   at start.
4. Type `pspell-ext` in the in-game console. `active=1` means it is working. It also shows
   which pSpell number each feature uses and how often each one was used or refused.

In TCP/IP games **every player** needs the plugin, same version. The host runs the effects;
each client sends the monster its player is aiming at, and without the plugin a client can't
right-click items with pSpell 20-30.

## How it works

Each feature answers to one pSpell number. Put that number in the `pSpell` column of an item
row (misc.txt, weapons.txt or armor.txt) and fill `calc1`-`calc3` as described below. Every
feature except the message scroll uses the item up, like a potion.

| Feature | pSpell (default) | Town | Item |
|---|---|---|---|
| Cast at target | 16 | no | used up |
| Cast at player | 17 | no | used up |
| Red portal | 18 | yes | used up |
| Skill tome | 19 | yes | used up |
| Summon | 20 | no | used up |
| Ambush | 21 | yes | used up |
| Message | 22 | yes | kept |

When an item can't be used, nothing is taken and the character says a voice line instead
(the game's "can't use" line, or its town line in town). Problems in your data, such as a
skill or monster id that doesn't exist, also write one warning to the log when `diagnostics`
is on (see **When something goes wrong**).

### Cast at target (16) and cast at player (17)

| Column | Meaning |
|---|---|
| `calc1` | Skill id (skills.txt `*Id`) |
| `calc2` | Skill level. Blank or below 1 = level 1, at most 255. |

**Cast at target** picks, in this order:
1. the monster you are aiming at: under the mouse, or the controller target (the one whose
   health bar shows at the top of the screen);
2. the nearest enemy within `search_radius`;
3. a point `front_distance` subtiles in front of the player, in the direction they face.

"Enemy" means a living, hostile monster that can be attacked. Hirelings, summons and other
allies are never picked. With `line_of_sight` on, an enemy behind a wall, door or missile
barrier is skipped.

**Cast at player** casts where the player stands: good for Nova, Frost Nova, warcries and buffs.

The cast works like a chance-to-cast skill on an item, so the skill needs an `ItemEffect` in
skills.txt, and its `ItemTarget` applies. Not usable in town or during an action that can't be
interrupted.

### Red portal (18)

| Column | Meaning |
|---|---|
| `calc1` | Level id the item must be used in (levels.txt). Blank or 0 = anywhere. |
| `calc2` | Destination level id (levels.txt). |

Opens a pair of red portals, like the Cow Level recipe. A destination in another act works when
the player has that act open on the current difficulty (its town waypoint is active, as for
waypoint travel; there is no Act 5 in a classic game). In that case the far portal appears where
the first player arrives and leads back. Every player who walks in is checked the same way.

### Skill tome (19)

| Column | Meaning |
|---|---|
| `calc1` | Skill id (skills.txt `*Id`) |

Each read adds one hard point to the skill, as if the player spent a skill point, but without
using one. The point shows in the skill tree and is saved with the character.

The normal skill-point rules apply:
- only the player's own class skills (the character file can't hold other skills);
- `reqlevel`, plus one level per point already in the skill;
- `reqskill1`-`3` and the str/dex/int/vit requirements;
- `maxlvl` (blank = 20).

A tome point is a real point, so it uses up the skill's one-point-per-level allowance: after
reading a tome you can't also put a normal point into that skill until the next level, and the
other way round.

### Summon (20) and ambush (21)

Both create hostile monster packs like the ones an Act 5 Evil Urn releases: one champion or
unique monster and its minions. They drop loot and give experience like any other pack.

| Column | Meaning |
|---|---|
| `calc1` | Monster id (monstats.txt row). Blank or 0 = the urn's choice: the first monster type of the level that can walk. |
| `calc2` | 1 = always unique. Blank or 0 = the urn's roll (champion or unique). |
| `calc3` | Number of packs. Blank or below 1 = 1, at most 10. |

**Summon** places the packs `summon_distance` subtiles in front of the player. If that spot is
in a wall or not loaded yet, they come closer, 5 subtiles at a time. Not usable in town.

**Ambush** sets a trap instead, like the Diablo Clone event: the packs appear around the next
superunique that spawns in this game. A superunique spawns when its area is first loaded, so ones
already met don't count. The superunique itself still spawns as normal. Up to 8 ambushes can wait
at once; leaving the game drops them, and the items are not given back.

### Message (22)

| Column | Meaning |
|---|---|
| `calc1` | Message number. Blank or 0 = read it from the item's `message_stat` instead. |

The text shows in the NPC speech box at the top of the screen and scrolls like NPC speech, in
the player's language. Right-click again to close it. In TCP/IP only the reader sees it. The
item is kept, so a scroll can be read again and again.

The message number picks a string: `message_format` with the number filled in. With the default
`scrollMsg%04d`, message 13 is the string `scrollMsg0013`. A `\n` in the string starts a new line.
If the string doesn't exist, nothing is shown (with `diagnostics` on, the log names the missing
key).

**One item, many messages.** Leave `calc1` blank and set `message_stat` to a stat's row number.
The message number then comes from that stat on the item, so a cube recipe or a unique or set
item can give each copy its own message (see **Your data**).

## Settings

```toml
[pspell-ext]
enabled = true
diagnostics = false

target_pspell = 16
position_pspell = 17
portal_pspell = 18
tome_pspell = 19
summon_pspell = 20
ambush_pspell = 21
message_pspell = 22

message_format = "scrollMsg%04d"
message_stat = 0

use_hover_target = true
line_of_sight = true
search_radius = 40
front_distance = 5
summon_distance = 20
```

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `true` | Master switch. `false` = every item with pSpell 16-30 is refused. |
| `diagnostics` | `false` | `true` = write details and warnings to the log (see **When something goes wrong**). |
| `target_pspell` | 16 | pSpell number for cast at target. |
| `position_pspell` | 17 | pSpell number for cast at player. |
| `portal_pspell` | 18 | pSpell number for the red portal. |
| `tome_pspell` | 19 | pSpell number for the skill tome. |
| `summon_pspell` | 20 | pSpell number for summon. |
| `ambush_pspell` | 21 | pSpell number for ambush. |
| `message_pspell` | 22 | pSpell number for message scrolls. |
| `message_format` | `scrollMsg%04d` | String key for a message number. Exactly one `%d` (or `%04d` etc.); letters, digits, `_`, `-` and `.` only. Anything else falls back to the default. |
| `message_stat` | 0 | itemstatcost.txt row that holds the message number when `calc1` is blank. 0 = off. |
| `use_hover_target` | `true` | Cast at target aims first at the monster under the cursor or controller target. `false` = always the nearest enemy. |
| `line_of_sight` | `true` | Skip enemies a missile can't reach. `false` = nearest enemy even behind a wall. |
| `search_radius` | 40 | How far cast at target looks for the nearest enemy, in subtiles (1-100). |
| `front_distance` | 5 | With no enemy, how far in front of the player the cast lands, in subtiles (1-50). |
| `summon_distance` | 20 | How far in front of the player summoned packs appear, in subtiles (1-100). |

**Moving pSpell numbers.** Every feature has its own key so you can move it out of the way of
another plugin. Allowed numbers are 16-30. An out-of-range number goes back to its default, and
if two features share a number the later one in the list moves to the next free number. Every
change is written to the log when `diagnostics` is on. A toml from an older version is not
updated for you; with `diagnostics` on, the log names any pSpell key it is missing, and you
can add it by hand.

## Your data

### Item rows

Fire Ball scroll in misc.txt (only the columns that matter):

| name | code | type | useable | pSpell | calc1 | calc2 | stackable | belt |
|---|---|---|---|---|---|---|---|---|
| Scroll of Fire Ball | sfb | sksc | 1 | 16 | 47 | 10 | 0 | 1 |

- **Item type:** give the item its own type in itemtypes.txt (4 lowercase letters, here `sksc`)
  that is not a book or scroll type and doesn't inherit from one. The game handles book and
  scroll types itself (books.txt and book charges) and the plugin never sees them.
- **Item code:** 3 letters. A new code also needs an entry in `data/hd/items/items.json` (its
  HD art) and a name string. Reusing an unused item row avoids both.
- **Item name:** put a string key that exists in `namestr` (for example
  `mymod:ScrollOfFireBall` from your mod's strings). A missing string shows as "An Evil Force".
- **stackable = 0.** A use removes the whole item.
- **belt = 1** only if the item type can go in the belt (`Beltable` in itemtypes.txt).
- **Don't use the Body Part type (`body`)** for an item vendors sell: its price depends on the
  monster it came from and buying it fails.

### Tooltip line

Set `spelldesc` 1, `spelldesccolor` 4, `spelldescstr` to one of the keys below and
`spelldescstr2` to the same key with `Controller` added. The texts ship with the plugin.

| Key | Text for |
|---|---|
| `pspell-ext:RightClickToCast` | cast at target, cast at player |
| `pspell-ext:RightClickToOpenPortal` | red portal |
| `pspell-ext:RightClickToRead` | skill tome, message |
| `pspell-ext:RightClickToSummon` | summon, ambush |

### Examples (misc.txt)

| Item | pSpell | calc1 | calc2 | calc3 | Result |
|---|---|---|---|---|---|
| Scroll of Frost Nova | 17 | 44 | 10 | | Frost Nova level 10 at the player |
| Red portal scroll | 18 | 0 | 39 | | portal to the Moo Moo Farm from anywhere |
| Tome of Fire Bolt | 19 | 36 | | | +1 Fire Bolt (Sorceress only) |
| Horn of Summoning | 20 | | | 3 | three packs of the level's monsters |
| Eye of Ambush | 21 | 19 | 1 | 1 | one unique Fallen pack at the next superunique |
| Scroll of the Stormy Night | 22 | 13 | | | shows `scrollMsg0013` |

Use a non-beltable item type for tomes, horns and message scrolls if you don't want them in
the belt.

### Message texts

Message strings go in the plugin's own string file:
`d2rl-pspell-ext.mpq\data\local\lng\strings\d2rloader\pspell-ext\strings.json`.
Add one entry per message, with the key built by `message_format` (`scrollMsg0013`) and a text
for each language.

### Stat-driven messages

To let one base item carry different messages (like the 1.10f "Scroll of Clues"):

1. **itemstatcost.txt:** add a stat, for example `item_scroll_message`, with `Save Bits` 8 and
   no description (so nothing shows in the tooltip). Note its row number.
2. **properties.txt:** add a property, for example `scroll-index`, with `func1` 1 and
   `stat1 item_scroll_message`.
3. **misc.txt:** the item gets `pSpell 22`, `calc1` blank and **`compactsave` 0**. A
   compact-saved item loses its stats when the game saves, and the message with them.
4. **Toml:** `message_stat` = the stat's row number.
5. **Give the stat:** for example a cubemain.txt recipe whose output is the item with
   `mod 1 scroll-index`, `mod 1 min 1`, `mod 1 max 4`. Each cubed scroll then shows one of
   `scrollMsg0001`-`scrollMsg0004`, and keeps it.

## When something goes wrong

By default the plugin keeps the log quiet: only errors that stop it from working are written.
To find out why an item doesn't behave as expected:

1. Set `diagnostics = true` in the toml and restart the game.
2. Do the thing that went wrong again.
3. Open the D2RLoader log. Lines starting with `pspell-ext:` show the settings in use, any
   setting that was corrected, and a warning for each data problem it hit (a skill, level or
   monster id that doesn't exist, a skill without `ItemEffect`, a missing message string).
4. Set `diagnostics = false` again when done.

The `pspell-ext` console command works either way and shows how often each feature was used
or refused.

## Notes and limits

- pSpell 16-30 can be used; 31 and above stay unusable, as in the base game.
- **Don't ship items with pSpell 16-30 without this plugin.** The base game has no code for
  16-19 (using such an item is unsafe) and treats 20-30 as unusable. With the plugin, a 16-30
  item that isn't one of the configured numbers, or any of them while `enabled = false`, is
  simply refused.
- Ambushes live only in the current game; leaving drops them.

## Credits

- Message scrolls (`message_pspell`): Originally designed by Joel Falcou for 1.10f (ScrollItem for
  D2Mod). He credited AFJ for the method of editing pSpells.
- Skill tomes (`tome_pspell`): inspired by Havvoric's SpellBook for 1.10f.
