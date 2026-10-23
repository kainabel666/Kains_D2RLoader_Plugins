# auto-identify

Identifies items when a player picks them up, never when they drop. Which item
qualities get identified is set in the TOML config. It can also hide the affix
names of magic items and the rare-name line of rare and crafted items.

## Build and install

1. Run `build.cmd` in this folder (MSVC x64 + Ninja, Visual Studio at `M:\Tools\Visual Studio`).
2. Copy `build\d2rl-auto-identify.dll` to `G:\Diablo II Resurrected\d2rloader\plugins\`.
3. Start the game once. D2RLoader writes the config to
   `G:\Diablo II Resurrected\d2rloader\config\auto-identify.toml`. Edit that copy;
   `config\auto-identify.toml` in this folder is only the default.
4. Config changes take effect the next time the game starts.

The plugin uses the Shared role: the host identifies picked-up items and sends
them to every client, and each PC applies its own name settings. In TCP/IP games
the host and every client need the same plugin version.

## Config

```toml
[auto-identify]
enabled = true
qualities = ["magic", "set", "rare", "unique", "crafted"]
```

`qualities` takes names (case-insensitive) or the game's quality numbers:

| Name | Id |
|---|---|
| inferior | 1 |
| normal | 2 |
| superior | 3 |
| magic | 4 |
| set | 5 |
| rare | 6 |
| unique | 7 |
| crafted (or craft) | 8 |
| tempered | 9 |

An empty list (`qualities = []`) identifies nothing. Unknown names are logged and skipped.

Name display (each defaults to `false`):

| Key | When `true` |
|---|---|
| `hide_magic_affixes` | Magic items show only the base name, without prefix and suffix names. |
| `hide_rare_names` | Rare items show only the base name, without the rareprefix/raresuffix name line. |
| `hide_crafted_names` | Same for crafted items, which use the same name tables. |

These only change the name text. The name colour still shows the quality, and
the tooltip still lists every stat. Unidentified items already show only the base
name. Tempered items are not affected.

If you already have `d2rloader\config\auto-identify.toml`, D2RLoader keeps it as it
is, so add the three keys under `[auto-identify]` yourself.

## Check it loaded

Type `auto-identify` in the console. It prints `enabled`, `hook` (pickup), the
active qualities, how many items it has identified this session, `name_hook` and
the qualities whose names are hidden. Each hook is installed only when its
feature is in use. If a hook you expect is 0, look in the D2RLoader log for
"unexpected bytes", which means the game build differs.

## Behaviour

- It hooks the game's server pickup routine, which the pickup packet and three
  other player-action paths call, so pickups to the cursor and straight to the
  inventory are both covered.
- Gold and pickups the game merges into an item you already have (for example a
  scroll going into a tome) are skipped, because the game destroys the picked-up unit.
- Identification uses the game's own identify function: the same sound, charm
  refresh and client update as a scroll of identify.
- An unidentified item you drop and pick up again is identified.
- Works next to gamble-tweaks; the two hooks don't interfere.
- Name hiding hooks the game's item-name builder. Every name the game shows goes
  through it: tooltip, ground label and the rest. For a hidden quality the plugin
  briefly marks the client's copy of the item unidentified while the name is
  built, so the game writes the plain base name.

## Tests

`tests\run_tests.cmd` builds and runs the config parser test.

Reverse-engineering notes and addresses: `..\notes\auto-identify.md`.
