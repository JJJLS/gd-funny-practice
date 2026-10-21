// Input Buffer - practice "training wheels" for Geometry Dash (Geode, GD 2.2081).
//
// How it works:
//   1. Right before a click is applied, the mod snapshots the whole level state
//      (the same CheckpointObject that practice mode uses).
//   2. If you die within the rescue window after that click, the death is
//      cancelled, the level is rewound to the snapshot, and the same click is
//      replayed 1 tick later. If that also dies, it tries 2 ticks later, and so
//      on, up to the current stage limit (2 / 4 / 8 / 16 ticks).
//   3. If no delay up to the limit survives, you die normally.
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

// ------------------------------------------------------------ buffer state --

struct BufferState {
    Ref<CheckpointObject> snapshot; // level state right before the watched click
    bool watching = false;          // a click is inside its rescue window
    int snapTick = 0;               // tick the click originally happened on
    int button = 1;
    bool isPlayer1 = true;
    int delay = 0;                  // delay (ticks) currently being tried
    int pressedAt = 0;              // tick the press was actually applied this timeline
    int holdTicks = -1;             // how long the click was held (-1 = still held)

    bool pendingPress = false;
    int pressAt = 0;
    bool pendingRelease = false;
    int releaseAt = 0;

    bool rewindQueued = false;
    bool replaying = false;         // we are calling handleButton ourselves
};

BufferState g_buf;
int g_tick = 0;
int g_rescuesThisAttempt = 0;
float g_statusTimer = 0.f;

void resetBuffer() {
    g_buf = BufferState{};
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
    g_buf.snapshot = nullptr;
}

} // namespace

$execute {
    g_stage = std::clamp(Mod::get()->getSavedValue<int>("stage", 0), 0, STAGE_COUNT - 1);
    console::open();
    console::line("[InputBuffer] v1.0.0 LOADED");
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
    void replayButton(bool down) {
        g_buf.replaying = true;
        this->handleButton(down, g_buf.button, g_buf.isPlayer1);
        g_buf.replaying = false;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_buf.replaying || button != 1 || !bufferActiveFor(this)) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        if (down) {
            // A fresh click: stop watching the old one and snapshot for this one.
            finishWatch();
            g_buf.pendingPress = false;
            g_buf.pendingRelease = false;
            g_buf.snapshot = static_cast<PlayLayer*>(static_cast<GJBaseGameLayer*>(this))->createCheckpoint();
            g_buf.watching = g_buf.snapshot.data() != nullptr;
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

        if (g_buf.watching && g_buf.holdTicks < 0) {
            g_buf.holdTicks = std::max(1, g_tick - g_buf.pressedAt);
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (bufferActiveFor(this)) {
            if (g_buf.pendingPress && g_tick >= g_buf.pressAt) {
                g_buf.pendingPress = false;
                g_buf.pressedAt = g_tick;
                this->replayButton(true);
                if (g_buf.holdTicks >= 0) {
                    g_buf.pendingRelease = true;
                    g_buf.releaseAt = g_tick + std::max(1, g_buf.holdTicks);
                }
            }
            if (g_buf.pendingRelease && g_tick >= g_buf.releaseAt) {
                g_buf.pendingRelease = false;
                this->replayButton(false);
            }
            if (g_buf.watching && g_tick - g_buf.snapTick > rescueWindow()) {
                finishWatch();
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        g_tick++;
    }
};

// --------------------------------------------------------- death + rewind --

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
        // Already rescuing this frame: ignore further hits until we rewind.
        if (g_buf.rewindQueued) return;

        bool inWindow = g_buf.watching && g_buf.snapshot.data() && g_tick - g_buf.snapTick <= rescueWindow();
        if (inWindow && g_buf.delay < bufferTicks()) {
            g_buf.rewindQueued = true;
            return;
        }

        if (inWindow && g_buf.delay > 0) {
            console::line(fmt::format(
                "xxx {} [Click] -> NOT SAVED (tried 1..{} ticks of delay)", g_buf.snapTick, g_buf.delay
            ));
        }
        resetBuffer();
        PlayLayer::destroyPlayer(player, object);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (g_buf.rewindQueued && g_buf.snapshot.data()) {
            g_buf.rewindQueued = false;
            g_buf.delay++;
            this->loadFromCheckpoint(g_buf.snapshot.data());
            g_tick = g_buf.snapTick;
            g_buf.pendingPress = true;
            g_buf.pressAt = g_buf.snapTick + g_buf.delay;
            g_buf.pendingRelease = false;
        }

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
