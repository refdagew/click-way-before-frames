#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/SliderNode.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>

using namespace geode::prelude;

namespace {
    constexpr int DEFAULT_LIMIT_TICKS = 16384;
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
        bool player2 = false;
    };

    struct PlayerBuffer {
        PlayerObject* ghost = nullptr;
        CCLayer* ghostLayer = nullptr;
        std::deque<QueuedInput> inputs;
        uint64_t tick = 0;
        float tickRemainder = 0.f;
        int activeShift = 0;
    };

    std::array<PlayerBuffer, 2> s_players;
    PlayLayer* s_playLayer = nullptr;
    PlayerObject* s_simGhost = nullptr;
    bool s_simulating = false;
    bool s_simDied = false;
    bool s_unsupportedInteraction = false;
    bool s_injecting = false;

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
        ghost->resetTouchedRings(true);
        ghost->copyAttributes(real);
        ghost->setPosition(real->getPosition());
        ghost->setRotation(real->getRotation());
        ghost->setScale(real->getScale());
        ghost->setContentSize(real->getContentSize());

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

        ghost->update(TICK_SECONDS);
        if (!s_simDied && !ghost->m_isDead) {
            layer->checkCollisions(ghost, TICK_SECONDS, false);
        }
        return !s_simDied && !ghost->m_isDead && !s_unsupportedInteraction;
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
        int delay,
        int horizon,
        uint32_t& work,
        uint32_t maxWork,
        std::chrono::steady_clock::time_point deadline
    ) {
        seedGhost(ghost, real);
        s_simGhost = ghost;
        s_simDied = false;
        s_unsupportedInteraction = false;
        s_simulating = true;

        for (int tick = 0; tick < delay; ++tick) {
            if (!ghostStep(layer, ghost, work, maxWork, deadline)) {
                s_simulating = false;
                if (s_unsupportedInteraction) return TestResult::Unsupported;
                if (std::chrono::steady_clock::now() >= deadline || work > maxWork) return TestResult::BudgetExceeded;
                return TestResult::Dies;
            }
        }

        if (down) ghost->pushButton(PlayerButton::Jump);
        else ghost->releaseButton(PlayerButton::Jump);

        for (int tick = 0; tick < horizon; ++tick) {
            if (!ghostStep(layer, ghost, work, maxWork, deadline)) {
                s_simulating = false;
                if (s_unsupportedInteraction) return TestResult::Unsupported;
                if (std::chrono::steady_clock::now() >= deadline || work > maxWork) return TestResult::BudgetExceeded;
                return TestResult::Dies;
            }
        }

        s_simulating = false;
        return TestResult::Survives;
    }

    // Returns a positive tick count only when the cloned player survives after
    // applying the same input later. Zero means "send it normally".
    int findSafeDelay(PlayLayer* layer, PlayerObject* real, int index, bool down) {
        const int limit = configuredLimit();
        if (!layer || !real || limit <= 0 || real->m_isDead || layer->m_isPlatformer) {
            return 0;
        }

        auto* ghost = createGhost(layer, index);
        if (!ghost) return 0;

        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + SEARCH_BUDGET;
        const uint32_t maxWork = std::min<uint32_t>(
            MAX_SIMULATION_STEPS,
            static_cast<uint32_t>(std::max(4096, limit * 8))
        );
        uint32_t work = 0;

        // First check the input exactly as requested. The horizon is the same
        // shared value as the delay limit, regardless of the current game mode.
        auto immediate = testDelay(layer, real, ghost, down, 0, limit, work, maxWork, deadline);
        if (immediate == TestResult::Survives) return 0;
        if (immediate == TestResult::BudgetExceeded || immediate == TestResult::Unsupported) return 0;

        // Search from the smallest correction upward, keeping the first working
        // offset. Each candidate starts from a fresh copy of the real player's state.
        for (int delay = 1; delay <= limit; ++delay) {
            auto result = testDelay(layer, real, ghost, down, delay, limit, work, maxWork, deadline);
            if (result == TestResult::Survives) return delay;
            if (result == TestResult::BudgetExceeded || result == TestResult::Unsupported) break;
        }
        return 0;
    }

    void enqueueInput(PlayerBuffer& state, bool down, bool player2, int newShift = 0) {
        if (newShift > 0) {
            state.activeShift = std::max(state.activeShift, newShift);
        }
        const auto due = state.tick + static_cast<uint64_t>(state.activeShift);
        state.inputs.push_back({ due, down, player2 });
    }

    void deliverInputs(PlayLayer* layer, int index) {
        auto& state = s_players[index];
        while (!state.inputs.empty() && state.inputs.front().dueTick <= state.tick) {
            auto input = state.inputs.front();
            state.inputs.pop_front();

            s_injecting = true;
            layer->handleButton(input.down, 1, input.player2);
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
        PlayLayer::destroyPlayer(player, object);
    }

    void resetLevel() {
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
                state.tickRemainder += std::max(0.f, dt) * 240.f;
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
        if (s_simulating || s_injecting || button != 1) {
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

        if (!real || !state.ghost || layer->m_isPlatformer) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Once an early input is queued, keep the same offset for its matching
        // release and any inputs made while that hold is pending.
        if (state.activeShift > 0) {
            enqueueInput(state, down, !isPlayer1);
            return;
        }

        const int delay = findSafeDelay(layer, real, index, down);
        if (delay > 0) {
            enqueueInput(state, down, !isPlayer1, delay);
            return;
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (s_simulating && isGhost(player)) {
            // This first engine intentionally declines to simulate object effects
            // (rings, pads, portals, triggers). We never let a speculative ghost
            // mutate the live level's shared objects.
            s_unsupportedInteraction = true;
            return false;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
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
