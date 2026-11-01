#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/SliderNode.hpp>

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace {
    constexpr int DEFAULT_LIMIT_TICKS = 16384;
    constexpr int MAX_LIMIT_TICKS = 16384;
    constexpr char const* SAVED_LIMIT_KEY = "shared-early-click-limit";

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
}

// One setting is shared by every mode. The prediction engine will consume
// this value for both its maximum delay and its forward simulation horizon.
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
        if (!Popup::init(360.f, 190.f)) {
            return false;
        }

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
        if (!slider) {
            return false;
        }

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

// One entry in the pause menu opens the compact, single-slider panel.
class $modify(CWBF_PauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        if (getChildByID("cwbf-pause-button-menu"_spr)) {
            return;
        }

        auto* icon = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        if (!icon) {
            icon = CCSprite::createWithSpriteFrameName("GJ_infoBtn_001.png");
        }
        if (!icon) {
            log::error("Click-Way-Before-Frames: couldn't create pause-menu icon");
            return;
        }

        icon->setScale(0.58f);
        auto* buttonSprite = CircleButtonSprite::create(
            icon,
            CircleBaseColor::DarkPurple,
            CircleBaseSize::Medium
        );
        auto* button = CCMenuItemSpriteExtra::create(
            buttonSprite,
            this,
            menu_selector(CWBF_PauseLayer::onOpenCWBF)
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
        auto* popup = CWBFSettingsPopup::create();
        if (popup) {
            popup->show();
        }
    }
};
