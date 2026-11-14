# Arena

**Originally designed by Blizzard North for Diablo II 1.10** (cut content, restored).

Arena brings the two cut arenas of Diablo II 1.10 back as a PvP playground for multiplayer games:

- Two arenas, Arena Tomb 1 and Arena Tomb 2, reached by trapdoors beside the palace in Lut Gholein.
- Nobody really dies in an arena: you get up again after a few seconds and lose nothing.
- 1 v 1 duels, Battle Royale for up to 8 players, and team duels from 1 v 1 up to 4 v 4.
- Duel rules you set together, and wagers on gold and items.
- A scoreboard at the top of the screen.

## Install

Arena is a mod for D2RLoader. It comes as a mod folder (`arena`) with the plugin, its settings and its data.

1. Copy the `arena` folder into `Diablo II Resurrected\mods\`.
2. Start D2RLoader with the arena mod (pick it in the launcher, or start with `-mod arena`).
3. The first start writes the settings file `mods\arena\d2rloader\config\arena.toml`. Restart the game after you change it.
4. Type `arena` in the in-game console to check that the plugin is running.

Every player needs the same mod. The arenas only exist in multiplayer (TCP/IP) games: in single player there are no
arena trapdoors or obelisks.

Characters of this mod are saved in their own folder (`arena`), so your other characters don't show up there.

**Adding arena to your own mod?** See **Your data** below: copy the ready tables from `data-rows\Replace` if your mod
uses the vanilla tables, or append the rows from `data-rows\Add` to your own tables and set the new ids in
`arena.toml`.

## How it works

### Getting in
Two trapdoors beside the palace in Lut Gholein lead to the arenas. Inside, the red gates send you to another gate of
the same arena, and paired trapdoors link two spots once both are open. There is no way out of an arena except
finishing the fight (or leaving the game). Town portals don't work in the arenas.

### Fighting
- Everyone in an arena is hostile to everyone else, except teammates.
- When you go down, you get up again a few seconds later with full life and mana, away from your enemies. You keep
  everything: no corpse, no lost gold or experience.
- While you're down, your pets and your mercenary stand still and can't be hit.
- An Iron Golem or bound demon that "dies" waits for you and comes back at your next respawn.
- Dead skeletons lie around for Raise Skeleton and Raise Skeletal Mage. Other corpse skills don't work in the arenas.
- Potions are limited per arena visit and have a short cooldown.
- Your items are locked during a duel. Potions and weapon swap still work.

### Duels
- **Walk-in duel:** a second player who comes into an occupied arena starts a duel. A third player turns it into a
  Battle Royale.
- **Menu duel:** click another player in town and choose **Duel**. If they accept, a **Duel Rules** window opens for
  both of you. The challenger sets the rules, and the duel starts when both click the check mark.
- **Team duels:** click an obelisk near the Lut Gholein start and join Team One or Team Two. When everyone is ready,
  all players go to a free arena together. Teammates show green on the automap.
- After a duel everyone is healed and sent back to town.

### Duel rules
You can set the duel type (Kills, Points or Timed), the score to win, points per kill and per death, a time limit,
potion limits and cooldowns, a Teleport cooldown, whether opponents show on the map, and a kill bonus that refills
part of your missing life and mana on each kill. A tied Timed duel goes to sudden death: the next kill wins.

The **Guide** tab on each window explains every rule.

### Wagers
Set **Stakes** to items + gold in the Duel Rules window. The normal trade window opens next: both of you put in your
stakes and accept. Nothing changes hands yet. When the duel ends, the winner gets both stakes.

The stake trade is cancelled if either player cancels it, walks away, or doesn't finish it within 2 minutes.

### Scoreboard
A medallion at the top of the screen shows the score, with a nameplate for each player. Team One is on the left, Team
Two on the right. Hide or show it with the console command `arena scoreboard`, or set a key in Options > Controls
(category Arena).

## Console commands

| Command | What it does |
|---|---|
| `arena` | Shows that the plugin is running. |
| `arena scores` | Lists the scores of the current fights. |
| `arena scoreboard` | Hides or shows the scoreboard. |
| `arena duel <name> [gold]` | Challenges a player, optionally for that much gold each. |
| `arena accept` / `arena decline` / `arena cancel` | Answers or takes back a challenge. |
| `arena where` | Prints your position, for placing spots in the settings. |
| `arena lobby here [1-4]` | Host only: moves an obelisk next to you for a try (keep it with `[arena.lobby] spot`). |

## Settings (`arena.toml`)

The defaults work out of the box. The main keys:

```toml
[arena]
respawn_seconds = 5.0
scoreboard = true
kill_bonus = false
kill_bonus_life = 50
kill_bonus_mana = 75
walk_in_kill_bonus = true

[arena.potions]
healing = 5
mana = 5
rejuvenation = 2
cooldown_seconds = 1.0

[arena.duel]
player_menu = true
walk_in = true
points = 9

[arena.wager]
max_gold = 0
```

| Key | Meaning |
|---|---|
| `enabled` | `false` = the plugin does nothing. |
| `pvp` | `false` = normal deaths and no automatic hostility in the arenas. |
| `respawn_seconds` | Time before you get up again. |
| `respawn_text` | Text of the respawn countdown. |
| `countdown_style` | `"hud"` = middle of the screen, `"window"` = side window. |
| `scoreboard` / `scoreboard_art` / `scoreboard_y` | Scoreboard on/off, picture or plain text line, distance from the top. |
| `last_player_return` / `return_seconds` / `return_title` / `return_text` | The last player left in an arena goes back to town after a countdown. |
| `alone_seconds` / `alone_text` | Time alone in an arena before you go back to town (0 = stay). |
| `kill_messages` / `hostility_messages` | Chat lines for kills and hostility. |
| `kill_bonus` / `kill_bonus_life` / `kill_bonus_mana` | Kill bonus in fights without duel rules, and the % of missing life / mana it refills. |
| `walk_in_kill_bonus` | Kill bonus in walk-in duels and Battle Royale. |
| `respawn_avoid_death` / `respawn_avoid_enemy` | Keep respawns this far from where you fell and from enemies. |
| `clean_start` | Clear buffs and curses at the start of a fight and at each respawn. |
| `sealed_message` / `sender_name` / `message_scope` / `message_color` / `refusal_sound` | Message and voice line when a paired trapdoor is still closed. |
| `trace_damage` / `diagnostics` | Extra log lines for troubleshooting (off by default). |
| `[arena.potions]` | Potions per arena visit per kind (0 = no limit), the cooldown between potions, and whether mercenaries can be given potions. |
| `[arena.score]` | Points for kills, deaths and suicides in open fights. |
| `[arena.duel]` | `player_menu` (Trade / Duel menu on players), `walk_in` (walk-in duels), `walk_in_extras` (extra players who can join), `points` (score to win), `join_score`, `join_protect_seconds`, `invite_seconds`, `countdown_seconds`, `end_seconds`, `arrive_seconds`, the loading screen waits, `map_sight_mask` and `busy_message`. |
| `[arena.wager]` | `enabled`, `max_gold` (0 = no limit), `stake_stat` and `winnings_stat` (the itemstatcost.txt ids of the two wager rows, see **Your data**). |
| `[arena.pets]` | Keep Iron Golem and bound demons (`all_golems` = all golems), and hold pets while their owner is down. |
| `[arena.bodies]` | Skeleton bodies: number, monster row, respawn time, message for other corpse skills. |
| `[arena.lobby]` | `spot` = up to 4 obelisk positions (x, y pairs from `arena where`), `walk_away` = distance that closes the lobby, `object` = objects.txt id of the obelisk row. |
| `[arena.entrance]` | The town level and its trapdoors: x, y, arena level and objects.txt id for each. |
| `[arena.level.<id>]` | Per arena level: `gates`, `trapdoors` (in pairs), `spawns`, `start_spawns` and `bodies` as x, y pairs. |

## Your data

The arena mod ships everything the plugin needs, so you can play it as it is. This section is for adding arena to
another mod.

Arena's files for that are in the `data-rows` folder:

- **`Replace`** – the five tables arena changes, complete: `itemstatcost.txt`, `levels.txt`, `lvlprest.txt`,
  `lvltypes.txt` and `objects.txt`, each the vanilla table with arena's rows added and its edits made.
- **`Add`** – only the rows arena adds (`itemstatcost.txt`, `levels.txt`, `lvlprest.txt`, `objects.txt`), each file with
  the table's header line.

Pick the way that fits your mod.

### A. Your mod uses the vanilla tables

If your mod doesn't change these five tables (or you start a new mod just for arena):

1. Copy the five files from `data-rows\Replace` into your mod's `data\global\excel` folder.
2. Copy arena's other files (see **Other files** below).
3. Put `d2rl-arena.dll` into your mod's `d2rloader\plugins` folder.

Nothing else to change: the ids in these tables match the defaults in `arena.toml`.

### B. Your mod already changes some of these tables

Then don't copy the `Replace` files over yours. Instead:

1. **Append** the rows from each file in `data-rows\Add` to the end of your table of the same name (skip the header
   line).
2. **Number** the new rows. In another mod they usually land on other ids, so give each new row the next free number
   in its table's id column, continuing your table's numbering. Write down every new id:

   | Table | Id column | Arena's rows | Ids in the arena mod |
   |---|---|---|---|
   | levels.txt | `Id` | Act 2 - Arena Tomb 1, Act 2 - Arena Tomb 2 | 138, 139 |
   | lvlprest.txt | `Def` | Act 2 - Arena Tomb 1, Act 2 - Arena Tomb 2 | 1092, 1093 |
   | objects.txt | `*ID` | arena_trapdoor_1, arena_trapdoor_2, arena_lobby | 587, 588, 589 |
   | itemstatcost.txt | `*ID` | arena_stake, arena_winnings | 368, 369 |

3. **Link the presets to the levels:** in your lvlprest.txt, set `LevelId` of the two arena rows to the new levels.txt
   ids of Arena Tomb 1 and Arena Tomb 2.
4. **Make arena's four edits to existing rows** in your tables (they are not in the `Add` files):

   | Table | Row | Edit |
   |---|---|---|
   | objects.txt | TeleportPad1, TeleportPad3, TeleportPad4 | `AutoMap` = 339 (the arena gates show the red portal marker on the automap) |
   | lvltypes.txt | Act 2 - Tomb | `File 12` = `Act2/Outdoors/Mesa.dt1`, `File 13` = `Act1/Graveyard/graveyrd.dt1` (used by Arena Tomb 2) |

5. **Update `arena.toml`** with your new ids. Start the game once so the plugin writes the file, then change these
   lines (shown here with the arena mod's ids):

   ```toml
   [arena.wager]
   stake_stat = 368      # your id of arena_stake
   winnings_stat = 369   # your id of arena_winnings

   [arena.lobby]
   object = 589          # your id of arena_lobby

   [arena.entrance]
   # four numbers per trapdoor: x, y, arena level id, trapdoor object id
   trapdoors = [5050, 5179, 138, 587, 5035, 5116, 139, 588]
   #                        ^^^  ^^^              ^^^  ^^^
   #           your Arena Tomb 1 id, arena_trapdoor_1 id / your Arena Tomb 2 id, arena_trapdoor_2 id

   [arena.level.138]     # rename to your Arena Tomb 1 id; keep the lines below it
   [arena.level.139]     # rename to your Arena Tomb 2 id; keep the lines below it
   ```

6. Copy arena's other files and put `d2rl-arena.dll` into your mod's `d2rloader\plugins` folder, as in way A.
7. Restart the game.

Two rules for the wager stats: `arena_stake` and `arena_winnings` must stay saved stats (`Saved` 1, `CSvBits` 25, as in
the `Add` file), and `stake_stat` / `winnings_stat` must never point at a row another stat uses, because arena writes
gold amounts into them. If you turn wagers off (`[arena.wager] enabled = false`), you can leave the two wager rows out.

### Other files
Copy these folders from `arena.mpq\data` into your mod's `data` folder:

- `global\tiles\arena\` – the two arena maps.
- `hd\env\preset\arena\`, `hd\env\biome\arena_*.json`, `hd\env\vis\arena_tomb_*.json`, `hd\env\texture\terrain\arena\` –
  the arenas in HD graphics.
- `hd\objects\doors\arena_trapdoor_*.json`, `hd\objects\env_pillars\arena_lobby.json` – the entrance trapdoors and the
  obelisk in HD.
- `hd\global\ui\panel\arena\` – the scoreboard art.

Two files may already exist in your mod. If they do, merge arena's entries into yours instead of copying over:

- `hd\objects\objects.json` – arena's entries for `arena_trapdoor_1`, `arena_trapdoor_2` and `arena_lobby`.
- `local\lng\strings\levels.json` – arena's seven strings: `ArenaTomb1Name`, `ArenaTomb1Warp`, `ArenaTomb1Entry`,
  `ArenaTomb2Name`, `ArenaTomb2Warp`, `ArenaTomb2Entry` and `ArenaLobbyName`.

Arena's rows name their strings as `d2rloader-mod:...`. If your mod uses another string namespace (the `namespace` in
your mod's `d2rloader\metadata.json`), change `d2rloader-mod` to yours in those rows.

## Notes and limits

- Up to 8 players: Battle Royale for 3 or more, team duels from 1 v 1 to 4 v 4.
- Team duels have no stakes yet.
- Corpse Explosion is refused in the arenas, but its effect still plays on your screen.
- Form parties before entering an arena: the game may refuse an invite between hostile players.
- Another plugin that changes teleport pads or trapdoors can't run together with this one. Arena then stays off and
  says so in its log.

## Credits

- Originally designed by Blizzard North for Diablo II 1.10. The arena maps were left in the game files but cut from it.
- Research on the cut arenas: The Phrozen Keep (d2mods.info) and The Cutting Room Floor (tcrf.net).
- Restored for D2RLoader by Kain.
