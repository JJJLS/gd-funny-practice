// Input Buffer - practice "training wheels" for Geometry Dash (Geode, GD 2.2081).
//
// How it works:
//   1. Right before a click is applied, the mod snapshots the level (practice
//      checkpoint) and both players' physics state.
//   2. If that click gets you killed within the rescue window, the death is
//      cancelled and, in the same tick, the mod rewinds to the snapshot and
//      silently re-simulates the ticks with the click moved 1 tick later, then
//      2, 3 ... up to the stage limit. The first timing that survives is kept,
//      so your click lands on the earliest tick that doesn't kill you.
//   3. If no timing up to the limit survives, you die normally.
//   Everything happens before the frame is drawn, so you never see a rewind
//   and you never pass through an obstacle.
//
// F6 cycles the stage (2 -> 4 -> 8 -> 16 -> OFF). F7 shows/hides the console.

#define NOMINMAX
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace geode::prelude;

namespace {

constexpr int STAGES[] = {2, 4, 8, 16, 0}; // 0 = OFF
constexpr int STAGE_COUNT = sizeof(STAGES) / sizeof(STAGES[0]);
constexpr double TICKS_PER_60FPS_FRAME = 240.0 / 60.0;

int g_stage = 0;

int bufferTicks() { return STAGES[g_stage]; }

double ticksToFrames(int ticks) { return ticks / TICKS_PER_60FPS_FRAME; }

std::string stageText() {
    if (bufferTicks() == 0) return "OFF (raw input)";
    return fmt::format("{} ticks save ({:.2f} frames @ 60FPS)", bufferTicks(), ticksToFrames(bufferTicks()));
}

// ---------------------------------------------------------------- console --

namespace console {
    bool g_ready = false;
    bool g_visible = true;
    std::string g_status;

    void open() {
#ifdef GEODE_IS_WINDOWS
        if (!GetConsoleWindow()) AllocConsole();
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
        SetConsoleTitleW(L"Geode Modded Executable - Live Console");
        g_ready = true;
#endif
    }

    // Permanent log line; the live status line is redrawn underneath it.
    void line(std::string const& text) {
        log::info("{}", text);
        if (!g_ready) return;
        std::printf("\r%-110s\n%s", text.c_str(), g_status.c_str());
        std::fflush(stdout);
    }

    void status(std::string const& text) {
        if (!g_ready || text == g_status) return;
        g_status = text;
        std::printf("\r%-110s", text.c_str());
        std::fflush(stdout);
    }

    void toggleVisible() {
#ifdef GEODE_IS_WINDOWS
        if (auto hwnd = GetConsoleWindow()) {
            g_visible = !g_visible;
            ShowWindow(hwnd, g_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
#endif
    }
}

bool debugLog() {
    return Mod::get()->getSettingValue<bool>("debug-log");
}

// ------------------------------------------------------------ buffer state --

// Everything needed to put a player back exactly where it was.
struct PlayerSnap {
    Ref<PlayerCheckpoint> checkpoint;
    gd::unordered_set<int> touchedRings;
    gd::map<int, bool> holdingButtons;

    void save(PlayerObject* player) {
        checkpoint = PlayerCheckpoint::create();
        player->saveToCheckpoint(checkpoint.data());
        touchedRings = player->m_touchedRings;
        holdingButtons = player->m_holdingButtons;
    }

    void load(PlayerObject* player) {
        player->loadFromCheckpoint(checkpoint.data());
        player->m_touchedRings = touchedRings;
        player->m_holdingButtons = holdingButtons;
    }
};

struct BufferState {
    Ref<CheckpointObject> level;    // level state right before the watched click
    PlayerSnap p1, p2;
    bool watching = false;          // a click is inside its rescue window
    int snapTick = 0;               // tick the click originally happened on
    int button = 1;
    bool isPlayer1 = true;
    int delay = 0;                  // delay (ticks) of the timeline we're on now
    int pressedAt = -1;             // tick the press was applied in this timeline (-1 = not yet)
    int holdTicks = -1;             // how long the click was held (-1 = still held)

    bool pendingPress = false;
    int pressAt = 0;
    bool pendingRelease = false;
    int releaseAt = 0;
};

BufferState g_buf;
int g_tick = 0;
int g_rescuesThisAttempt = 0;
float g_statusTimer = 0.f;
float g_stepDt = 1.f / 240.f;

bool g_replaying = false;          // we are calling handleButton ourselves
bool g_simulating = false;         // we are re-running ticks in a rescue
bool g_rescueNeeded = false;       // a death was swallowed this tick
bool g_simDied = false;
bool g_restoreFailed = false;
PlayerObject* g_deathPlayer = nullptr;
GameObject* g_deathObject = nullptr;

void resetBuffer() {
    g_buf = BufferState{};
    g_rescueNeeded = false;
}

int rescueWindow() {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>("rescue-window"));
}

bool bufferActiveFor(GJBaseGameLayer* layer) {
    auto pl = PlayLayer::get();
    if (!pl || layer != pl) return false;
    if (bufferTicks() == 0 || pl->m_isPlatformer) return false;
    if (Mod::get()->getSettingValue<bool>("practice-only") && !pl->m_isPracticeMode) return false;
    return true;
}

// Called once a watched click has made it through its rescue window.
void finishWatch() {
    if (g_buf.watching && g_buf.delay > 0) {
        console::line(fmt::format(
            ">>> {} [Click] -> SAVED {}/{} TICKS ({:.2f} Frames @ 60FPS) -> {} Delayed",
            g_buf.snapTick, g_buf.delay, bufferTicks(), ticksToFrames(g_buf.delay), g_buf.delay
        ));
        g_rescuesThisAttempt++;
    }
    g_buf.watching = false;
    g_buf.level = nullptr;
    g_buf.p1 = {};
    g_buf.p2 = {};
}

void replayButton(GJBaseGameLayer* layer, bool down) {
    g_replaying = true;
    layer->handleButton(down, g_buf.button, g_buf.isPlayer1);
    g_replaying = false;
}

// Put the level and players back to the moment before the watched click.
// Returns false if the player didn't actually move back (rescue unsafe).
bool restoreSnapshot(PlayLayer* pl) {
    pl->loadFromCheckpoint(g_buf.level.data());
    g_buf.p1.load(pl->m_player1);
    if (g_buf.p2.checkpoint.data()) g_buf.p2.load(pl->m_player2);
    pl->m_playerDied = false;
    g_tick = g_buf.snapTick;

    auto want = g_buf.p1.checkpoint->m_position;
    auto got = pl->m_player1->m_position;
    bool ok = std::abs(want.x - got.x) < 0.5f && std::abs(want.y - got.y) < 0.5f;
    if (!ok) {
        console::line(fmt::format(
            "    [debug] restore to tick {}: want ({:.1f}, {:.1f}) got ({:.1f}, {:.1f}) {}",
            g_buf.snapTick, want.x, want.y, got.x, got.y, ok ? "OK" : "FAILED"
        ));
    }
    return ok;
}

// Re-run ticks [snapTick, untilTick) with the click moved to snapTick + delay.
// Returns true if the player survives all of them.
bool simulate(PlayLayer* pl, int delay, int untilTick) {
    g_restoreFailed = !restoreSnapshot(pl);
    if (g_restoreFailed) return false;

    int pressAt = g_buf.snapTick + delay;
    g_buf.pressedAt = -1;
    g_buf.pendingRelease = false;
    g_simDied = false;
    g_simulating = true;

    while (g_tick < untilTick && !g_simDied) {
        if (g_buf.pressedAt < 0 && g_tick >= pressAt) {
            g_buf.pressedAt = g_tick;
            replayButton(pl, true);
        }
        if (g_buf.pressedAt >= 0 && g_buf.holdTicks >= 0 && !g_buf.pendingRelease
            && g_tick >= g_buf.pressedAt + std::max(1, g_buf.holdTicks)) {
            g_buf.pendingRelease = true; // marks "already released" inside the sim
            replayButton(pl, false);
        }
        pl->processCommands(g_stepDt, false, false);
        g_tick++;
    }

    g_simulating = false;
    bool released = g_buf.pendingRelease;
    g_buf.delay = delay;

    // Hand whatever hasn't happened yet back to real time.
    g_buf.pendingPress = g_buf.pressedAt < 0;
    g_buf.pressAt = pressAt;
    g_buf.pendingRelease = !released && g_buf.holdTicks >= 0;
    if (g_buf.pendingRelease) {
        g_buf.releaseAt = (g_buf.pressedAt < 0 ? pressAt : g_buf.pressedAt) + std::max(1, g_buf.holdTicks);
    }
    return !g_simDied;
}

// A death was swallowed this tick: find the earliest later click that survives.
void runRescue(PlayLayer* pl) {
    int deathTick = g_tick;
    auto deathPlayer = g_deathPlayer;
    auto deathObject = g_deathObject;

    std::string trace;
    for (int d = g_buf.delay + 1; d <= bufferTicks(); d++) {
        bool survived = simulate(pl, d, deathTick);
        if (g_restoreFailed) break;
        trace += survived ? fmt::format(" +{} ok", d) : fmt::format(" +{} dies@{}", d, g_tick);
        if (survived) {
            if (debugLog()) console::line(fmt::format("    [debug] click @{} died @{}:{}", g_buf.snapTick, deathTick, trace));
            return;
        }
        deathPlayer = g_deathPlayer;
        deathObject = g_deathObject;
    }
    if (debugLog()) console::line(fmt::format("    [debug] click @{} died @{}:{}", g_buf.snapTick, deathTick, trace));

    console::line(fmt::format(
        "xxx {} [Click] -> NOT SAVED (no timing within {} ticks survives)", g_buf.snapTick, bufferTicks()
    ));
    resetBuffer();
    pl->destroyPlayer(deathPlayer, deathObject);
}

} // namespace

$execute {
    g_stage = std::clamp(Mod::get()->getSavedValue<int>("stage", 0), 0, STAGE_COUNT - 1);
    console::open();
    console::line("[InputBuffer] v1.1.0 LOADED");
    console::line(fmt::format("[InputBuffer] STAGE {}: {}   (F6 = cycle stage, F7 = hide/show console)", g_stage + 1, stageText()));
}

// ------------------------------------------------------------------ hotkeys --

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (isKeyDown && !isKeyRepeat) {
            if (key == KEY_F6) {
                g_stage = (g_stage + 1) % STAGE_COUNT;
                Mod::get()->setSavedValue<int>("stage", g_stage);
                resetBuffer();
                console::line(fmt::format("[InputBuffer] STAGE {}: {}", g_stage + 1, stageText()));
            }
            else if (key == KEY_F7) {
                console::toggleVisible();
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};

// ------------------------------------------------------------ input + ticks --

class $modify(IBGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_replaying || g_simulating || button != 1 || !bufferActiveFor(this)) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        auto pl = static_cast<PlayLayer*>(static_cast<GJBaseGameLayer*>(this));

        if (down) {
            // A fresh click: stop watching the old one and snapshot for this one.
            finishWatch();
            g_buf.pendingPress = false;
            g_buf.pendingRelease = false;
            g_buf.level = pl->createCheckpoint();
            g_buf.watching = g_buf.level.data() != nullptr;
            if (g_buf.watching) {
                g_buf.p1.save(m_player1);
                if (m_gameState.m_isDualMode) g_buf.p2.save(m_player2);
            }
            g_buf.snapTick = g_tick;
            g_buf.pressedAt = g_tick;
            g_buf.button = button;
            g_buf.isPlayer1 = isPlayer1;
            g_buf.delay = 0;
            g_buf.holdTicks = -1;
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // Release while our delayed press hasn't fired yet: keep it as a short tap.
        if (g_buf.pendingPress) {
            if (g_buf.holdTicks < 0) g_buf.holdTicks = 1;
            return;
        }
        // Our own scheduled release will handle it.
        if (g_buf.pendingRelease) return;

        if (g_buf.watching && g_buf.holdTicks < 0 && g_buf.pressedAt >= 0) {
            g_buf.holdTicks = std::max(1, g_tick - g_buf.pressedAt);
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        // Inside a rescue the simulation loop drives ticks itself.
        if (g_simulating || !bufferActiveFor(this)) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            if (!g_simulating) g_tick++;
            return;
        }

        g_stepDt = dt;
        if (g_buf.pendingPress && g_tick >= g_buf.pressAt) {
            g_buf.pendingPress = false;
            g_buf.pressedAt = g_tick;
            replayButton(this, true);
            if (g_buf.holdTicks >= 0) {
                g_buf.pendingRelease = true;
                g_buf.releaseAt = g_tick + std::max(1, g_buf.holdTicks);
            }
        }
        if (g_buf.pendingRelease && g_tick >= g_buf.releaseAt) {
            g_buf.pendingRelease = false;
            replayButton(this, false);
        }
        if (g_buf.watching && g_tick - g_buf.snapTick > rescueWindow()) {
            finishWatch();
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        g_tick++;

        if (g_rescueNeeded) {
            g_rescueNeeded = false;
            runRescue(static_cast<PlayLayer*>(static_cast<GJBaseGameLayer*>(this)));
        }
    }
};

// --------------------------------------------------------- death + rescue --

class $modify(IBPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* label = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        g_tick = 0;
        g_rescuesThisAttempt = 0;
        resetBuffer();

        auto winSize = CCDirector::get()->getWinSize();
        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setAnchorPoint({0.f, 1.f});
        label->setScale(0.3f);
        label->setOpacity(170);
        label->setPosition({4.f, winSize.height - 4.f});
        label->setID("input-buffer-label"_spr);
        this->addChild(label, 1000);
        m_fields->label = label;
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // The anticheat spike "kill" at level start must always go through.
        if (object == m_anticheatSpike || !bufferActiveFor(this)) {
            return PlayLayer::destroyPlayer(player, object);
        }
        // A death inside a simulated timeline just marks that timing as bad.
        if (g_simulating) {
            g_simDied = true;
            g_deathPlayer = player;
            g_deathObject = object;
            return;
        }
        // Already rescuing this tick: ignore further hits until the tick ends.
        if (g_rescueNeeded) return;

        bool inWindow = g_buf.watching && g_tick - g_buf.snapTick <= rescueWindow();
        if (inWindow && g_buf.delay < bufferTicks()) {
            g_rescueNeeded = true;
            g_deathPlayer = player;
            g_deathObject = object;
            return;
        }

        if (inWindow && g_buf.delay > 0) {
            console::line(fmt::format(
                "xxx {} [Click] -> NOT SAVED (no timing within {} ticks survives)", g_buf.snapTick, bufferTicks()
            ));
        }
        resetBuffer();
        PlayLayer::destroyPlayer(player, object);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (auto label = m_fields->label) {
            bool show = Mod::get()->getSettingValue<bool>("show-label");
            label->setVisible(show);
            if (show) {
                label->setString(fmt::format(
                    "IB {} | tick {} | saves {}",
                    bufferTicks() ? fmt::format("{}t", bufferTicks()) : std::string("OFF"),
                    g_tick, g_rescuesThisAttempt
                ).c_str());
            }
        }

        g_statusTimer += dt;
        if (g_statusTimer >= 0.05f) {
            g_statusTimer = 0.f;
            console::status(fmt::format(
                "[LIVE] tick {:>7} | stage {} | saves this attempt: {}",
                g_tick, stageText(), g_rescuesThisAttempt
            ));
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_tick = 0;
        g_rescuesThisAttempt = 0;
        resetBuffer();
    }

    void onQuit() {
        resetBuffer();
        PlayLayer::onQuit();
    }
};
