# Input Buffer: training wheels for Geometry Dash

This mod saves you when you click a little **too early**. If an early click would have killed you, the mod rewinds and replays that click a few ticks later, so you get through tight 1-frame gaps, orbs and wave corridors.

Built for **Geode 5.11.2** and **Geometry Dash 2.2081** (Windows, Steam).

## What it does

- Right before each click is applied, the mod saves a snapshot of the level. This is the same kind of snapshot practice mode uses for checkpoints.
- If you die within the **rescue window** after that click (120 ticks, about 0.5 s, by default), the death is cancelled. The level rewinds to the snapshot and the click is replayed **1 tick later**. If that run also dies, the click is retried at 2 ticks later, then 3, and so on, up to the current stage limit.
- If no delay up to the limit survives, you die normally. Late clicks are never saved, only early ones.
- GD runs physics at 240 ticks per second, so 4 ticks equal 1 frame at 60 FPS.

| F6 stage | Max delay | ≈ frames @ 60 FPS |
|---|---|---|
| 1 | 2 ticks | 0.5 |
| 2 | 4 ticks | 1 |
| 3 | 8 ticks | 2 |
| 4 | 16 ticks | 4 |
| 5 | OFF | raw input |

**Keys (while in game):** `F6` cycles the stage, and the choice is remembered. `F7` hides or shows the console window.

**Console:** a separate window titled *Geode Modded Executable - Live Console* opens when the game starts and always stays open. Its bottom line shows the live tick counter. Every save is logged like this:

```
>>> 431 [Click] -> SAVED 3/8 TICKS (0.75 Frames @ 60FPS) -> 3 Delayed
```

A small label in the top-left corner of the game shows the same info. You can turn it off in the mod's settings.

**Settings** (Geode menu → Input Buffer → ⚙): rescue window, *Practice mode only*, and the in-game label.

> ⚠️ It's a practice tool. Anything you beat with it on is assisted. Don't submit those completions to leaderboards or lists. Turn on **Practice mode only** if you want normal-mode runs to stay clean.

---

## Setup (first time)

### 1. Install Geode (the mod loader)

1. Go to **https://geode-sdk.org** → *Download* → run the Windows installer.
2. Point it at your Geometry Dash folder: `C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash`
3. Launch GD. A new **Geode** button (a green gem) appears on the main menu. That means it worked.

### 2. Get the built mod file (`jaheim.input-buffer.geode`)

You have to compile the source code into a `.geode` file. The easiest way needs no programming tools on your PC:

**Option A: GitHub builds it for you (recommended)**

1. Make a free account at github.com and create a new repository (any name, e.g. `gd-input-buffer`).
2. Upload everything in this `InputBuffer` folder to that repository, **including the hidden `.github` folder**. You can drag and drop on the repository page, or push it with git.
3. Open the repository's **Actions** tab. The *Build Geode Mod* job starts automatically and takes about 5–10 minutes. If it doesn't start, click it → *Run workflow*.
4. When it shows a green check, open the run and download the **InputBuffer** artifact (a zip). Inside it is `jaheim.input-buffer.geode`.

**Option B: build on your PC**

1. Install Visual Studio 2022 (the "Desktop development with C++" workload), CMake, and the Geode CLI (`winget install GeodeSDK.GeodeCLI`).
2. Run `geode sdk install` and then `geode sdk install-binaries`.
3. In this folder, run `geode build`. The `.geode` file ends up in `build\`.

### 3. Install the mod

Copy `jaheim.input-buffer.geode` into:

```
C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash\geode\mods
```

Restart GD. The console window opens along with the game. Go into a level, press **F6** to pick a stage, and play.

## Known limitations

- This is an untested first version. It was written against the Geode 2.2081 bindings and hasn't been compiled or run yet. Expect to fix a build error or a few game bugs.
- Each save rewinds up to the stage limit in ticks (at most 16 ticks, about 67 ms), so you may see a tiny hitch when it saves you.
- Platformer levels are skipped. Only the main jump/click is buffered.
- The tick counter counts ticks in the current attempt. It resets when you respawn.
