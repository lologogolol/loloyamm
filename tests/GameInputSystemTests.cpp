#include "doctest/doctest.h"

#include "game/app/GameInputSystem.h"
#include "game/ui/GameplayUiController.h"

#include <limits>

#if defined(__ANDROID__)
#include "game/app/GameSession.h"
#include "game/outdoor/OutdoorWorldRuntime.h"
#endif

using namespace OpenYAMM::Game;

TEST_CASE("gamepad axis mapping applies a configurable rescaled deadzone")
{
    CHECK(gamepadAxisValue(0, 25) == 0.0f);
    CHECK(gamepadAxisValue(8191, 25) == 0.0f);
    CHECK(gamepadAxisValue(16384, 25) == doctest::Approx(1.0f / 3.0f).epsilon(0.001));
    CHECK(gamepadAxisValue(std::numeric_limits<int16_t>::max(), 25) == doctest::Approx(1.0f));
    CHECK(gamepadAxisValue(std::numeric_limits<int16_t>::min(), 25) == doctest::Approx(-1.0f));
    CHECK(gamepadAxisValue(16384, -10) == gamepadAxisValue(16384, 0));
    CHECK(gamepadAxisValue(16384, 100) == gamepadAxisValue(16384, 95));
}

#if !defined(__ANDROID__)
namespace
{
SDL_Event mouseButtonEvent(SDL_EventType type, uint8_t button, float x, float y)
{
    SDL_Event event = {};
    event.type = type;
    event.button.button = button;
    event.button.x = x;
    event.button.y = y;
    return event;
}

class VirtualMenuGamepad
{
public:
    VirtualMenuGamepad()
    {
        m_initializedHere = (SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) == 0;
        REQUIRE((!m_initializedHere || SDL_InitSubSystem(SDL_INIT_GAMEPAD)));
        SDL_VirtualJoystickDesc description = {};
        SDL_INIT_INTERFACE(&description);
        description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        description.naxes = SDL_GAMEPAD_AXIS_COUNT;
        description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        description.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
        description.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
        description.name = "OpenYAMM menu navigation test gamepad";
        m_id = SDL_AttachVirtualJoystick(&description);
        REQUIRE_MESSAGE(m_id != 0, SDL_GetError());
        SDL_PumpEvents();
        REQUIRE_MESSAGE(SDL_IsGamepad(m_id), SDL_GetError());
        m_pJoystick = SDL_OpenJoystick(m_id);
        REQUIRE_MESSAGE(m_pJoystick != nullptr, SDL_GetError());
        SDL_UpdateGamepads();
    }

    ~VirtualMenuGamepad()
    {
        SDL_CloseJoystick(m_pJoystick);
        SDL_DetachVirtualJoystick(m_id);
        if (m_initializedHere)
        {
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        }
    }

    void axis(SDL_GamepadAxis axis, int16_t value)
    {
        REQUIRE(SDL_SetJoystickVirtualAxis(m_pJoystick, axis, value));
        SDL_UpdateGamepads();
        SDL_PumpEvents();
    }

    void button(SDL_GamepadButton button, bool held)
    {
        REQUIRE(SDL_SetJoystickVirtualButton(m_pJoystick, button, held));
        SDL_UpdateGamepads();
        SDL_PumpEvents();
    }

    SDL_Event addedEvent() const
    {
        SDL_Event event = {};
        event.type = SDL_EVENT_GAMEPAD_ADDED;
        event.gdevice.which = m_id;
        return event;
    }

private:
    bool m_initializedHere = false;
    SDL_JoystickID m_id = 0;
    SDL_Joystick *m_pJoystick = nullptr;
};

void updateMenuGamepad(GameInputSystem &input, const GameSettings &settings, float deltaSeconds = 1.0f / 60.0f)
{
    input.updateFromEngineInput(
        640, 480, 0.0f, settings, false, true, true, false, true, false, {}, true, deltaSeconds);
}

void updateGameplayGamepad(GameInputSystem &input, const GameSettings &settings, float deltaSeconds = 1.0f / 60.0f)
{
    input.updateFromEngineInput(
        640, 480, 0.0f, settings, false, true, true, false, true, false, {}, false, deltaSeconds);
}
}

TEST_CASE("gamepad right stick drives the menu cursor and face buttons click")
{
    VirtualMenuGamepad gamepad;
    GameInputSystem input;
    const GameSettings settings = GameSettings::createDefault();
    input.handleSdlEvent(gamepad.addedEvent());

    updateMenuGamepad(input, settings);
    const float initialX = input.frame().pointerX;
    gamepad.axis(SDL_GAMEPAD_AXIS_RIGHTX, std::numeric_limits<int16_t>::max());
    updateMenuGamepad(input, settings, 0.05f);
    CHECK(input.frame().pointerX > initialX);
    CHECK(input.frame().relativeMouseX == 0.0f);

    gamepad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    updateMenuGamepad(input, settings);
    CHECK(input.frame().leftMouseButton.pressed);
    CHECK(input.frame().leftMouseButton.held);
    gamepad.button(SDL_GAMEPAD_BUTTON_SOUTH, false);
    updateMenuGamepad(input, settings);
    CHECK(input.frame().leftMouseButton.released);

    gamepad.button(SDL_GAMEPAD_BUTTON_WEST, true);
    updateMenuGamepad(input, settings);
    CHECK(input.frame().rightMouseButton.pressed);
}

TEST_CASE("gamepad dpad and left stick navigate menu focus and A accepts")
{
    VirtualMenuGamepad gamepad;
    GameInputSystem input;
    const GameSettings settings = GameSettings::createDefault();
    input.handleSdlEvent(gamepad.addedEvent());

    gamepad.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    updateMenuGamepad(input, settings);
    CHECK_EQ(input.frame().scancodePressCount(SDL_SCANCODE_DOWN), 1);
    CHECK(input.frame().isScancodeHeld(SDL_SCANCODE_DOWN));
    gamepad.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
    updateMenuGamepad(input, settings);

    gamepad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    updateMenuGamepad(input, settings);
    CHECK_EQ(input.frame().scancodePressCount(SDL_SCANCODE_RETURN), 1);
    CHECK_FALSE(input.frame().leftMouseButton.held);
    gamepad.button(SDL_GAMEPAD_BUTTON_SOUTH, false);
    updateMenuGamepad(input, settings);

    gamepad.axis(SDL_GAMEPAD_AXIS_LEFTX, std::numeric_limits<int16_t>::min());
    updateMenuGamepad(input, settings);
    CHECK_EQ(input.frame().scancodePressCount(SDL_SCANCODE_LEFT), 1);
    updateMenuGamepad(input, settings, 0.1f);
    updateMenuGamepad(input, settings, 0.1f);
    updateMenuGamepad(input, settings, 0.1f);
    updateMenuGamepad(input, settings, 0.1f);
    CHECK_EQ(input.frame().scancodePressCount(SDL_SCANCODE_LEFT), 1);

    gamepad.button(SDL_GAMEPAD_BUTTON_EAST, true);
    updateMenuGamepad(input, settings);
    CHECK_EQ(input.frame().scancodePressCount(SDL_SCANCODE_ESCAPE), 1);
}

TEST_CASE("right stick click toggles gamepad look and interact cursor modes")
{
    VirtualMenuGamepad gamepad;
    GameInputSystem input;
    const GameSettings settings = GameSettings::createDefault();
    input.handleSdlEvent(gamepad.addedEvent());

    updateGameplayGamepad(input, settings);
    CHECK_FALSE(input.frame().gamepadCursorModeActive);

    gamepad.button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
    updateGameplayGamepad(input, settings);
    CHECK(input.frame().gamepadCursorModeActive);
    gamepad.button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, false);
    updateGameplayGamepad(input, settings);
    CHECK(input.frame().gamepadCursorModeActive);

    const float initialX = input.frame().pointerX;
    gamepad.axis(SDL_GAMEPAD_AXIS_RIGHTX, std::numeric_limits<int16_t>::max());
    updateGameplayGamepad(input, settings, 0.05f);
    CHECK(input.frame().pointerX > initialX);
    CHECK(input.frame().relativeMouseX == 0.0f);

    gamepad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    updateGameplayGamepad(input, settings);
    CHECK(input.frame().leftMouseButton.held);
    CHECK_FALSE(input.frame().action(KeyboardAction::Jump).held);
    gamepad.button(SDL_GAMEPAD_BUTTON_SOUTH, false);
    updateGameplayGamepad(input, settings);

    gamepad.button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
    updateGameplayGamepad(input, settings);
    CHECK_FALSE(input.frame().gamepadCursorModeActive);
    CHECK(input.frame().relativeMouseX > 0.0f);
}

TEST_CASE("gamepad gameplay buttons and triggers follow remapped settings")
{
    VirtualMenuGamepad gamepad;
    GameInputSystem input;
    GameSettings settings = GameSettings::createDefault();
    settings.gamepad.setBinding(
        KeyboardAction::Attack, gamepadButtonInputBinding(SDL_GAMEPAD_BUTTON_NORTH));
    settings.gamepad.setBinding(KeyboardAction::Jump, GamepadBinding{});
    input.handleSdlEvent(gamepad.addedEvent());

    gamepad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    updateGameplayGamepad(input, settings);
    CHECK_FALSE(input.frame().action(KeyboardAction::Jump).held);
    CHECK_FALSE(input.frame().action(KeyboardAction::Attack).held);

    gamepad.button(SDL_GAMEPAD_BUTTON_NORTH, true);
    updateGameplayGamepad(input, settings);
    CHECK(input.frame().action(KeyboardAction::Attack).pressed);
    CHECK(input.frame().action(KeyboardAction::Attack).held);
}

TEST_CASE("gamepad enable deadzone sensitivity and invert settings affect input independently")
{
    VirtualMenuGamepad gamepad;
    GameInputSystem input;
    GameSettings settings = GameSettings::createDefault();
    settings.gamepad.setBinding(
        KeyboardAction::Attack, gamepadButtonInputBinding(SDL_GAMEPAD_BUTTON_NORTH));
    input.handleSdlEvent(gamepad.addedEvent());

    gamepad.axis(SDL_GAMEPAD_AXIS_RIGHTX, 12000);
    settings.gamepad.deadZonePercent = 50;
    updateGameplayGamepad(input, settings, 0.02f);
    CHECK(input.frame().relativeMouseX == 0.0f);

    settings.gamepad.deadZonePercent = 0;
    updateGameplayGamepad(input, settings, 0.02f);
    CHECK(input.frame().relativeMouseX > 0.0f);

    gamepad.axis(SDL_GAMEPAD_AXIS_RIGHTX, std::numeric_limits<int16_t>::max());
    settings.gamepad.lookSensitivity = 50;
    updateGameplayGamepad(input, settings, 0.02f);
    CHECK(input.frame().relativeMouseX == doctest::Approx(6.0f));

    gamepad.axis(SDL_GAMEPAD_AXIS_RIGHTY, std::numeric_limits<int16_t>::max());
    settings.gamepad.invertLookY = true;
    updateGameplayGamepad(input, settings, 0.02f);
    CHECK(input.frame().relativeMouseY == doctest::Approx(-6.0f));

    gamepad.button(SDL_GAMEPAD_BUTTON_NORTH, true);
    settings.gamepad.enabled = false;
    updateGameplayGamepad(input, settings);
    CHECK_FALSE(input.frame().action(KeyboardAction::Attack).held);
    CHECK(input.frame().relativeMouseX == 0.0f);
}

TEST_CASE("game input preserves a quick trackpad tap and its mouse action")
{
    GameInputSystem inputSystem;
    GameSettings settings = {};
    settings.keyboard.setBinding(KeyboardAction::Attack, mouseButtonInputBinding(SDL_BUTTON_LEFT));
    inputSystem.handleSdlEvent(mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, 100.0f, 200.0f));
    inputSystem.handleSdlEvent(mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT, 100.0f, 200.0f));

    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
    CHECK(inputSystem.frame().leftMouseButton.held);
    CHECK(inputSystem.frame().leftMouseButton.pressed);
    CHECK(inputSystem.frame().actions[keyboardActionIndex(KeyboardAction::Attack)].pressed);

    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
    CHECK_FALSE(inputSystem.frame().leftMouseButton.held);
    CHECK(inputSystem.frame().leftMouseButton.released);
    CHECK(inputSystem.frame().actions[keyboardActionIndex(KeyboardAction::Attack)].released);

    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
    CHECK_FALSE(inputSystem.frame().leftMouseButton.pressed);
    CHECK_FALSE(inputSystem.frame().leftMouseButton.released);
}

TEST_CASE("auto-hold left click latches in gameplay and menus cancel it")
{
    GameInputSystem inputSystem;
    GameSettings settings = {};
    settings.autoHoldLeftClick = true;

    inputSystem.handleSdlEvent(mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, 100.0f, 200.0f));
    inputSystem.handleSdlEvent(mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT, 100.0f, 200.0f));
    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
    CHECK(inputSystem.frame().leftMouseButton.pressed);
    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
    CHECK(inputSystem.frame().leftMouseButton.held);

    inputSystem.handleSdlEvent(mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, 100.0f, 200.0f));
    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
    CHECK(inputSystem.frame().leftMouseButton.released);

    inputSystem.handleSdlEvent(mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, 100.0f, 200.0f));
    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
    REQUIRE(inputSystem.frame().leftMouseButton.held);
    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings, true);
    CHECK_FALSE(inputSystem.frame().leftMouseButton.held);
    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
    CHECK_FALSE(inputSystem.frame().leftMouseButton.held);
}

TEST_CASE("game input preserves drag start and release positions between frames")
{
    GameInputSystem inputSystem;
    GameSettings settings = {};
    inputSystem.handleSdlEvent(mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, 100.0f, 200.0f));
    inputSystem.handleSdlEvent(mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT, 300.0f, 400.0f));

    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
    CHECK(inputSystem.frame().leftMouseButton.pressed);
    CHECK(inputSystem.frame().pointerX == 100.0f);
    CHECK(inputSystem.frame().pointerY == 200.0f);

    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
    CHECK(inputSystem.frame().leftMouseButton.released);
    CHECK(inputSystem.frame().pointerX == 300.0f);
    CHECK(inputSystem.frame().pointerY == 400.0f);
}

TEST_CASE("game input preserves repeated taps and secondary button taps")
{
    for (const uint8_t button : {SDL_BUTTON_LEFT, SDL_BUTTON_MIDDLE, SDL_BUTTON_RIGHT})
    {
        GameInputSystem inputSystem;
        GameSettings settings = {};

        for (int tap = 0; tap < 2; ++tap)
        {
            inputSystem.handleSdlEvent(mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, button, 100.0f, 200.0f));
            inputSystem.handleSdlEvent(mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, button, 100.0f, 200.0f));
        }

        for (int frame = 0; frame < 4; ++frame)
        {
            inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
            const GameplayButtonInputState &state = button == SDL_BUTTON_LEFT
                ? inputSystem.frame().leftMouseButton
                : button == SDL_BUTTON_RIGHT
                    ? inputSystem.frame().rightMouseButton
                    : inputSystem.frame().middleMouseButton;
            CHECK(state.pressed == (frame % 2 == 0));
            CHECK(state.released == (frame % 2 == 1));
        }
    }
}

TEST_CASE("game input discards queued taps when input is blocked or a screen suppresses the mouse")
{
    GameInputSystem inputSystem;
    GameSettings settings = {};
    inputSystem.handleSdlEvent(mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, 100.0f, 200.0f));
    inputSystem.handleSdlEvent(mouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT, 100.0f, 200.0f));

    SUBCASE("blocked input")
    {
        inputSystem.updateFromEngineInput(640, 480, 0.0f, settings, true);
    }

    SUBCASE("screen transition")
    {
        inputSystem.suppressMouseButtonsUntilReleased();
    }

    for (int frame = 0; frame < 3; ++frame)
    {
        inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
        CHECK_FALSE(inputSystem.frame().leftMouseButton.pressed);
        CHECK_FALSE(inputSystem.frame().leftMouseButton.released);
    }
}

TEST_CASE("game input guards a window opened by a click against the next quick click")
{
    // SDL's clock starts at its first read: never 0 here, which would mean no click at all.
    const uint64_t now = SDL_GetTicksNS() + 1;
    const auto click = [](GameInputSystem &inputSystem, uint64_t timestamp)
    {
        for (const SDL_EventType type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP})
        {
            SDL_Event event = mouseButtonEvent(type, SDL_BUTTON_LEFT, 100.0f, 200.0f);
            event.button.timestamp = timestamp;
            inputSystem.handleSdlEvent(event);
        }
    };
    const auto pressedInFrames = [](GameInputSystem &inputSystem, const GameSettings &settings)
    {
        bool pressed = false;

        for (int frame = 0; frame < 3; ++frame)
        {
            inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
            pressed = pressed || inputSystem.frame().leftMouseButton.pressed;
        }

        return pressed;
    };
    GameInputSystem inputSystem;
    GameSettings settings = {};

    SUBCASE("the second click of a double click is dropped, a later click lands")
    {
        // The click on the pedestal opens the window.
        click(inputSystem, now);
        CHECK(pressedInFrames(inputSystem, settings));
        inputSystem.guardClicksAfterWindowOpened();

        click(inputSystem, now + 50'000'000ULL);
        CHECK_FALSE(pressedInFrames(inputSystem, settings));

        click(inputSystem, now + GameInputSystem::WindowClickGuardNanoseconds + 50'000'000ULL);
        CHECK(pressedInFrames(inputSystem, settings));
    }

    SUBCASE("a window opened with no click is not guarded, and keys always pass")
    {
        inputSystem.guardClicksAfterWindowOpened();
        click(inputSystem, now + 10'000'000ULL);
        SDL_Event key = {};
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.scancode = SDL_SCANCODE_RETURN;
        inputSystem.handleSdlEvent(key);
        inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
        CHECK(inputSystem.frame().leftMouseButton.pressed);
        CHECK(inputSystem.frame().keyboardPressCounts[SDL_SCANCODE_RETURN] == 1);
    }
}

TEST_CASE("game input turns the camera by the mouse's motion events only")
{
    // A compositor that cannot warp the pointer (Wayland) later reports where it really is; SDL adds that jump to
    // its relative mouse state though no motion event carries it. Mouse look takes the motion events alone.
    GameInputSystem inputSystem;
    GameSettings settings = {};
    SDL_Event motion = {};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.xrel = 3.0f;
    motion.motion.yrel = -2.0f;
    inputSystem.handleSdlEvent(motion);
    inputSystem.handleSdlEvent(motion);

    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
    CHECK(inputSystem.frame().relativeMouseX == 6.0f);
    CHECK(inputSystem.frame().relativeMouseY == -4.0f);

    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
    CHECK(inputSystem.frame().relativeMouseX == 0.0f);
    CHECK(inputSystem.frame().relativeMouseY == 0.0f);

    // Entering or leaving mouse look drops the motion made before it.
    inputSystem.handleSdlEvent(motion);
    inputSystem.resetRelativeMouseMotion();
    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);
    CHECK(inputSystem.frame().relativeMouseX == 0.0f);
    CHECK(inputSystem.frame().relativeMouseY == 0.0f);
}

#endif

#if defined(__ANDROID__)
TEST_CASE("Android inventory close stores held items and preserves them when all packs are full")
{
    class InventoryWorld : public OutdoorWorldRuntime
    {
    public:
        Party m_party;
        Party *party() override
        {
            return &m_party;
        }
        const Party *party() const override
        {
            return &m_party;
        }
    };
    InventoryWorld world;
    PartySeed seed;
    seed.members.resize(2);
    world.m_party.seed(seed);
    GameSession session;
    session.bindActiveWorldRuntime(&world);
    GameplayScreenRuntime &runtime = session.gameplayScreenRuntime();
    runtime.characterScreen().open = true;
    runtime.closeCharacterScreen();
    CHECK_FALSE(runtime.heldInventoryItem().active);
    CHECK(world.m_party.member(0)->inventory.empty());
    CHECK(world.m_party.member(1)->inventory.empty());

    InventoryItem item = {};
    item.objectDescriptionId = 99;
    item.quantity = 1;
    item.width = 1;
    item.height = 1;
    item.currentCharges = 3;
    for (const int fullPacks : {0, 1, 2})
    {
        if (fullPacks > 0)
        {
            Character *pMember = world.m_party.member(fullPacks == 1 ? 1 : 0);
            pMember->inventory.clear();
            for (uint8_t y = 0; y < Character::InventoryHeight; ++y)
            {
                for (uint8_t x = 0; x < Character::InventoryWidth; ++x)
                {
                    REQUIRE(pMember->addInventoryItemAt(item, x, y));
                }
            }
        }
        runtime.characterScreen().open = true;
        runtime.characterScreen().sourceIndex = 1;
        runtime.heldInventoryItem().active = true;
        runtime.heldInventoryItem().item = item;
        world.m_party.setHeldItemForQueries(item);
        runtime.interactionState().characterTouchItemDragActive = true;
        runtime.toggleCharacterInventoryScreen();
        CHECK_FALSE(runtime.characterScreenReadOnly().open);
        CHECK_FALSE(runtime.interactionState().characterTouchItemDragActive);
        CHECK(runtime.heldInventoryItem().active == (fullPacks == 2));
        if (fullPacks == 2)
        {
            CHECK(runtime.heldInventoryItem().item.objectDescriptionId == item.objectDescriptionId);
            CHECK(runtime.heldInventoryItem().item.currentCharges == item.currentCharges);
        }
        else
        {
            const Character *pRecipient = world.m_party.member(fullPacks == 0 ? 1 : 0);
            REQUIRE(pRecipient->inventory.size() == 1);
            CHECK(pRecipient->inventory.front().objectDescriptionId == item.objectDescriptionId);
            CHECK(pRecipient->inventory.front().currentCharges == item.currentCharges);
        }
    }
}

TEST_CASE("Android item drags preserve pickup, drop, pointer and inspection")
{
    for (const std::pair<int, int> dimensions : {std::pair{640, 480}, {1600, 900}, {2100, 900}})
    {
        const int width = dimensions.first;
        const int height = dimensions.second;
        const std::array<GameplayTouchControl, 1> controls = {{
            {GameplayTouchRole::Inspect, float(width - 100), 10, 80, 60}}};
        const auto event = [&](GameInputSystem &input, uint32_t type, int finger, float x, float y)
        {
            SDL_Event touch = {};
            touch.type = type;
            touch.tfinger.fingerID = finger;
            touch.tfinger.x = x / width;
            touch.tfinger.y = y / height;
            input.handleSdlEvent(touch);
        };
        const auto update = [&](GameInputSystem &input)
        {
            input.updateFromEngineInput(
                width, height, 0, GameSettings{}, false, false, false, false, true, false, controls);
        };
        const auto pointerAt = [&](const GameInputSystem &input, float x, float y)
        {
            return std::abs(input.frame().pointerX - x) < 0.01f
                && std::abs(input.frame().pointerY - y) < 0.01f;
        };

        for (const bool betweenFrames : {false, true})
        {
            GameInputSystem input;
            event(input, SDL_EVENT_FINGER_DOWN, 1, width / 3, height / 3);
            if (!betweenFrames)
            {
                update(input);
            }
            event(input, SDL_EVENT_FINGER_MOTION, 1, width / 2, height * 0.9f);
            if (!betweenFrames)
            {
                update(input);
                CHECK(input.frame().mobileTouchDragStarted);
                CHECK(input.frame().mobileTouchDragActive);
            }
            event(input, SDL_EVENT_FINGER_UP, 1, width / 2, height * 0.9f);
            update(input);
            if (betweenFrames)
            {
                CHECK(input.frame().mobileTouchDragStarted);
                CHECK(input.frame().mobileTouchDragActive);
                CHECK_FALSE(input.frame().mobileTouchDragReleased);
                CHECK(pointerAt(input, width / 3, height / 3));
                update(input);
            }
            CHECK(input.frame().mobileTouchDragReleased);
            CHECK_FALSE(input.frame().leftMouseButton.held);
            CHECK(pointerAt(input, width / 2, height * 0.9f));
            update(input);
            CHECK_FALSE(input.frame().mobileTouchDragReleased);
            CHECK(pointerAt(input, width / 2, height * 0.9f));

            event(input, SDL_EVENT_FINGER_DOWN, 2, width - 60, 40);
            update(input);
            CHECK(input.frame().rightMouseButton.held);
            CHECK_FALSE(input.frame().leftMouseButton.held);
            event(input, SDL_EVENT_FINGER_DOWN, 3, width / 3, height / 3);
            update(input);
            CHECK(input.frame().rightMouseButton.held);
            CHECK(pointerAt(input, width / 3, height / 3));
            event(input, SDL_EVENT_FINGER_UP, 3, width / 3, height / 3);
            event(input, SDL_EVENT_FINGER_UP, 2, width - 60, 40);
            update(input);
            CHECK_FALSE(input.frame().rightMouseButton.held);
        }
    }
}

TEST_CASE("Android overlay taps click on release and long presses inspect")
{
    const int width = 1600;
    const int height = 900;
    const auto event = [&](GameInputSystem &input, uint32_t type, float x, float y)
    {
        SDL_Event touch = {};
        touch.type = type;
        touch.tfinger.timestamp = SDL_GetTicksNS();
        touch.tfinger.fingerID = 1;
        touch.tfinger.x = x / width;
        touch.tfinger.y = y / height;
        input.handleSdlEvent(touch);
    };
    const auto update = [&](GameInputSystem &input, bool longPressInspect = true)
    {
        input.updateFromEngineInput(
            width, height, 0, GameSettings{}, false, false, false, false, true, longPressInspect);
    };
    const auto pointerAt = [&](const GameInputSystem &input, float x, float y)
    {
        return std::abs(input.frame().pointerX - x) < 0.01f && std::abs(input.frame().pointerY - y) < 0.01f;
    };

    GameInputSystem tap;
    event(tap, SDL_EVENT_FINGER_DOWN, 400, 300);
    update(tap);
    CHECK_FALSE(tap.frame().leftMouseButton.held);
    CHECK_FALSE(tap.frame().rightMouseButton.held);
    CHECK(pointerAt(tap, 400, 300));
    event(tap, SDL_EVENT_FINGER_UP, 400, 300);
    update(tap);
    CHECK(tap.frame().leftMouseButton.pressed);
    CHECK(pointerAt(tap, 400, 300));
    update(tap);
    CHECK(tap.frame().leftMouseButton.released);

    GameInputSystem hold;
    event(hold, SDL_EVENT_FINGER_DOWN, 400, 300);
    update(hold);
    SDL_Delay(420);
    update(hold);
    CHECK(hold.frame().rightMouseButton.pressed);
    CHECK_FALSE(hold.frame().leftMouseButton.held);
    event(hold, SDL_EVENT_FINGER_MOTION, 700, 500);
    update(hold);
    CHECK(hold.frame().rightMouseButton.held);
    CHECK_FALSE(hold.frame().mobileTouchDragStarted);
    CHECK(pointerAt(hold, 700, 500));
    event(hold, SDL_EVENT_FINGER_UP, 700, 500);
    update(hold);
    CHECK(hold.frame().rightMouseButton.released);
    CHECK_FALSE(hold.frame().leftMouseButton.held);
    update(hold);
    CHECK_FALSE(hold.frame().leftMouseButton.held);

    GameInputSystem drag;
    event(drag, SDL_EVENT_FINGER_DOWN, 400, 300);
    update(drag);
    CHECK_FALSE(drag.frame().leftMouseButton.held);
    event(drag, SDL_EVENT_FINGER_MOTION, 700, 500);
    update(drag);
    CHECK(drag.frame().leftMouseButton.pressed);
    CHECK(drag.frame().mobileTouchDragStarted);
    CHECK(pointerAt(drag, 400, 300));
    update(drag);
    CHECK(pointerAt(drag, 700, 500));
    event(drag, SDL_EVENT_FINGER_UP, 700, 500);
    update(drag);
    CHECK(drag.frame().mobileTouchDragReleased);
    CHECK_FALSE(drag.frame().rightMouseButton.held);

    // The finger that opened the overlay keeps its press instead of turning into a tap or an inspection.
    GameInputSystem carried;
    event(carried, SDL_EVENT_FINGER_DOWN, 400, 300);
    update(carried, false);
    CHECK(carried.frame().leftMouseButton.pressed);
    SDL_Delay(420);
    update(carried);
    CHECK(carried.frame().leftMouseButton.held);
    CHECK_FALSE(carried.frame().rightMouseButton.held);
    event(carried, SDL_EVENT_FINGER_UP, 400, 300);
    update(carried);
    CHECK(carried.frame().leftMouseButton.released);
    update(carried);
    CHECK_FALSE(carried.frame().leftMouseButton.held);
}
#endif

TEST_CASE("default settings disable Ctrl turbo movement without suppressing modifier keys")
{
    const std::optional<GameSettings> settings = GameSettings::createDefault();
    CHECK_FALSE(settings->turboMovementEnabled);

    GameInputSystem inputSystem;
    inputSystem.updateFromEngineInput(640, 480, 0.0f, *settings);
    GameplayInputFrame &input = inputSystem.frame();
    CHECK_FALSE(input.turboMovementHeld());

    for (const SDL_Scancode ctrl : {SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL})
    {
        input.keyboardHeld[ctrl] = true;
        input.keyboardHeld[SDL_SCANCODE_LSHIFT] = true;
        CHECK(input.isScancodeHeld(ctrl));
        CHECK(input.isScancodeHeld(SDL_SCANCODE_LSHIFT));
        CHECK_FALSE(input.turboMovementHeld());
        input.keyboardHeld[ctrl] = false;
    }

    GameSettings turboSettings = GameSettings::createDefault();
    turboSettings.turboMovementEnabled = true;
    inputSystem.updateFromEngineInput(640, 480, 0.0f, turboSettings);
    for (const SDL_Scancode ctrl : {SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL})
    {
        input.keyboardHeld[ctrl] = true;
        CHECK(input.turboMovementHeld());
        input.keyboardHeld[ctrl] = false;
    }
    CHECK_FALSE(input.turboMovementHeld());
}

TEST_CASE("game input mouse wheel bindings pulse on every scroll and respect blocked input")
{
    GameSettings settings = GameSettings::createDefault();
    settings.keyboard.setBinding(KeyboardAction::FlyUp, mouseWheelInputBinding(1.0f));
    settings.keyboard.setBinding(KeyboardAction::FlyDown, mouseWheelInputBinding(-1.0f));
    GameInputSystem inputSystem;

    for (const float delta : {1.0f, 0.5f, 2.0f, -1.0f, -0.5f, 0.0f})
    {
        inputSystem.updateFromEngineInput(640, 480, delta, settings);
        const GameplayInputFrame &input = inputSystem.frame();
        CHECK(input.action(KeyboardAction::FlyUp).held == (delta > 0.0f));
        CHECK(input.action(KeyboardAction::FlyUp).pressed == (delta > 0.0f));
        CHECK(input.action(KeyboardAction::FlyDown).held == (delta < 0.0f));
        CHECK(input.action(KeyboardAction::FlyDown).pressed == (delta < 0.0f));
        CHECK(input.mouseWheelDelta == delta);
    }

    inputSystem.updateFromEngineInput(640, 480, 1.0f, settings, true);
    CHECK_FALSE(inputSystem.frame().action(KeyboardAction::FlyUp).held);
    CHECK_FALSE(inputSystem.frame().action(KeyboardAction::FlyUp).pressed);
    CHECK(inputSystem.frame().mouseWheelDelta == 0.0f);

    inputSystem.updateFromEngineInput(640, 480, 1.0f, GameSettings::createDefault());
    CHECK_FALSE(inputSystem.frame().action(KeyboardAction::FlyUp).held);
    CHECK(inputSystem.frame().mouseWheelDelta == 1.0f);
}

TEST_CASE("game input preserves a transient key press until the next frame")
{
    GameInputSystem inputSystem;
    SDL_Event keyDown = {};
    keyDown.type = SDL_EVENT_KEY_DOWN;
    keyDown.key.scancode = SDL_SCANCODE_BACKSPACE;
    keyDown.key.key = SDLK_BACKSPACE;

    SDL_Event keyUp = keyDown;
    keyUp.type = SDL_EVENT_KEY_UP;

    inputSystem.handleSdlEvent(keyDown);
    inputSystem.handleSdlEvent(keyUp);

    GameSettings settings = {};
    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);

    CHECK_FALSE(inputSystem.frame().isScancodeHeld(SDL_SCANCODE_BACKSPACE));
    CHECK(inputSystem.frame().scancodePressCount(SDL_SCANCODE_BACKSPACE) == 1);

    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);

    CHECK(inputSystem.frame().scancodePressCount(SDL_SCANCODE_BACKSPACE) == 0);
}

TEST_CASE("game input reports a held use key press only on its initial frame")
{
    GameInputSystem inputSystem;
    SDL_Event keyDown = {};
    keyDown.type = SDL_EVENT_KEY_DOWN;
    keyDown.key.scancode = SDL_SCANCODE_E;
    keyDown.key.key = SDLK_E;

    inputSystem.handleSdlEvent(keyDown);

    GameSettings settings = {};
    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);

    CHECK(inputSystem.frame().scancodePressCount(SDL_SCANCODE_E) == 1);

    inputSystem.updateFromEngineInput(640, 480, 0.0f, settings);

    CHECK(inputSystem.frame().scancodePressCount(SDL_SCANCODE_E) == 0);
}

TEST_CASE("mobile inspection follows the character screen instead of its selected tab or source")
{
    using CharacterPage = GameplayUiController::CharacterPage;
    using CharacterScreenSource = GameplayUiController::CharacterScreenSource;

    GameplayUiController::CharacterScreenState screen = {};
    CHECK_FALSE(GameplayUiController::characterScreenSupportsInspection(screen));

    screen.open = true;

    for (const CharacterPage page : {
             CharacterPage::Stats,
             CharacterPage::Skills,
             CharacterPage::Inventory,
             CharacterPage::Awards,
         })
    {
        screen.page = page;
        CHECK(GameplayUiController::characterScreenSupportsInspection(screen));
    }

    screen.source = CharacterScreenSource::AdventurersInn;
    screen.adventurersInnRosterOverlayOpen = true;
    CHECK(GameplayUiController::characterScreenSupportsInspection(screen));
}
