#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/SliderNode.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr int DEFAULT_LIMIT_TICKS = 130;
    constexpr int MAX_LIMIT_TICKS = 16384;
    constexpr float TICK_SECONDS = 1.f / 240.f;
    constexpr char const* SAVED_LIMIT_KEY = "shared-early-click-limit";

    // A single safety budget prevents a long prediction from freezing the game.
    // The delay cap and the simulation horizon still come from the same slider.
    constexpr auto SEARCH_BUDGET = std::chrono::milliseconds(8);
    constexpr uint32_t MAX_SIMULATION_STEPS = 120000;

    struct QueuedInput {
        uint64_t dueTick = 0;
        bool down = false;
        int button = 1;
        bool isPlayer1 = true;
    };

    struct PlayerBuffer {
        PlayerObject* ghost = nullptr;
        CCLayer* ghostLayer = nullptr;
        std::deque<QueuedInput> inputs;
        uint64_t tick = 0;
        float tickRemainder = 0.f;
        int activeShift = 0;
        int pressStartTick = -1;
        int tapHoldTicks = 12;
        int robotHoldTicks = 12;
        int shipHoldTicks = 18;
        int waveHoldTicks = 12;
        int swingHoldTicks = 12;
        bool pressWasRobot = false;
        bool pressWasShip = false;
        bool pressWasWave = false;
        bool pressWasSwing = false;
        bool trackedHeld = false;
    };

    std::array<PlayerBuffer, 2> s_players;
    PlayLayer* s_playLayer = nullptr;
    PlayerObject* s_simGhost = nullptr;
    bool s_simulating = false;
    bool s_simDied = false;
    bool s_unsupportedInteraction = false;
    bool s_injecting = false;
    float s_tickDuration = TICK_SECONDS;

    struct ObjectRollback {
        GameObject* object = nullptr;
        bool groupDisabled = false;
        bool activated = false;
        EffectGameObject* effect = nullptr;
        bool effectActivated = false;
        bool effectP1 = false;
        bool effectP2 = false;
        bool animationTriggered = false;
    };
    std::vector<ObjectRollback> s_objectRollback;
    std::unordered_set<GameObject*> s_recordedObjects;
    std::unordered_set<GameObject*> s_simTouchedObjects;
    std::unordered_set<GameObject*> s_simDisabledObjects;
    std::unordered_set<GameObject*> s_simEnabledObjects;
    std::unordered_set<GameObject*> s_collisionSeen;

    struct GroupToggleRollback {
        int group = 0;
        bool active = false;
    };
    std::vector<GroupToggleRollback> s_groupToggleRollback;
    std::unordered_set<int> s_recordedGroupToggles;

    void rememberGroupToggle(GJBaseGameLayer* layer, int group) {
        if (!layer || !layer->m_effectManager || group < 0) return;
        if (!s_recordedGroupToggles.insert(group).second) return;
        s_groupToggleRollback.push_back({ group, layer->m_effectManager->isGroupEnabled(group) });
    }

    void rememberObject(GameObject* object) {
        if (!object || !s_recordedObjects.insert(object).second) return;
        ObjectRollback state;
        state.object = object;
        state.groupDisabled = object->m_isGroupDisabled;
        state.activated = object->m_isActivated;
        if (auto* effect = typeinfo_cast<EffectGameObject*>(object)) {
            state.effect = effect;
            state.effectActivated = effect->m_activated;
            state.effectP1 = effect->m_activatedByPlayer1;
            state.effectP2 = effect->m_activatedByPlayer2;
            state.animationTriggered = effect->m_animationTriggered;
        }
        s_objectRollback.push_back(state);
    }

    void restorePredictionWorld() {
        for (auto it = s_objectRollback.rbegin(); it != s_objectRollback.rend(); ++it) {
            auto& state = *it;
            if (!state.object) continue;
            state.object->m_isGroupDisabled = state.groupDisabled;
            state.object->m_isActivated = state.activated;
            if (state.effect) {
                state.effect->m_activated = state.effectActivated;
                state.effect->m_activatedByPlayer1 = state.effectP1;
                state.effect->m_activatedByPlayer2 = state.effectP2;
                state.effect->m_animationTriggered = state.animationTriggered;
            }
        }
        s_objectRollback.clear();
        s_recordedObjects.clear();
        if (s_playLayer && s_playLayer->m_effectManager) {
            for (auto it = s_groupToggleRollback.rbegin(); it != s_groupToggleRollback.rend(); ++it) {
                s_playLayer->m_effectManager->toggleGroup(it->group, it->active);
            }
        }
        s_groupToggleRollback.clear();
        s_recordedGroupToggles.clear();
        s_simTouchedObjects.clear();
        s_simDisabledObjects.clear();
        s_simEnabledObjects.clear();
        s_collisionSeen.clear();
    }

    void setSimulatedGroupState(GJBaseGameLayer* layer, int group, bool active) {
        if (!layer || group <= 0) return;
        rememberGroupToggle(layer, group);
        if (layer->m_effectManager) {
            layer->m_effectManager->toggleGroup(group, active);
        }
        auto* objects = layer->getGroup(group);
        if (!objects) return;
        for (auto object : geode::cocos::CCArrayExt<GameObject, false>(objects)) {
            rememberObject(object);
            object->m_isGroupDisabled = !active;
            if (active) {
                s_simDisabledObjects.erase(object);
                s_simEnabledObjects.insert(object);
            }
            else {
                s_simEnabledObjects.erase(object);
                s_simDisabledObjects.insert(object);
            }
        }
    }

    int configuredLimit() {
        return std::clamp(
            Mod::get()->getSavedValue<int>(SAVED_LIMIT_KEY, DEFAULT_LIMIT_TICKS),
            0,
            MAX_LIMIT_TICKS
        );
    }

    void saveLimit(int ticks) {
        Mod::get()->setSavedValue(SAVED_LIMIT_KEY, std::clamp(ticks, 0, MAX_LIMIT_TICKS));
    }

    std::string groupedNumber(int value) {
        auto raw = std::to_string(value);
        for (int i = static_cast<int>(raw.size()) - 3; i > 0; i -= 3) {
            raw.insert(static_cast<size_t>(i), ",");
        }
        return raw;
    }

    bool isGhost(PlayerObject* player) {
        return player && (player == s_players[0].ghost || player == s_players[1].ghost);
    }

    int playerIndex(PlayerObject* player, PlayLayer* layer) {
        if (!player || !layer) return -1;
        if (player == layer->m_player1) return 0;
        if (player == layer->m_player2) return 1;
        return -1;
    }

    void clearPlayerBuffer(PlayerBuffer& state) {
        state.inputs.clear();
        state.activeShift = 0;
        state.tickRemainder = 0.f;
        state.tick = 0;
        state.pressStartTick = -1;
        state.tapHoldTicks = 12;
        state.robotHoldTicks = 12;
        state.shipHoldTicks = 18;
        state.waveHoldTicks = 12;
        state.swingHoldTicks = 12;
        state.pressWasRobot = false;
        state.pressWasShip = false;
        state.pressWasWave = false;
        state.pressWasSwing = false;
        state.trackedHeld = false;
    }

    void cancelQueuedInputs(PlayLayer* layer, PlayerObject* player, int index) {
        if (index < 0 || index >= static_cast<int>(s_players.size())) return;
        auto& state = s_players[index];

        // If the queued press already reached GD, make sure it is released before
        // clearing the pending release. This avoids a held button leaking into retry.
        if (layer && player) {
            for (int button = static_cast<int>(PlayerButton::Jump);
                button <= static_cast<int>(PlayerButton::Right); ++button) {
                auto held = player->m_holdingButtons.find(button);
                if (held != player->m_holdingButtons.end() && held->second) {
                    s_injecting = true;
                    layer->handleButton(false, button, index == 0);
                    s_injecting = false;
                }
            }
        }

        state.inputs.clear();
        state.activeShift = 0;
        state.trackedHeld = false;
        state.pressStartTick = -1;
    }

    void destroyGhosts() {
        s_simGhost = nullptr;
        s_simulating = false;
        for (auto& state : s_players) {
            clearPlayerBuffer(state);
            if (state.ghostLayer) {
                if (state.ghost) {
                    state.ghostLayer->removeChild(state.ghost, true);
                }
                state.ghost = nullptr;
                state.ghostLayer->release();
                state.ghostLayer = nullptr;
            }
        }
        s_playLayer = nullptr;
    }

    PlayerObject* createGhost(PlayLayer* layer, int index) {
        auto& state = s_players[index];
        if (state.ghost) return state.ghost;

        state.ghostLayer = CCLayer::create();
        if (!state.ghostLayer) return nullptr;
        state.ghostLayer->retain();

        // The detached layer is deliberately never attached to the visible scene.
        // It gives the clone a valid parent without drawing or scheduling it.
        state.ghost = PlayerObject::create(1, 1, layer, state.ghostLayer, index == 1);
        if (!state.ghost) {
            state.ghostLayer->release();
            state.ghostLayer = nullptr;
            return nullptr;
        }
        state.ghostLayer->addChild(state.ghost);
        state.ghost->setVisible(false);
        state.ghost->setOpacity(0);
        return state.ghost;
    }

    void ensureGhosts(PlayLayer* layer) {
        if (!layer) return;
        if (s_playLayer != layer) {
            destroyGhosts();
            s_playLayer = layer;
        }
        createGhost(layer, 0);
        if (layer->m_player2) createGhost(layer, 1);
    }

    void seedGhost(PlayerObject* ghost, PlayerObject* real) {
        ghost->setVisible(false);
        ghost->copyAttributes(real);
        // Do not inherit ring-contact history from the live player: each
        // prediction starts with a clean collision-contact set.
        ghost->resetTouchedRings(true);
        ghost->setPosition(real->getPosition());
        ghost->setRotation(real->getRotation());
        ghost->setScale(real->getScale());
        ghost->setContentSize(real->getContentSize());

        // Preserve the mode-specific physics state that is not contained in the
        // generic attribute copy: vehicle flags, directional state, velocity,
        // gravity, slope contact, dash state and temporary physics modifiers.
        ghost->m_isShip = real->m_isShip;
        ghost->m_isBird = real->m_isBird;
        ghost->m_isBall = real->m_isBall;
        ghost->m_isDart = real->m_isDart;
        ghost->m_isRobot = real->m_isRobot;
        ghost->m_isSpider = real->m_isSpider;
        ghost->m_isSwing = real->m_isSwing;
        ghost->m_isPlatformer = real->m_isPlatformer;
        ghost->m_platformerXVelocity = real->m_platformerXVelocity;
        ghost->m_platformerVelocityRelated = real->m_platformerVelocityRelated;
        ghost->m_platformerMovingLeft = real->m_platformerMovingLeft;
        ghost->m_platformerMovingRight = real->m_platformerMovingRight;
        ghost->m_changedDirectionsTime = real->m_changedDirectionsTime;
        ghost->m_maybeChangedDirectionAngle = real->m_maybeChangedDirectionAngle;
        ghost->m_maybeGoingCorrectSlopeDirection = real->m_maybeGoingCorrectSlopeDirection;
        ghost->m_isGoingLeft = real->m_isGoingLeft;
        ghost->m_isSideways = real->m_isSideways;
        ghost->m_reverseSync = real->m_reverseSync;
        ghost->m_reverseRelated = real->m_reverseRelated;
        ghost->m_maybeReverseSpeed = real->m_maybeReverseSpeed;
        ghost->m_maybeReverseAcceleration = real->m_maybeReverseAcceleration;
        ghost->m_speedMultiplier = real->m_speedMultiplier;
        ghost->m_gravity = real->m_gravity;
        ghost->m_yStart = real->m_yStart;
        ghost->m_totalTime = real->m_totalTime;
        ghost->m_lastPosition = real->m_lastPosition;

        ghost->m_yVelocity = real->m_yVelocity;
        ghost->m_fallSpeed = real->m_fallSpeed;
        ghost->m_yVelocityBeforeSlope = real->m_yVelocityBeforeSlope;
        ghost->m_yVelocityRelated = real->m_yVelocityRelated;
        ghost->m_yVelocityRelated3 = real->m_yVelocityRelated3;
        ghost->m_groundYVelocity = real->m_groundYVelocity;
        ghost->m_accelerationOrSpeed = real->m_accelerationOrSpeed;
        ghost->m_isAccelerating = real->m_isAccelerating;
        ghost->m_maybeIsFalling = real->m_maybeIsFalling;

        ghost->m_isOnGround2 = real->m_isOnGround2;
        ghost->m_isOnGround3 = real->m_isOnGround3;
        ghost->m_isOnGround4 = real->m_isOnGround4;
        ghost->m_stateOnGround = real->m_stateOnGround;
        ghost->m_lastLandTime = real->m_lastLandTime;
        ghost->m_lastJumpTime = real->m_lastJumpTime;
        ghost->m_lastFlipTime = real->m_lastFlipTime;
        ghost->m_lastSpiderFlipTime = real->m_lastSpiderFlipTime;
        ghost->m_lastGroundedPos = real->m_lastGroundedPos;

        ghost->m_isOnSlope = real->m_isOnSlope;
        ghost->m_wasOnSlope = real->m_wasOnSlope;
        ghost->m_slopeVelocity = real->m_slopeVelocity;
        ghost->m_maybeUpsideDownSlope = real->m_maybeUpsideDownSlope;
        ghost->m_currentSlope = real->m_currentSlope;
        ghost->m_currentSlope2 = real->m_currentSlope2;
        ghost->m_currentPotentialSlope = real->m_currentPotentialSlope;
        ghost->m_isCollidingWithSlope = real->m_isCollidingWithSlope;
        ghost->m_collidingWithSlopeId = real->m_collidingWithSlopeId;
        ghost->m_slopeFlipGravityRelated = real->m_slopeFlipGravityRelated;
        ghost->m_slopeAngle = real->m_slopeAngle;
        ghost->m_slopeAngleRadians = real->m_slopeAngleRadians;
        ghost->m_slopeRotation = real->m_slopeRotation;
        ghost->m_currentSlopeYVelocity = real->m_currentSlopeYVelocity;
        ghost->m_isCurrentSlopeTop = real->m_isCurrentSlopeTop;
        ghost->m_slopeStartTime = real->m_slopeStartTime;
        ghost->m_slopeEndTime = real->m_slopeEndTime;
        ghost->m_slopeSlidingMaybeRotated = real->m_slopeSlidingMaybeRotated;

        ghost->m_lastGroundObject = real->m_lastGroundObject;
        ghost->m_preLastGroundObject = real->m_preLastGroundObject;
        ghost->m_maybeLastGroundObject = real->m_maybeLastGroundObject;
        ghost->m_collidedObject = real->m_collidedObject;
        ghost->m_objectSnappedTo = real->m_objectSnappedTo;
        ghost->m_snapDistance = real->m_snapDistance;
        ghost->m_lastCollisionBottom = real->m_lastCollisionBottom;
        ghost->m_lastCollisionTop = real->m_lastCollisionTop;
        ghost->m_lastCollisionLeft = real->m_lastCollisionLeft;
        ghost->m_lastCollisionRight = real->m_lastCollisionRight;
        ghost->m_collidedTopMinY = real->m_collidedTopMinY;
        ghost->m_collidedBottomMaxY = real->m_collidedBottomMaxY;
        ghost->m_collidedLeftMaxX = real->m_collidedLeftMaxX;
        ghost->m_collidedRightMinX = real->m_collidedRightMinX;

        ghost->m_dashX = real->m_dashX;
        ghost->m_dashY = real->m_dashY;
        ghost->m_dashAngle = real->m_dashAngle;
        ghost->m_dashStartTime = real->m_dashStartTime;
        ghost->m_dashRing = real->m_dashRing;
        ghost->m_dashFireFrame = real->m_dashFireFrame;

        ghost->m_stateForce = real->m_stateForce;
        ghost->m_stateForceVector = real->m_stateForceVector;
        ghost->m_affectedByForces = real->m_affectedByForces;
        ghost->m_stateBoostX = real->m_stateBoostX;
        ghost->m_stateBoostY = real->m_stateBoostY;
        ghost->m_stateNoStickX = real->m_stateNoStickX;
        ghost->m_stateNoStickY = real->m_stateNoStickY;
        ghost->m_stateHitHead = real->m_stateHitHead;
        ghost->m_stateFlipGravity = real->m_stateFlipGravity;
        ghost->m_stateNoAutoJump = real->m_stateNoAutoJump;
        ghost->m_stateScale = real->m_stateScale;
        ghost->m_maybeStateForce2 = real->m_maybeStateForce2;

        // copyAttributes carries the movement state; these fields explicitly restore
        // the held-button state, which determines ship/wave/swing movement and releases.
        ghost->m_holdingButtons = real->m_holdingButtons;
        ghost->m_jumpBuffered = real->m_jumpBuffered;
        ghost->m_isDead = false;
        ghost->setVisible(false);
        ghost->setOpacity(0);
    }

    bool ghostStep(PlayLayer* layer, PlayerObject* ghost, uint32_t& work,
        uint32_t maxWork, std::chrono::steady_clock::time_point deadline) {
        if (++work > maxWork || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }

        ghost->update(s_tickDuration);
        ghost->updateRotation(s_tickDuration);
        if (!s_simDied && !ghost->m_isDead) {
            layer->checkCollisions(ghost, s_tickDuration, false);
        }
        return !s_simDied && !ghost->m_isDead && !s_unsupportedInteraction;
    }

    int normalizedButton(int button) {
        // Match GD's public button-ID normalization, including P2 variants.
        switch (button) {
            case 2: case 7: return 2;
            case 3: case 8: return 3;
            case 4: case 9: return 4;
            case 5: case 10: return 5;
            default: return 1;
        }
    }

    bool canPredictButton(PlayLayer* layer, int button) {
        const int normalized = normalizedButton(button);
        if (normalized == 1) return true;
        // Platformer has directional inputs in addition to jump. The same player
        // simulation can evaluate those controls, so they share the exact slider.
        return layer && layer->m_isPlatformer && (normalized == 2 || normalized == 3);
    }

    PlayerButton asPlayerButton(int button) {
        switch (normalizedButton(button)) {
            case 2: return PlayerButton::Left;
            case 3: return PlayerButton::Right;
            default: return PlayerButton::Jump;
        }
    }

    enum class TestResult {
        Survives,
        Dies,
        BudgetExceeded,
        Unsupported
    };

    TestResult testDelay(
        PlayLayer* layer,
        PlayerObject* real,
        PlayerObject* ghost,
        bool down,
        int button,
        int delay,
        int horizon,
        int releaseAfter,
        uint32_t& work,
        uint32_t maxWork,
        std::chrono::steady_clock::time_point deadline
    ) {
        restorePredictionWorld();
        s_objectRollback.clear();
        s_recordedObjects.clear();
        seedGhost(ghost, real);
        s_simGhost = ghost;
        s_simDied = false;
        s_unsupportedInteraction = false;
        s_simulating = true;

        for (int tick = 0; tick < delay; ++tick) {
            if (!ghostStep(layer, ghost, work, maxWork, deadline)) {
                s_simulating = false;
                restorePredictionWorld();
                if (s_unsupportedInteraction) return TestResult::Unsupported;
                if (std::chrono::steady_clock::now() >= deadline || work > maxWork) return TestResult::BudgetExceeded;
                return TestResult::Dies;
            }
        }

        const auto playerButton = asPlayerButton(button);
        if (down) ghost->pushButton(playerButton);
        else ghost->releaseButton(playerButton);

        int airborneTicks = 0;
        int groundedTicks = 0;
        const bool landingBased = !ghost->m_isShip && !ghost->m_isDart && !ghost->m_isSwing;
        const int landingConfirm = ghost->m_isSpider ? 12 : (ghost->m_isBall || ghost->m_isBird || ghost->m_isRobot ? 8 : 3);

        for (int tick = 0; tick < horizon; ++tick) {
            if (down && releaseAfter >= 0 && tick == releaseAfter) {
                ghost->releaseButton(asPlayerButton(button));
            }
            if (!ghostStep(layer, ghost, work, maxWork, deadline)) {
                s_simulating = false;
                restorePredictionWorld();
                if (s_unsupportedInteraction) return TestResult::Unsupported;
                if (std::chrono::steady_clock::now() >= deadline || work > maxWork) return TestResult::BudgetExceeded;
                return TestResult::Dies;
            }
            if (!ghost->m_isOnGround) {
                ++airborneTicks;
                groundedTicks = 0;
            }
            else if (airborneTicks >= (ghost->m_isBall ? 2 : 6)) {
                ++groundedTicks;
                if (landingBased && (releaseAfter < 0 || tick >= releaseAfter) && groundedTicks >= landingConfirm) {
                    s_simulating = false;
                    restorePredictionWorld();
                    return TestResult::Survives;
                }
            }
        }

        s_simulating = false;
        restorePredictionWorld();
        return TestResult::Survives;
    }

    // Returns a positive tick count only when the cloned player survives after
    // applying the same input later. Zero means "send it normally".
    int findSafeDelay(PlayLayer* layer, PlayerObject* real, int index, bool down, int button, int releaseAfter) {
        const int limit = configuredLimit();
        if (!layer || !real || limit <= 0 || real->m_isDead || !canPredictButton(layer, button)) {
            return 0;
        }

        auto* ghost = createGhost(layer, index);
        if (!ghost) return 0;

        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + SEARCH_BUDGET;
        // Reserve enough work for a complete ascending scan at modest
        // settings: each candidate runs its delay prefix plus one full horizon.
        // Large user-selected horizons remain bounded to avoid freezing GD.
        const uint64_t requestedWork =
            static_cast<uint64_t>(limit) * (3ull * static_cast<uint64_t>(limit) + 3ull) / 2ull;
        const uint32_t maxWork = static_cast<uint32_t>(std::min<uint64_t>(
            MAX_SIMULATION_STEPS,
            std::max<uint64_t>(4096ull, requestedWork)
        ));
        uint32_t work = 0;

        // First check the input exactly as requested. The horizon is the same
        // shared value as the delay limit, regardless of the current game mode.
        auto immediate = testDelay(layer, real, ghost, down, button, 0, limit, releaseAfter, work, maxWork, deadline);
        if (immediate == TestResult::Survives) return 0;
        if (immediate == TestResult::BudgetExceeded || immediate == TestResult::Unsupported) return 0;

        // Search from the smallest correction upward, keeping the first working
        // offset. Each candidate starts from a fresh copy of the real player's state.
        for (int delay = 1; delay <= limit; ++delay) {
            auto result = testDelay(layer, real, ghost, down, button, delay, limit, releaseAfter, work, maxWork, deadline);
            if (result == TestResult::Survives) return delay;
            if (result == TestResult::BudgetExceeded || result == TestResult::Unsupported) break;
        }
        return 0;
    }

    void enqueueInput(PlayerBuffer& state, bool down, int button, bool isPlayer1, int newShift = 0) {
        if (newShift > 0) {
            state.activeShift = std::max(state.activeShift, newShift);
        }
        const auto due = state.tick + static_cast<uint64_t>(state.activeShift);
        state.inputs.push_back({ due, down, button, isPlayer1 });
    }

    void deliverInputs(PlayLayer* layer, int index) {
        auto& state = s_players[index];
        while (!state.inputs.empty() && state.inputs.front().dueTick <= state.tick) {
            auto input = state.inputs.front();
            state.inputs.pop_front();

            s_injecting = true;
            layer->handleButton(input.down, input.button, input.isPlayer1);
            s_injecting = false;

            // The delayed hold is complete only when its matching release reaches
            // the game. Until then, later inputs retain the same offset.
            if (!input.down && state.inputs.empty()) {
                state.activeShift = 0;
            }
        }
    }
}

// One small panel and one persisted setting. The same number drives both the
// maximum delay search and the future simulation horizon.
class CWBFSettingsPopup final : public Popup {
protected:
    CCLabelBMFont* m_valueLabel = nullptr;
    CCLabelBMFont* m_framesLabel = nullptr;

    void refreshLabels(int ticks) {
        if (m_valueLabel) {
            m_valueLabel->setString((groupedNumber(ticks) + " ticks").c_str());
        }
        if (m_framesLabel) {
            m_framesLabel->setString((groupedNumber(ticks / 4) + " frames at 60 FPS").c_str());
        }
    }

    bool init() {
        if (!Popup::init(360.f, 190.f)) return false;
        setTitle("CLICK-WAY-BEFORE-FRAMES", "goldFont.fnt", 0.62f);

        auto size = m_mainLayer->getContentSize();
        auto centerX = size.width * 0.5f;

        auto* caption = CCLabelBMFont::create("MAXIMUM EARLY-CLICK DELAY", "bigFont.fnt");
        caption->setScale(0.48f);
        caption->setPosition({ centerX, size.height - 65.f });
        m_mainLayer->addChild(caption);

        m_valueLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_valueLabel->setScale(0.72f);
        m_valueLabel->setPosition({ centerX, size.height - 91.f });
        m_mainLayer->addChild(m_valueLabel);

        auto* slider = SliderNode::create([this](SliderNode*, float value) {
            int ticks = std::clamp(static_cast<int>(std::lround(value)), 0, MAX_LIMIT_TICKS);
            saveLimit(ticks);
            refreshLabels(ticks);
        });
        if (!slider) return false;

        slider->setMin(0.f);
        slider->setMax(static_cast<float>(MAX_LIMIT_TICKS));
        slider->setSnapStep(1.f);
        slider->setValue(static_cast<float>(configuredLimit()));
        slider->setScale(1.45f);
        slider->setPosition({ centerX, size.height - 127.f });
        m_mainLayer->addChild(slider);

        m_framesLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_framesLabel->setScale(0.46f);
        m_framesLabel->setPosition({ centerX, 27.f });
        m_mainLayer->addChild(m_framesLabel);

        refreshLabels(configuredLimit());
        return true;
    }

public:
    static CWBFSettingsPopup* create() {
        auto* popup = new CWBFSettingsPopup();
        if (popup && popup->init()) {
            popup->autorelease();
            return popup;
        }
        CC_SAFE_DELETE(popup);
        return nullptr;
    }
};

// Player clones are destroyed through the same game path as real players.
// This hook turns that into a simulation result instead of a real death.
class $modify(CWBF_PlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        ensureGhosts(this);
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (s_simulating && isGhost(player)) {
            s_simDied = true;
            player->m_isDead = true;
            return;
        }
        const int index = playerIndex(player, this);
        if (index >= 0) cancelQueuedInputs(this, player, index);
        PlayLayer::destroyPlayer(player, object);
    }

    void resetLevel() {
        cancelQueuedInputs(this, m_player1, 0);
        cancelQueuedInputs(this, m_player2, 1);
        for (auto& state : s_players) clearPlayerBuffer(state);
        PlayLayer::resetLevel();
    }

    void onQuit() {
        destroyGhosts();
        PlayLayer::onQuit();
    }
};

class $modify(CWBF_PlayerObject, PlayerObject) {
    void update(float dt) {
        if (!s_simulating && !isGhost(this)) {
            auto* layer = GameManager::get()->m_playLayer;
            const int index = playerIndex(this, layer);
            if (index >= 0) {
                auto& state = s_players[index];
                if (dt > 0.0001f && dt < 1.f) s_tickDuration = dt;
                state.tickRemainder += std::max(0.f, dt) / s_tickDuration;
                while (state.tickRemainder >= 1.f) {
                    state.tickRemainder -= 1.f;
                    ++state.tick;
                    deliverInputs(layer, index);
                }
            }
        }
        PlayerObject::update(dt);
    }
};

class $modify(CWBF_BaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (s_simulating || s_injecting) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto* layer = GameManager::get()->m_playLayer;
        if (!layer || layer->m_isPaused || layer->m_playerDied) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        ensureGhosts(layer);
        const int index = isPlayer1 ? 0 : 1;
        auto* real = isPlayer1 ? layer->m_player1 : layer->m_player2;
        auto& state = s_players[index];

        if (!real || !state.ghost || !canPredictButton(layer, button)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Learn the user's actual tap length from real input edges. A press trial
        // then includes a representative release instead of assuming every button
        // remains down for the entire prediction horizon.
        if (down && !state.trackedHeld) {
            state.pressStartTick = static_cast<int>(state.tick);
            state.pressWasRobot = real->m_isRobot;
            state.pressWasShip = real->m_isShip;
            state.pressWasWave = real->m_isDart;
            state.pressWasSwing = real->m_isSwing;
            state.trackedHeld = true;
        }
        else if (!down && state.trackedHeld && state.pressStartTick >= 0) {
            const int duration = std::clamp(static_cast<int>(state.tick) - state.pressStartTick, 1, 240);
            int* estimate = &state.tapHoldTicks;
            if (state.pressWasRobot) estimate = &state.robotHoldTicks;
            else if (state.pressWasShip) estimate = &state.shipHoldTicks;
            else if (state.pressWasWave) estimate = &state.waveHoldTicks;
            else if (state.pressWasSwing) estimate = &state.swingHoldTicks;
            *estimate = std::clamp((*estimate * 3 + duration) / 4, 1, 240);
            state.pressStartTick = -1;
            state.trackedHeld = false;
        }

        // Once an early input is queued, keep the same offset for its matching
        // release and any inputs made while that hold is pending.
        if (state.activeShift > 0) {
            enqueueInput(state, down, button, isPlayer1);
            return;
        }

        int releaseAfter = -1;
        if (down) {
            if (real->m_isRobot) releaseAfter = state.robotHoldTicks;
            else if (real->m_isShip) releaseAfter = state.shipHoldTicks;
            else if (real->m_isDart) releaseAfter = state.waveHoldTicks;
            else if (real->m_isSwing) releaseAfter = state.swingHoldTicks;
            else releaseAfter = state.tapHoldTicks;
        }
        const int delay = findSafeDelay(layer, real, index, down, button, releaseAfter);
        if (delay > 0) {
            enqueueInput(state, down, button, isPlayer1, delay);
            return;
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!s_simulating || !isGhost(player) || !objects) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }

        static gd::vector<GameObject*> collisionObjects;
        collisionObjects.clear();
        s_collisionSeen.clear();
        if (collisionObjects.capacity() < static_cast<size_t>(objectCount) + s_simEnabledObjects.size()) {
            collisionObjects.reserve(static_cast<size_t>(objectCount) + s_simEnabledObjects.size());
        }
        s_collisionSeen.reserve(static_cast<size_t>(objectCount) + s_simEnabledObjects.size());

        const auto playerRect = player->getObjectRect();
        for (int i = 0; i < objectCount; ++i) {
            auto* object = (*objects)[i];
            if (!object || s_simDisabledObjects.contains(object)) continue;
            s_collisionSeen.insert(object);

            auto type = object->m_objectType;
            const int objectID = object->m_objectID;
            const bool speedPortal =
                objectID == 200 || objectID == 201 || objectID == 202 ||
                objectID == 203 || objectID == 1334;
            const bool unsupportedPortal =
                type == GameObjectType::DualPortal ||
                type == GameObjectType::SoloPortal ||
                type == GameObjectType::NormalMirrorPortal ||
                type == GameObjectType::InverseMirrorPortal;
            const bool specialPortal = speedPortal || unsupportedPortal ||
                type == GameObjectType::RegularSizePortal ||
                type == GameObjectType::MiniSizePortal ||
                type == GameObjectType::TeleportPortal;

            if (!specialPortal || !playerRect.intersectsRect(object->getObjectRect())) {
                collisionObjects.push_back(object);
                continue;
            }

            // These portals are applied to the ghost itself and removed from the
            // normal collision pass so GD cannot apply the same portal a second time.
            if (!s_simTouchedObjects.insert(object).second) continue;

            if (unsupportedPortal) {
                s_unsupportedInteraction = true;
                continue;
            }

            if (speedPortal) {
                switch (objectID) {
                    case 200: player->m_playerSpeed = 0.7f; break;
                    case 201: player->m_playerSpeed = 0.9f; break;
                    case 202: player->m_playerSpeed = 1.1f; break;
                    case 203: player->m_playerSpeed = 1.3f; break;
                    case 1334: player->m_playerSpeed = 1.6f; break;
                }
                continue;
            }

            if (type == GameObjectType::RegularSizePortal || type == GameObjectType::MiniSizePortal) {
                const bool mini = type == GameObjectType::MiniSizePortal;
                if ((player->m_vehicleSize < 1.f) != mini) {
                    player->togglePlayerScale(mini, true);
                }
                continue;
            }

            auto* portal = static_cast<TeleportPortalObject*>(object);
            if (object->m_objectID == 749) {
                continue; // orange endpoint is only a destination
            }
            if (object->m_objectID != 747) {
                s_unsupportedInteraction = true;
                continue;
            }
            if (portal->m_staticForceEnabled || portal->m_redirectForceEnabled ||
                portal->m_redirectDash || portal->m_gravityMode != 0 || portal->m_teleportEase) {
                // Force redirects, gravity changes and eased teleports depend on more
                // than a simple paired-portal offset; do not score those runs as safe.
                s_unsupportedInteraction = true;
                continue;
            }

            double dx = 0.0;
            double dy = portal->m_teleportYOffset;
            if (portal->m_orangePortal && portal->m_orangePortal != portal) {
                dx = portal->m_orangePortal->m_positionX - portal->m_positionX;
                dy = portal->m_orangePortal->m_positionY - portal->m_positionY;
            }
            if (portal->m_ignoreX) dx = 0.0;
            if (portal->m_ignoreY) dy = 0.0;

            const auto shift = CCPoint { static_cast<float>(dx), static_cast<float>(dy) };
            player->setPosition(player->getPosition() + shift);
            player->m_position = player->m_position + shift;
            player->m_lastPosition = player->m_lastPosition + shift;
            player->m_isOnGround = false;
            player->m_isOnGround2 = false;
            player->playerTeleported();
        }

        // Groups enabled during a prediction are absent from the game's pre-filtered
        // collision vector. Reinsert nearby members so the ghost sees newly enabled
        // hazards and solids on its next physics step.
        const auto ghostPosition = player->getPosition();
        for (auto* object : s_simEnabledObjects) {
            if (!object || s_simDisabledObjects.contains(object) || s_collisionSeen.contains(object)) continue;
            if (std::abs(object->getPositionX() - ghostPosition.x) > 220.f ||
                std::abs(object->getPositionY() - ghostPosition.y) > 220.f) {
                continue;
            }
            s_collisionSeen.insert(object);
            collisionObjects.push_back(object);
        }

        GJBaseGameLayer::collisionCheckObjects(
            player, &collisionObjects, static_cast<int>(collisionObjects.size()), dt
        );
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (s_simulating && isGhost(player)) {
            rememberObject(object);
            return true;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (s_simulating && isGhost(player)) {
            rememberObject(ring);
            GJBaseGameLayer::playerTouchedRing(player, ring);
            auto held = player->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
            if (held == player->m_holdingButtons.end() || !held->second) {
                player->pushButton(PlayerButton::Jump);
            }
            return;
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (s_simulating && isGhost(player)) {
            player->flipGravity(flip, true);
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    void playerWillSwitchMode(PlayerObject* player, GameObject* portal) {
        if (s_simulating && isGhost(player)) {
            if (!portal) return;
            auto mode = portal->m_objectType;
            if (player->m_isShip) player->toggleFlyMode(false, true);
            if (player->m_isBird) player->toggleBirdMode(false, true);
            if (player->m_isDart) player->toggleDartMode(false, true);
            if (player->m_isBall) player->toggleRollMode(false, true);
            if (player->m_isRobot) player->toggleRobotMode(false, true);
            if (player->m_isSpider) player->toggleSpiderMode(false, true);
            if (player->m_isSwing) player->toggleSwingMode(false, true);

            switch (mode) {
                case GameObjectType::ShipPortal: player->toggleFlyMode(true, true); break;
                case GameObjectType::UfoPortal: player->toggleBirdMode(true, true); break;
                case GameObjectType::WavePortal: player->toggleDartMode(true, true); break;
                case GameObjectType::BallPortal: player->toggleRollMode(true, true); break;
                case GameObjectType::RobotPortal: player->toggleRobotMode(true, true); break;
                case GameObjectType::SpiderPortal: player->toggleSpiderMode(true, true); break;
                case GameObjectType::SwingPortal: player->toggleSwingMode(true, true); break;
                case GameObjectType::CubePortal: break;
                default: s_unsupportedInteraction = true; break;
            }
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(player, portal);
    }

    void toggleGroupTriggered(int group, bool activate, gd::vector<int> const& remapKeys,
        int triggerID, int controlID) {
        if (s_simulating) {
            if (!remapKeys.empty()) {
                s_unsupportedInteraction = true;
                return;
            }
            setSimulatedGroupState(this, group, activate);
            return;
        }
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerID, controlID);
    }

    void toggleGroup(int group, bool activate) {
        if (s_simulating) {
            setSimulatedGroupState(this, group, activate);
            return;
        }
        GJBaseGameLayer::toggleGroup(group, activate);
    }

    void spawnGroup(int group, bool ordered, double delay, gd::vector<int> const& remapKeys,
        int triggerID, int controlID) {
        if (s_simulating) {
            if (ordered || delay > 0.0 || !remapKeys.empty()) {
                s_unsupportedInteraction = true;
                return;
            }
            setSimulatedGroupState(this, group, true);
            return;
        }
        GJBaseGameLayer::spawnGroup(group, ordered, delay, remapKeys, triggerID, controlID);
    }

    void moveAreaObject(GameObject* object, float dx, float dy) {
        if (s_simulating) {
            s_unsupportedInteraction = true;
            return;
        }
        GJBaseGameLayer::moveAreaObject(object, dx, dy);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (s_simulating) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void pickupItem(EffectGameObject* object) {
        if (s_simulating) return;
        GJBaseGameLayer::pickupItem(object);
    }

    void updateCounters(int itemID, int value) {
        if (s_simulating) return;
        GJBaseGameLayer::updateCounters(itemID, value);
    }
};


// Enhanced move/rotate triggers enqueue commands on the live game layer. Their
// dispatch functions are inline in GD 2.2081 and cannot be patched directly.
// Intercept the effect callback before it can write those actions into live state.
class $modify(CWBF_EffectGameObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (s_simulating && typeinfo_cast<EnhancedTriggerObject*>(this)) {
            s_unsupportedInteraction = true;
            return;
        }
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

// Existing pause-menu entry point, with the new project's branding.
class $modify(CWBF_PauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        if (getChildByID("cwbf-pause-button-menu"_spr)) return;

        auto* icon = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        if (!icon) icon = CCSprite::createWithSpriteFrameName("GJ_infoBtn_001.png");
        if (!icon) {
            log::error("Click-Way-Before-Frames: couldn't create pause-menu icon");
            return;
        }

        icon->setScale(0.58f);
        auto* buttonSprite = CircleButtonSprite::create(
            icon, CircleBaseColor::DarkPurple, CircleBaseSize::Medium
        );
        auto* button = CCMenuItemSpriteExtra::create(
            buttonSprite, this, menu_selector(CWBF_PauseLayer::onOpenCWBF)
        );
        button->setID("cwbf-open-panel"_spr);

        auto* menu = CCMenu::create();
        menu->addChild(button);
        auto win = CCDirector::get()->getWinSize();
        menu->setPosition({ win.width - 40.f, 40.f });
        menu->setID("cwbf-pause-button-menu"_spr);
        addChild(menu, 20);
    }

    void onOpenCWBF(CCObject*) {
        if (auto* popup = CWBFSettingsPopup::create()) popup->show();
    }
};
