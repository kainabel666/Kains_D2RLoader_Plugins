# Missile HitCalc (missile-hitcalc)

A D2RLoader plugin for the "nova ring" missile hit functions: when a missile with one of
them hits, it bursts into a ring of sub-missiles. The base game always spawns a fixed ring.
With this plugin your mod's data sets:

- **How many** missiles the ring has, from a calc (`SHitCalc1` / `CHitCalc1`), so it can
  grow with skill level and synergies.
- **How wide** the ring is aimed, from the missile's `Radius` calc (or a setting).

Rows that don't use these columns behave exactly as in the base game.

## Install

1. Copy `d2rl-missile-hitcalc.dll` into your mod's `d2rloader/plugins/` folder.
2. Start the game once. The plugin writes its settings file to
   `d2rloader/config/missile-hitcalc.toml`.
3. Set up your missile rows (see **Your data**).
4. Restart the game after changing the settings or the txt files.

Type `missile-hitcalc` in the in-game console to check it loaded. It shows the settings,
whether each part is on, and how many rings it has handled.

## How it works

The ring is built twice: once by the server, which does the damage, and once by your
client, which is what you see. The server ring is not drawn, so set up both sides.

| | Server (damage) | Client (what you see) |
|---|---|---|
| Hit function | `pSrvHitFunc` = 5 | `pCltHitFunc` = 4 |
| Sub-missile | `HitSubMissile1` | `CltHitSubMissile1` |
| Count | `SHitCalc1` | `CHitCalc1` |
| Base-game step | `sHitPar1` | `cHitPar1` |

- **Count:** when the calc gives 1 or more, the ring has that many missiles, spread evenly
  all the way round (capped by `max_ring_count`). Calcs can use `lvl` and skill keywords.
- **Base game:** when the calc is empty or gives 0 or less, the base game's ring is used.
  It steps round 64 directions by the HitPar1 value: 1 = 64 missiles, 2 = 32, 4 = 16, 8 = 8.
- **Radius:** each ring missile is aimed at a point this far from the hit. Bigger values
  make the missiles fly straight out; at the base game's 30 they bend slightly (about 15°)
  after a short distance. Taken from the hit missile's `Radius` calc in missiles.txt; when
  that is empty or 0, from `ring_radius` in the settings. Most 99.
- The missiles are created the same way the base game does it: same owner, skill and skill
  level as the missile that hit.

## Settings (`missile-hitcalc.toml`)

```toml
[missile-hitcalc]
srvhit05_count_from_calc = true
clthit04_count_from_calc = true
max_ring_count = 64
ring_radius = 30
log_calls = false
```

| Setting | What it does |
|---|---|
| `srvhit05_count_from_calc` | Server ring (`pSrvHitFunc` 5) takes its count from `SHitCalc1`. `false` = base game. |
| `clthit04_count_from_calc` | Client ring (`pCltHitFunc` 4) takes its count from `CHitCalc1`. `false` = base game. |
| `max_ring_count` | Most missiles in one ring, 1 to 64. |
| `ring_radius` | Aim distance used when the missile's `Radius` is empty or 0. 1 to 99; 30 is the base game's. |
| `log_calls` | Writes each ring (missile, level, calc result, count, radius) to the plugin log, up to 256 per side. |

All settings except `log_calls` must be the same for the host and everyone who joins a
TCP/IP game.

## Your data (missiles.txt)

On the missile that bursts (example: Fire Bolt shooting a ring of fireballs):

| Column | Value | Notes |
|---|---|---|
| pSrvHitFunc | 5 | |
| HitSubMissile1 | fireball | missile the server ring spawns |
| sHitPar1 | 8 | used when the plugin is off or SHitCalc1 gives 0 |
| SHitCalc1 | `4+lvl` | ring count |
| pCltHitFunc | 4 | |
| CltHitSubMissile1 | fireball | missile you see |
| cHitPar1 | 8 | as sHitPar1 |
| CHitCalc1 | `4+lvl` | keep identical to SHitCalc1 |
| Radius | `90` | optional; empty = `ring_radius` |

## Notes and limits

- Keep `CHitCalc1` the same as `SHitCalc1`, or you will see a different ring from the one
  that does the damage.
- For other players to see the same count as the caster, use calcs that only use `lvl`.
  Their client may not know your skill levels, so `skill(...)` terms can give them a
  different number.
- The game also stores a missile's `Radius` on the missile when it is created. Some
  Reign of the Warlock missiles use that value for their own effect, so on those missiles
  the column does both jobs.
- Most 64 missiles per ring and a radius of at most 99.
- Tested in game so far (single player, Fire Bolt row above): `4+lvl` gives a ring of 5 at
  level 1 and 7 at level 3 on both the server and the client, and `ring_radius = 90` removes
  the bend. Not yet tested: the `Radius` column, and other players in a TCP/IP game.
