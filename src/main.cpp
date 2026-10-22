// Input Buffer - practice "training wheels" for Geometry Dash (Geode, GD 2.2081).
//
// How it works (ghost prediction, nothing is ever rewound):
//   1. The mod keeps a hidden "ghost" copy of the player.
//   2. When you click, the ghost is set to your exact state and run forward a
//      short distance through the level's solids and hazards, clicking right
//      now. If the ghost survives, your click goes through untouched.
//   3. If the ghost dies, the mod tries the click 1 tick later, 2 ticks later,
//      ... up to the stage limit, and holds your click until the first tick
//      where the ghost survives.
//   4. In wave mode, releases get the same treatment with their own limit.
//
// F6 cycles the stage. F7 shows/hides the console.

#define NOMINMAX
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {

struct Stage {
    int press;       // max ticks a click (any mode) can be moved later
    int waveRelease; // max ticks a wave release can be moved later
};

constexpr Stage STAGES[] = {{2, 2}, {4, 4}, {8, 8}, {16, 8}, {0, 0}}; // last = OFF
constexpr int STAGE_COUNT = sizeof(STAGES) / sizeof(STAGES[0]);
constexpr double TICKS_PER_60FPS_FRAME = 240.0 / 60.0;

int g_stage = 0;

Stage const& stage() { return STAGES[g_stage]; }

double ticksToFrames(int ticks) { return ticks / TICKS_PER_60FPS_FRAME; }

std::string stageText() {
    if (stage().press == 0) return "OFF (raw input)";
    return fmt::format(
        "{} ticks all | {} ticks wave ({:.2f}f / {:.2f}f @ 60fps)",
        stage().press, stage().waveRelease, ticksToFrames(stage().press), ticksToFrames(stage().waveRelease)
    );
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

bool debugLog() { return Mod::get()->getSettingValue<bool>("debug-log"); }

int horizon() { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("prediction-horizon")); }

// ------------------------------------------------------------------ state --

struct QueuedInput {
    bool down;
    bool isPlayer1;
    int fireTick;
};

// Compares the chosen prediction against what really happens (debug only).
struct PredictionCheck {
    bool active = false;
    PlayerObject* player = nullptr;
    int startTick = 0;
    std::vector<CCPoint> predicted;
    float maxError = 0.f;
    bool saved = false;
};

Ref<PlayerObject> g_ghost;
Ref<PlayerCheckpoint> g_ghostState;
bool g_predicting = false;
bool g_ghostDied = false;
float g_playerDt = 0.25f;

std::deque<QueuedInput> g_queue;
int g_shift = 0;            // delay given to the latest click (keeps hold length)
bool g_replaying = false;   // we are calling handleButton ourselves
PredictionCheck g_check;

int g_tick = 0;
int g_savesThisAttempt = 0;
float g_statusTimer = 0.f;

void resetBuffer() {
    g_queue.clear();
    g_shift = 0;
    g_check = {};
}

PlayLayer* asPlayLayer(GJBaseGameLayer* layer) {
    return static_cast<PlayLayer*>(layer);
}

bool bufferActiveFor(GJBaseGameLayer* layer) {
    auto pl = PlayLayer::get();
    if (!pl || layer != pl || !g_ghost.data()) return false;
    if (stage().press == 0 || pl->m_isPlatformer) return false;
    if (Mod::get()->getSettingValue<bool>("practice-only") && !pl->m_isPracticeMode) return false;
    return true;
}

// Speed and size portals are the only special objects the ghost reacts to.
bool isGhostPortal(int id) {
    switch (id) {
        case 99: case 101:                               // normal / mini size
        case 200: case 201: case 202: case 203: case 1334: // speed portals
            return true;
        default:
            return false;
    }
}

void applyGhostPortal(PlayerObject* ghost, int id) {
    switch (id) {
        case 101: ghost->togglePlayerScale(true, true); ghost->updatePlayerScale(); break;
        case 99: ghost->togglePlayerScale(false, true); ghost->updatePlayerScale(); break;
        case 200: ghost->m_playerSpeed = 0.7f; break;
        case 201: ghost->m_playerSpeed = 0.9f; break;
        case 202: ghost->m_playerSpeed = 1.1f; break;
        case 203: ghost->m_playerSpeed = 1.3f; break;
        case 1334: ghost->m_playerSpeed = 1.6f; break;
        default: break;
    }
}

// --------------------------------------------------------------- the ghost --

void copyToGhost(PlayerObject* real, bool holding) {
    auto ghost = g_ghost.data();
    if (ghost->m_holdingButtons[1]) ghost->releaseButton(PlayerButton::Jump);
    real->saveToCheckpoint(g_ghostState.data());
    ghost->loadFromCheckpoint(g_ghostState.data());
    ghost->m_isDead = false;
    ghost->setRotation(real->getRotation());
    if (holding) ghost->pushButton(PlayerButton::Jump);
}

// Runs the ghost forward `ticks` steps from `real`'s current state, pushing
// (or releasing) the button at step `inputAt`. True if the ghost survives.
bool ghostSurvives(PlayLayer* pl, PlayerObject* real, bool push, int inputAt, int ticks,
                   std::vector<CCPoint>* track) {
    auto ghost = g_ghost.data();
    g_predicting = true;
    g_ghostDied = false;
    copyToGhost(real, !push);

    for (int i = 0; i < ticks && !g_ghostDied; i++) {
        ghost->m_collisionLogTop->removeAllObjects();
        ghost->m_collisionLogBottom->removeAllObjects();
        ghost->m_collisionLogLeft->removeAllObjects();
        ghost->m_collisionLogRight->removeAllObjects();

        pl->checkCollisions(ghost, g_playerDt, false);
        if (g_ghostDied) break;

        if (i == inputAt) {
            if (push) ghost->pushButton(PlayerButton::Jump);
            else ghost->releaseButton(PlayerButton::Jump);
        }
        ghost->update(g_playerDt);
        ghost->updateRotation(g_playerDt);
        ghost->updatePlayerScale();
        if (track) track->push_back(ghost->getPosition());
    }

    g_predicting = false;
    return !g_ghostDied;
}

// Earliest delay (0..limit ticks) at which this input doesn't kill the player.
int chooseDelay(PlayLayer* pl, PlayerObject* player, bool down, int limit) {
    std::vector<CCPoint> track;
    int chosen = -1;
    std::string trace;

    for (int d = 0; d <= limit; d++) {
        track.clear();
        if (ghostSurvives(pl, player, down, d, d + horizon(), &track)) {
            chosen = d;
            break;
        }
        trace += fmt::format(" +{}x", d);
    }

    if (chosen < 0) {
        if (debugLog()) {
            console::line(fmt::format(
                "    [debug] {} [{}] no safe timing within {} ticks:{} (input left as-is)",
                g_tick, down ? "Click" : "Release", limit, trace
            ));
        }
        return 0;
    }

    g_check = {};
    g_check.active = true;
    g_check.player = player;
    g_check.startTick = g_tick;
    g_check.predicted = std::move(track);
    g_check.saved = chosen > 0;
    return chosen;
}

void finishCheck(char const* why) {
    if (g_check.active && debugLog() && (g_check.saved || g_check.maxError > 0.5f)) {
        console::line(fmt::format(
            "    [debug] prediction vs real: max error {:.2f} units ({})", g_check.maxError, why
        ));
    }
    g_check.active = false;
}

void replay(GJBaseGameLayer* layer, QueuedInput const& input) {
    g_replaying = true;
    layer->handleButton(input.down, 1, input.isPlayer1);
    g_replaying = false;
}

} // namespace

$execute {
    g_stage = std::clamp(Mod::get()->getSavedValue<int>("stage", 0), 0, STAGE_COUNT - 1);
    console::open();
    console::line("[InputBuffer] v2.0.0 LOADED (ghost prediction)");
    console::line(fmt::format("[InputBuffer] STAGE {}: {}", g_stage + 1, stageText()));
    console::line("[InputBuffer] F6 = cycle 2/2 -> 4/4 -> 8/8 -> 16/8 -> OFF   F7 = hide/show console");
}

// ------------------------------------------------------------------ hotkeys --

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (isKeyDown && !isKeyRepeat) {
            if (key == KEY_F6) {
                g_stage = (g_stage + 1) % STAGE_COUNT;
                Mod::get()->setSavedValue<int>("stage", g_stage);
                resetBuffer();
                console::line(fmt::format("[InputBuffer] F6 -> STAGE {}: {}", g_stage + 1, stageText()));
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
        if (g_replaying || g_predicting || button != 1 || !bufferActiveFor(this)) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        finishCheck("new input");

        // An earlier input is still waiting: keep the order and the hold length.
        if (!g_queue.empty()) {
            int fire = std::max(g_queue.back().fireTick + 1, g_tick + g_shift);
            g_queue.push_back({down, isPlayer1, fire});
            return;
        }

        auto player = isPlayer1 ? m_player1 : m_player2;
        int limit = down ? stage().press : (player->m_isDart ? stage().waveRelease : 0);
        int delay = limit > 0 ? chooseDelay(asPlayLayer(this), player, down, limit) : 0;
        if (down || delay > 0) g_shift = delay;

        if (delay == 0) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        g_queue.push_back({down, isPlayer1, g_tick + delay});
        g_savesThisAttempt++;
        console::line(fmt::format(
            ">>> {} [{}] -> SAVED {}/{} TICKS ({:.2f} Frames @ 60FPS) -> {} Delayed",
            g_tick, down ? "Click" : "Release", delay, limit, ticksToFrames(delay), delay
        ));
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!bufferActiveFor(this)) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            g_tick++;
            return;
        }

        while (!g_queue.empty() && g_queue.front().fireTick <= g_tick) {
            auto input = g_queue.front();
            g_queue.pop_front();
            replay(this, input);
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (g_check.active) {
            size_t i = static_cast<size_t>(std::max(0, g_tick - g_check.startTick));
            if (i < g_check.predicted.size()) {
                auto real = g_check.player->getPosition();
                auto want = g_check.predicted[i];
                g_check.maxError = std::max(g_check.maxError, std::hypot(real.x - want.x, real.y - want.y));
            }
            if (i + 1 >= g_check.predicted.size()) finishCheck("full horizon");
        }
        g_tick++;
    }

    // ---- keep the ghost from touching the real level while predicting ----

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt) {
        if (!g_predicting) {
            return GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
        }
        // Only solids, hazards, slopes and speed/size portals exist for the ghost.
        std::vector<GameObject*> disabled;
        for (auto obj : *objects) {
            if (!obj || obj->m_isDisabled || obj->m_isDisabled2) continue;
            auto type = obj->m_objectType;
            bool keep = type == GameObjectType::Solid || type == GameObjectType::Hazard
                || type == GameObjectType::AnimatedHazard || type == GameObjectType::Slope
                || isGhostPortal(obj->m_objectID);
            if (!keep) {
                obj->m_isDisabled = true;
                obj->m_isDisabled2 = true;
                disabled.push_back(obj);
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
        for (auto obj : disabled) {
            obj->m_isDisabled = false;
            obj->m_isDisabled2 = false;
        }
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (g_predicting) {
            applyGhostPortal(player, object->m_objectID);
            return false;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
        if (g_predicting) return applyGhostPortal(player, object->m_objectID);
        GJBaseGameLayer::playerTouchedTrigger(player, object);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* object) {
        if (!g_predicting) GJBaseGameLayer::playerTouchedRing(player, object);
    }

    void activateSFXTrigger(SFXTriggerGameObject* object) {
        if (!g_predicting) GJBaseGameLayer::activateSFXTrigger(object);
    }

    void activateSongEditTrigger(SongTriggerGameObject* object) {
        if (!g_predicting) GJBaseGameLayer::activateSongEditTrigger(object);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (!g_predicting) GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }
};

class $modify(IBPlayerObject, PlayerObject) {
    void update(float dt) {
        PlayerObject::update(dt);
        if (!g_predicting) g_playerDt = dt; // the real step size the ghost must use
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (!g_predicting) PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (!g_predicting) PlayerObject::incrementJumps();
    }

    void ringJump(RingObject* object, bool skipCheck) {
        if (!g_predicting) PlayerObject::ringJump(object, skipCheck);
    }
};

class $modify(HardStreak) {
    void addPoint(CCPoint point) {
        if (!g_predicting) HardStreak::addPoint(point);
    }
};

class $modify(GameObject) {
    void playShineEffect() {
        if (!g_predicting) GameObject::playShineEffect();
    }
};

class $modify(EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (!g_predicting) EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

// ----------------------------------------------------- level + ghost setup --

class $modify(IBPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* label = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        g_tick = 0;
        g_savesThisAttempt = 0;
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

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        auto ghost = PlayerObject::create(1, 1, this, this, true);
        ghost->setPosition({0.f, 105.f});
        ghost->setVisible(false);
        m_objectLayer->addChild(ghost);
        g_ghost = ghost;
        g_ghostState = PlayerCheckpoint::create();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_predicting || player == g_ghost.data()) {
            g_ghostDied = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void playEndAnimationToPos(CCPoint position) {
        if (!g_predicting) PlayLayer::playEndAnimationToPos(position);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (auto label = m_fields->label) {
            bool show = Mod::get()->getSettingValue<bool>("show-label");
            label->setVisible(show);
            if (show) {
                label->setString(fmt::format(
                    "IB {} | tick {} | saves {}",
                    stage().press ? fmt::format("{}/{}t", stage().press, stage().waveRelease) : std::string("OFF"),
                    g_tick, g_savesThisAttempt
                ).c_str());
            }
        }

        g_statusTimer += dt;
        if (g_statusTimer >= 0.05f) {
            g_statusTimer = 0.f;
            console::status(fmt::format(
                "[LIVE] tick {:>7} | stage {} | saves this attempt: {}",
                g_tick, stageText(), g_savesThisAttempt
            ));
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_tick = 0;
        g_savesThisAttempt = 0;
        resetBuffer();
    }

    void onQuit() {
        resetBuffer();
        g_ghost = nullptr;
        g_ghostState = nullptr;
        PlayLayer::onQuit();
    }
};
