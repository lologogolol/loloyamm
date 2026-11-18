#include "game/app/GameInputSystem.h"

#include "game/app/MobileJoystickDirection.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace OpenYAMM::Game
{
namespace
{
constexpr float MobileLogicalHeight = 480.0f;
constexpr float MobileMovementZoneX = 0.0f;
constexpr float MobileMovementZoneY = 160.0f;
constexpr float MobileMovementZoneWidth = 300.0f;
constexpr float MobileMovementZoneHeight = 320.0f;
constexpr float MobileCameraZoneX = 300.0f;
constexpr float MobileJoystickRadius = 64.0f;
constexpr float MobileJoystickDeadZone = 10.0f;
constexpr float MobileJoystickFullSpeedRadius = MobileJoystickRadius * 0.7f;
constexpr float MobileGameplayTapMaxNormalizedDistanceSquared = 0.000225f;
constexpr float MobileDebugConsoleGestureTopEdgeNormalized = 0.055f;
constexpr float MobileDebugConsoleGestureMinDragNormalized = 0.14f;
constexpr float MobileDebugConsoleGestureMaxHorizontalNormalized = 0.09f;
constexpr uint64_t MobileLongPressInspectNanoseconds = 400'000'000ULL;
constexpr float GamepadDigitalThreshold = 0.25f;
constexpr float GamepadTriggerThreshold = 0.35f;
constexpr float GamepadLookPixelsPerSecond = 600.0f;
constexpr float GamepadMenuDigitalThreshold = 0.55f;
constexpr float GamepadMenuInitialRepeatSeconds = 0.36f;
constexpr float GamepadMenuRepeatSeconds = 0.11f;
constexpr float GamepadMenuCursorMinimumPixelsPerSecond = 260.0f;
constexpr float GamepadMenuCursorMaximumPixelsPerSecond = 1260.0f;

GameplayButtonInputState buildButtonState(bool held, bool previousHeld)
{
    GameplayButtonInputState state = {};
    state.held = held;
    state.pressed = held && !previousHeld;
    state.released = !held && previousHeld;
    return state;
}

float mobileLogicalScale(int screenHeight)
{
    return screenHeight > 0 ? static_cast<float>(screenHeight) / MobileLogicalHeight : 1.0f;
}

bool pointInsideRect(float x, float y, float rectX, float rectY, float rectWidth, float rectHeight)
{
    return x >= rectX && y >= rectY && x <= rectX + rectWidth && y <= rectY + rectHeight;
}

void suppressButtonWhileHeld(bool physicalHeld, bool &logicalHeld, bool &suppressUntilReleased)
{
    if (!suppressUntilReleased)
    {
        return;
    }

    if (physicalHeld)
    {
        logicalHeld = false;
        return;
    }

    suppressUntilReleased = false;
}

} // namespace

float gamepadAxisValue(int16_t value, int deadZonePercent)
{
    const float normalized = value < 0
        ? static_cast<float>(value) / 32768.0f
        : static_cast<float>(value) / 32767.0f;
    const float magnitude = std::abs(normalized);
    const float deadZone = static_cast<float>(std::clamp(deadZonePercent, 0, 95)) / 100.0f;
    if (magnitude <= deadZone)
    {
        return 0.0f;
    }
    const float rescaled = std::clamp((magnitude - deadZone) / (1.0f - deadZone), 0.0f, 1.0f);
    return std::copysign(rescaled, normalized);
}

GameInputSystem::~GameInputSystem()
{
    closeGamepad();
}

void GameInputSystem::closeGamepad()
{
    if (m_pGamepad != nullptr)
    {
        SDL_CloseGamepad(m_pGamepad);
        m_pGamepad = nullptr;
    }
    m_gamepadInteractCursorMode = false;
    m_gamepadCursorDriving = false;
}

void GameInputSystem::ensureGamepadOpen()
{
    if (m_pGamepad != nullptr || m_initialGamepadScanComplete)
    {
        return;
    }

    int gamepadCount = 0;
    SDL_JoystickID *pGamepadIds = SDL_GetGamepads(&gamepadCount);
    if (pGamepadIds != nullptr && gamepadCount > 0)
    {
        m_pGamepad = SDL_OpenGamepad(pGamepadIds[0]);
    }
    SDL_free(pGamepadIds);
    m_initialGamepadScanComplete = true;
}

void GameInputSystem::handleSdlEvent(const SDL_Event &event)
{
    if (event.type == SDL_EVENT_GAMEPAD_ADDED && m_pGamepad == nullptr)
    {
        m_pGamepad = SDL_OpenGamepad(event.gdevice.which);
        m_initialGamepadScanComplete = true;
    }
    else if (event.type == SDL_EVENT_GAMEPAD_REMOVED
        && m_pGamepad != nullptr
        && SDL_GetGamepadID(m_pGamepad) == event.gdevice.which)
    {
        closeGamepad();
        m_initialGamepadScanComplete = false;
    }

#if !defined(__ANDROID__) && !defined(OPENYAMM_IOS)
    if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP)
        && event.button.button >= SDL_BUTTON_LEFT && event.button.button <= SDL_BUTTON_RIGHT)
    {
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        {
            m_lastMouseButtonDownNanoseconds = event.button.timestamp;
        }

        m_pendingMouseButtonEvents[event.button.button - SDL_BUTTON_LEFT].push_back(event.button);
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT
            && m_pendingAutoHoldLeftClickPresses < std::numeric_limits<uint16_t>::max())
        {
            ++m_pendingAutoHoldLeftClickPresses;
        }
        return;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION)
    {
        m_pendingRelativeMouseX += event.motion.xrel;
        m_pendingRelativeMouseY += event.motion.yrel;
    }
#endif

    if (event.type == SDL_EVENT_TEXT_INPUT)
    {
        if (event.text.text != nullptr)
        {
            m_pendingTextInput.append(event.text.text, std::strlen(event.text.text));
        }

        return;
    }

    if (event.type == SDL_EVENT_KEY_DOWN
        && !event.key.repeat
        && event.key.scancode > SDL_SCANCODE_UNKNOWN
        && event.key.scancode < SDL_SCANCODE_COUNT)
    {
        uint16_t &pressCount = m_pendingKeyboardPressCounts[event.key.scancode];

        if (pressCount < std::numeric_limits<uint16_t>::max())
        {
            ++pressCount;
        }
    }

#if defined(__ANDROID__) || defined(OPENYAMM_IOS)
    if (event.type == SDL_EVENT_MOUSE_MOTION && event.motion.which != SDL_TOUCH_MOUSEID)
    {
        m_mobilePointerPosition.reset();
    }

    const auto findTouch =
        [this](SDL_FingerID fingerId) -> MobileTouchPoint *
        {
            for (MobileTouchPoint &touch : m_mobileTouches)
            {
                if (touch.active && touch.fingerId == fingerId)
                {
                    return &touch;
                }
            }

            return nullptr;
        };

    if (event.type == SDL_EVENT_FINGER_DOWN)
    {
        MobileTouchPoint *pTouch = findTouch(event.tfinger.fingerID);

        if (pTouch == nullptr)
        {
            for (MobileTouchPoint &candidate : m_mobileTouches)
            {
                if (!candidate.active)
                {
                    pTouch = &candidate;
                    break;
                }
            }
        }

        if (pTouch == nullptr)
        {
            return;
        }

        *pTouch = {};
        pTouch->active = true;
        pTouch->fingerId = event.tfinger.fingerID;
        pTouch->startX = event.tfinger.x;
        pTouch->startY = event.tfinger.y;
        pTouch->startTimestampNanoseconds = event.tfinger.timestamp;
        pTouch->x = event.tfinger.x;
        pTouch->y = event.tfinger.y;
        pTouch->debugConsoleGestureCandidate =
            event.tfinger.y >= 0.0f && event.tfinger.y <= MobileDebugConsoleGestureTopEdgeNormalized;
        return;
    }

    if (event.type == SDL_EVENT_FINGER_MOTION)
    {
        MobileTouchPoint *pTouch = findTouch(event.tfinger.fingerID);

        if (pTouch == nullptr)
        {
            return;
        }

        pTouch->deltaX += event.tfinger.x - pTouch->x;
        pTouch->deltaY += event.tfinger.y - pTouch->y;
        pTouch->x = event.tfinger.x;
        pTouch->y = event.tfinger.y;

        const float touchDragX = pTouch->x - pTouch->startX;
        const float touchDragY = pTouch->y - pTouch->startY;
        pTouch->dragThresholdExceeded =
            pTouch->dragThresholdExceeded
            || touchDragX * touchDragX + touchDragY * touchDragY
                > MobileGameplayTapMaxNormalizedDistanceSquared;

        if (pTouch->debugConsoleGestureCandidate && !pTouch->debugConsoleGestureTriggered)
        {
            const float dragX = event.tfinger.x - pTouch->startX;
            const float dragY = event.tfinger.y - pTouch->startY;

            if (dragY >= MobileDebugConsoleGestureMinDragNormalized
                && std::abs(dragX) <= MobileDebugConsoleGestureMaxHorizontalNormalized)
            {
                pTouch->role = MobileTouchRole::DebugConsoleGesture;
                pTouch->debugConsoleGestureTriggered = true;
                pTouch->dragThresholdExceeded = false;
                m_mobilePendingHudTap = false;
                m_mobilePendingHudRelease = false;
                m_mobilePendingCameraTap = false;
                m_mobileJumpDoubleTapGesture.cancel();
                m_mobileDebugConsoleToggleRequested = true;
            }
        }

        return;
    }

    if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED)
    {
        MobileTouchPoint *pTouch = findTouch(event.tfinger.fingerID);

        if (pTouch != nullptr)
        {
            const bool completedHudDrag =
                event.type == SDL_EVENT_FINGER_UP
                && pTouch->dragThresholdExceeded
                && (pTouch->role == MobileTouchRole::Hud || pTouch->role == MobileTouchRole::None);

            if (completedHudDrag)
            {
                m_mobilePendingHudDragRelease = true;
                m_mobilePendingHudDragStartDelivered = pTouch->dragStartDelivered;
                m_mobilePendingHudDragStartX = pTouch->startX;
                m_mobilePendingHudDragStartY = pTouch->startY;
                m_mobilePendingHudDragReleaseX = event.tfinger.x;
                m_mobilePendingHudDragReleaseY = event.tfinger.y;
                m_mobilePendingHudRelease = false;
                m_mobilePendingHudTap = false;
            }
            else if (pTouch->role == MobileTouchRole::Hud
                && pTouch->longPressCandidate
                && event.type == SDL_EVENT_FINGER_UP)
            {
                // A short tap on a long-press screen clicks only now that it has been released.
                m_mobilePendingHudTap = true;
                m_mobilePendingHudTapStartX = pTouch->startX;
                m_mobilePendingHudTapStartY = pTouch->startY;
                m_mobilePendingHudTapX = event.tfinger.x;
                m_mobilePendingHudTapY = event.tfinger.y;
            }
            else if (pTouch->role == MobileTouchRole::Hud)
            {
                m_mobilePendingHudRelease = true;
                m_mobilePendingHudReleaseX = event.tfinger.x;
                m_mobilePendingHudReleaseY = event.tfinger.y;
            }
            else if (pTouch->role == MobileTouchRole::Camera)
            {
                const float deltaX = event.tfinger.x - pTouch->startX;
                const float deltaY = event.tfinger.y - pTouch->startY;
                const float distanceSquared = deltaX * deltaX + deltaY * deltaY;

                if (event.type == SDL_EVENT_FINGER_UP
                    && !pTouch->dragThresholdExceeded
                    && distanceSquared <= MobileGameplayTapMaxNormalizedDistanceSquared)
                {
                    m_mobilePendingCameraTap = true;
                    m_mobilePendingCameraTapTimestampNanoseconds = event.tfinger.timestamp;
                    m_mobilePendingCameraTapX = event.tfinger.x;
                    m_mobilePendingCameraTapY = event.tfinger.y;
                }
            }
            else if (pTouch->role == MobileTouchRole::None)
            {
                m_mobilePendingHudTap = true;
                m_mobilePendingHudTapStartX = pTouch->startX;
                m_mobilePendingHudTapStartY = pTouch->startY;
                m_mobilePendingHudTapX = event.tfinger.x;
                m_mobilePendingHudTapY = event.tfinger.y;
            }

            *pTouch = {};
        }
    }
#else
    (void)event;
#endif
}

bool GameInputSystem::consumeMobileDebugConsoleToggleRequested()
{
    const bool requested = m_mobileDebugConsoleToggleRequested;
    m_mobileDebugConsoleToggleRequested = false;
    return requested;
}

void GameInputSystem::updateFromEngineInput(
    int screenWidth,
    int screenHeight,
    float mouseWheelDelta,
    const GameSettings &settings,
    bool blockGameplayInput,
    bool mobileGameplayTouchControlsEnabled,
    bool mobileJumpGestureEnabled,
    bool mobileFlightControlsEnabled,
    bool mobileInspectControlEnabled,
    bool mobileLongPressInspectEnabled,
    std::span<const GameplayTouchControl> touchControls,
    bool menuNavigationEnabled,
    float frameDeltaSeconds)
{
    m_frame = {};
    m_frame.turboMovementEnabled = settings.turboMovementEnabled;
    m_frame.screenWidth = screenWidth;
    m_frame.screenHeight = screenHeight;
    m_frame.mouseWheelDelta = blockGameplayInput ? 0.0f : mouseWheelDelta;
    m_frame.textInput = blockGameplayInput ? std::string() : std::move(m_pendingTextInput);
    m_pendingTextInput.clear();

    if (!blockGameplayInput)
    {
        m_frame.keyboardPressCounts = m_pendingKeyboardPressCounts;
    }

    m_pendingKeyboardPressCounts.fill(0);

    int keyboardStateCount = 0;
    const bool *pKeyboardState = SDL_GetKeyboardState(&keyboardStateCount);

    if (pKeyboardState != nullptr)
    {
        for (int scancode = 0; scancode < keyboardStateCount && scancode < SDL_SCANCODE_COUNT; ++scancode)
        {
            m_frame.keyboardHeld[scancode] = blockGameplayInput ? false : pKeyboardState[scancode];
        }
    }

    float pointerX = 0.0f;
    float pointerY = 0.0f;
    const SDL_MouseButtonFlags mouseButtons = SDL_GetMouseState(&pointerX, &pointerY);
    m_frame.pointerX = pointerX;
    m_frame.pointerY = pointerY;

    const bool physicalLeftMouseButtonHeld = (mouseButtons & SDL_BUTTON_LMASK) != 0;
    const bool physicalRightMouseButtonHeld = (mouseButtons & SDL_BUTTON_RMASK) != 0;
    const bool physicalMiddleMouseButtonHeld = (mouseButtons & SDL_BUTTON_MMASK) != 0;
    bool leftMouseButtonHeld = !blockGameplayInput && physicalLeftMouseButtonHeld;
    bool rightMouseButtonHeld = !blockGameplayInput && physicalRightMouseButtonHeld;
    bool middleMouseButtonHeld = !blockGameplayInput && physicalMiddleMouseButtonHeld;

    // The click guard after a window opened (guardClicksAfterWindowOpened): a press while it lasts, held or queued,
    // is dropped with its release.
    if (m_clickGuardUntilNanoseconds != 0)
    {
        const uint64_t now = SDL_GetTicksNS();
        const std::array<bool, 3> physicalHeld = {
            physicalLeftMouseButtonHeld, physicalMiddleMouseButtonHeld, physicalRightMouseButtonHeld};
        const std::array<bool *, 3> suppress = {&m_suppressLeftMouseButtonUntilReleased,
            &m_suppressMiddleMouseButtonUntilReleased, &m_suppressRightMouseButtonUntilReleased};

        for (size_t button = 0; button < suppress.size(); ++button)
        {
            const std::deque<SDL_MouseButtonEvent> &events = m_pendingMouseButtonEvents[button];
            const bool pressedInGuard = (physicalHeld[button] && now < m_clickGuardUntilNanoseconds)
                || std::any_of(events.begin(), events.end(),
                    [this](const SDL_MouseButtonEvent &event)
                    {
                        return event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                            && event.timestamp < m_clickGuardUntilNanoseconds;
                    });

            if (pressedInGuard)
            {
                *suppress[button] = true;
            }
        }

        if (now >= m_clickGuardUntilNanoseconds)
        {
            m_clickGuardUntilNanoseconds = 0;
        }
    }

#if defined(__ANDROID__) || defined(OPENYAMM_IOS)
    if (m_mobilePointerPosition)
    {
        m_frame.pointerX = (*m_mobilePointerPosition)[0] * static_cast<float>(screenWidth);
        m_frame.pointerY = (*m_mobilePointerPosition)[1] * static_cast<float>(screenHeight);
    }
#endif

#if !defined(__ANDROID__) && !defined(OPENYAMM_IOS)
    // A quick click, such as a trackpad tap, can press and release between two frames, and the polled state then
    // misses it. Apply queued button events one per frame at their own positions: the UI derives clicks and drags
    // from the held state of consecutive frames.
    const auto applyPendingMouseButton =
        [this, blockGameplayInput](size_t buttonIndex, bool suppressed, bool &held)
        {
            std::deque<SDL_MouseButtonEvent> &events = m_pendingMouseButtonEvents[buttonIndex];

            if (blockGameplayInput || suppressed)
            {
                events.clear();
                return;
            }

            if (!events.empty())
            {
                const SDL_MouseButtonEvent event = events.front();
                events.pop_front();
                held = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                m_frame.pointerX = event.x;
                m_frame.pointerY = event.y;
            }
        };

    applyPendingMouseButton(0, m_suppressLeftMouseButtonUntilReleased, leftMouseButtonHeld);
    applyPendingMouseButton(1, m_suppressMiddleMouseButtonUntilReleased, middleMouseButtonHeld);
    applyPendingMouseButton(2, m_suppressRightMouseButtonUntilReleased, rightMouseButtonHeld);

    SDL_Window *pMouseLookWindow = SDL_GetMouseFocus();

    if (pMouseLookWindow == nullptr)
    {
        pMouseLookWindow = SDL_GetKeyboardFocus();
    }

    if (settings.controlScheme == ControlScheme::Modern
        && pMouseLookWindow != nullptr
        && SDL_GetWindowRelativeMouseMode(pMouseLookWindow)
        && screenWidth > 0
        && screenHeight > 0)
    {
        m_frame.pointerX = static_cast<float>(screenWidth) * 0.5f;
        m_frame.pointerY = static_cast<float>(screenHeight) * 0.5f;
    }
#endif

#if defined(__ANDROID__) || defined(OPENYAMM_IOS)
    const bool physicalMouseMoved = false;
    m_frame.relativeMouseX = 0.0f;
    m_frame.relativeMouseY = 0.0f;
#else
    const float relativeMouseX = m_pendingRelativeMouseX;
    const float relativeMouseY = m_pendingRelativeMouseY;
    const bool physicalMouseMoved = relativeMouseX != 0.0f || relativeMouseY != 0.0f;
    m_pendingRelativeMouseX = 0.0f;
    m_pendingRelativeMouseY = 0.0f;
    const float mouseSensitivityScale = static_cast<float>(std::clamp(settings.mouseSensitivity, 0, 100)) / 100.0f;
    m_frame.relativeMouseX = blockGameplayInput ? 0.0f : relativeMouseX * mouseSensitivityScale;
    m_frame.relativeMouseY = blockGameplayInput ? 0.0f
        : (settings.invertMouseY ? -relativeMouseY : relativeMouseY) * mouseSensitivityScale;
#endif

    suppressButtonWhileHeld(
        physicalLeftMouseButtonHeld,
        leftMouseButtonHeld,
        m_suppressLeftMouseButtonUntilReleased);
    suppressButtonWhileHeld(
        physicalRightMouseButtonHeld,
        rightMouseButtonHeld,
        m_suppressRightMouseButtonUntilReleased);
    suppressButtonWhileHeld(
        physicalMiddleMouseButtonHeld,
        middleMouseButtonHeld,
        m_suppressMiddleMouseButtonUntilReleased);

    if (blockGameplayInput || !settings.autoHoldLeftClick)
    {
        m_autoHoldLeftClickLatched = false;
    }
    else if ((m_pendingAutoHoldLeftClickPresses & 1U) != 0)
    {
        m_autoHoldLeftClickLatched = !m_autoHoldLeftClickLatched;
    }
    m_pendingAutoHoldLeftClickPresses = 0;
    if (settings.autoHoldLeftClick)
    {
        leftMouseButtonHeld = !blockGameplayInput && m_autoHoldLeftClickLatched;
    }

    std::array<bool, KeyboardActionCount> actionHeld = {};

    for (const KeyboardBindingDefinition &definition : keyboardBindingDefinitions())
    {
        const InputBinding binding = settings.keyboard.binding(definition.action);
        bool held = false;

        if (binding.kind == InputBindingKind::Keyboard)
        {
            held =
                binding.scancode > SDL_SCANCODE_UNKNOWN
                && binding.scancode < SDL_SCANCODE_COUNT
                && m_frame.keyboardHeld[binding.scancode];
        }
        else if (binding.kind == InputBindingKind::MouseButton)
        {
            if (binding.mouseButton == SDL_BUTTON_LEFT)
            {
                held = leftMouseButtonHeld;
            }
            else if (binding.mouseButton == SDL_BUTTON_RIGHT)
            {
                held = rightMouseButtonHeld;
            }
            else if (binding.mouseButton == SDL_BUTTON_MIDDLE)
            {
                held = middleMouseButtonHeld;
            }
            else
            {
                held = !blockGameplayInput && mouseButtonHeldInFlags(mouseButtons, binding.mouseButton);
            }
        }
        else if (binding.kind == InputBindingKind::MouseWheelUp)
        {
            held = m_frame.mouseWheelDelta > 0.0f;
        }
        else if (binding.kind == InputBindingKind::MouseWheelDown)
        {
            held = m_frame.mouseWheelDelta < 0.0f;
        }

        actionHeld[keyboardActionIndex(definition.action)] = held;
    }

#if !defined(OPENYAMM_IOS)
    ensureGamepadOpen();
    if (!blockGameplayInput && settings.gamepad.enabled && m_pGamepad != nullptr)
    {
        const int deadZonePercent = std::clamp(settings.gamepad.deadZonePercent, 0, 95);
        const float lookSensitivityScale =
            static_cast<float>(std::clamp(settings.gamepad.lookSensitivity, 0, 200)) / 100.0f;
        const float leftX = gamepadAxisValue(
            SDL_GetGamepadAxis(m_pGamepad, SDL_GAMEPAD_AXIS_LEFTX), deadZonePercent);
        const float leftY = gamepadAxisValue(
            SDL_GetGamepadAxis(m_pGamepad, SDL_GAMEPAD_AXIS_LEFTY), deadZonePercent);
        const float rightX = gamepadAxisValue(
            SDL_GetGamepadAxis(m_pGamepad, SDL_GAMEPAD_AXIS_RIGHTX), deadZonePercent);
        const float rightY = gamepadAxisValue(
            SDL_GetGamepadAxis(m_pGamepad, SDL_GAMEPAD_AXIS_RIGHTY), deadZonePercent);
        std::array<bool, KeyboardActionCount> gamepadActionHeld = {};
        for (size_t actionIndex = 0; actionIndex < gamepadActionHeld.size(); ++actionIndex)
        {
            const GamepadBinding binding = settings.gamepad.bindings[actionIndex];
            if (binding.kind == GamepadBindingKind::Button)
            {
                gamepadActionHeld[actionIndex] = SDL_GetGamepadButton(m_pGamepad, binding.button);
            }
            else if (binding.kind == GamepadBindingKind::Trigger)
            {
                gamepadActionHeld[actionIndex] = gamepadAxisValue(
                    SDL_GetGamepadAxis(m_pGamepad, binding.axis), deadZonePercent) > GamepadTriggerThreshold;
            }
        }

        const size_t toggleIndex = keyboardActionIndex(KeyboardAction::ToggleCursorMode);
        const bool toggleHeld = actionHeld[toggleIndex] || gamepadActionHeld[toggleIndex];
        if (!menuNavigationEnabled && toggleHeld && !m_previousActionHeld[toggleIndex])
        {
            m_gamepadInteractCursorMode = !m_gamepadInteractCursorMode;
            m_gamepadCursorDriving = m_gamepadInteractCursorMode;
        }
        actionHeld[toggleIndex] = toggleHeld;
        m_frame.gamepadCursorModeActive = m_gamepadInteractCursorMode && !menuNavigationEnabled;

        if (menuNavigationEnabled || m_frame.gamepadCursorModeActive)
        {
            const float rightMagnitude = std::clamp(std::hypot(rightX, rightY), 0.0f, 1.0f);
            if (!m_gamepadCursorInitialized || m_gamepadCursorScreenWidth != screenWidth
                || m_gamepadCursorScreenHeight != screenHeight)
            {
                m_gamepadCursorX = std::clamp(m_frame.pointerX, 0.0f, static_cast<float>(std::max(0, screenWidth - 1)));
                m_gamepadCursorY = std::clamp(
                    m_frame.pointerY, 0.0f, static_cast<float>(std::max(0, screenHeight - 1)));
                if (screenWidth > 0 && screenHeight > 0 && m_gamepadCursorX == 0.0f && m_gamepadCursorY == 0.0f)
                {
                    m_gamepadCursorX = static_cast<float>(screenWidth) * 0.5f;
                    m_gamepadCursorY = static_cast<float>(screenHeight) * 0.5f;
                }
                m_gamepadCursorScreenWidth = screenWidth;
                m_gamepadCursorScreenHeight = screenHeight;
                m_gamepadCursorInitialized = true;
            }

            if (physicalMouseMoved && (!m_gamepadCursorDriving
                    || std::hypot(m_frame.pointerX - m_gamepadCursorX, m_frame.pointerY - m_gamepadCursorY) > 3.0f))
            {
                m_gamepadCursorDriving = false;
                m_gamepadMenuFocusMode = false;
                m_gamepadCursorX = m_frame.pointerX;
                m_gamepadCursorY = m_frame.pointerY;
            }

            if (rightMagnitude > 0.0f && lookSensitivityScale > 0.0f && screenWidth > 0 && screenHeight > 0)
            {
                m_gamepadCursorDriving = true;
                m_gamepadMenuFocusMode = false;
                const float speed = GamepadMenuCursorMinimumPixelsPerSecond
                    + (GamepadMenuCursorMaximumPixelsPerSecond - GamepadMenuCursorMinimumPixelsPerSecond)
                        * rightMagnitude * rightMagnitude;
                const float resolutionScale = std::max(0.75f, static_cast<float>(screenHeight) / 720.0f);
                const float stepSeconds = std::clamp(frameDeltaSeconds, 0.0f, 0.05f);
                m_gamepadCursorX = std::clamp(m_gamepadCursorX + rightX / rightMagnitude * speed
                        * lookSensitivityScale * resolutionScale * stepSeconds,
                    0.0f, static_cast<float>(screenWidth - 1));
                m_gamepadCursorY = std::clamp(m_gamepadCursorY + rightY / rightMagnitude * speed
                        * lookSensitivityScale * resolutionScale * stepSeconds,
                    0.0f, static_cast<float>(screenHeight - 1));
#if !defined(__ANDROID__)
                SDL_Window *pWindow = SDL_GetMouseFocus();
                if (pWindow == nullptr)
                {
                    pWindow = SDL_GetKeyboardFocus();
                }
                if (pWindow != nullptr)
                {
                    SDL_WarpMouseInWindow(pWindow, m_gamepadCursorX, m_gamepadCursorY);
                }
#endif
            }
            if (m_gamepadCursorDriving)
            {
                m_frame.pointerX = m_gamepadCursorX;
                m_frame.pointerY = m_gamepadCursorY;
            }

            if (menuNavigationEnabled)
            {
                std::array<bool, 4> directions = {
                    SDL_GetGamepadButton(m_pGamepad, SDL_GAMEPAD_BUTTON_DPAD_UP),
                    SDL_GetGamepadButton(m_pGamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN),
                    SDL_GetGamepadButton(m_pGamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT),
                    SDL_GetGamepadButton(m_pGamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)};
                if (!directions[0] && !directions[1] && !directions[2] && !directions[3])
                {
                    if (std::abs(leftY) >= std::abs(leftX) && std::abs(leftY) > GamepadMenuDigitalThreshold)
                    {
                        directions[leftY < 0.0f ? 0 : 1] = true;
                    }
                    else if (std::abs(leftX) > GamepadMenuDigitalThreshold)
                    {
                        directions[leftX < 0.0f ? 2 : 3] = true;
                    }
                }
                constexpr std::array<SDL_Scancode, 4> DirectionScancodes = {
                    SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT};
                const float repeatStep = std::clamp(frameDeltaSeconds, 0.0f, 0.1f);
                for (size_t direction = 0; direction < directions.size(); ++direction)
                {
                    const SDL_Scancode scancode = DirectionScancodes[direction];
                    m_frame.keyboardHeld[scancode] = directions[direction];
                    if (directions[direction])
                    {
                        m_gamepadMenuFocusMode = true;
                        bool press = !m_previousGamepadMenuDirections[direction];
                        if (press)
                        {
                            m_gamepadMenuRepeatSeconds[direction] = GamepadMenuInitialRepeatSeconds;
                        }
                        else
                        {
                            m_gamepadMenuRepeatSeconds[direction] -= repeatStep;
                            if (m_gamepadMenuRepeatSeconds[direction] <= 0.0f)
                            {
                                press = true;
                                m_gamepadMenuRepeatSeconds[direction] += GamepadMenuRepeatSeconds;
                            }
                        }
                        if (press && m_frame.keyboardPressCounts[scancode] < std::numeric_limits<uint16_t>::max())
                        {
                            ++m_frame.keyboardPressCounts[scancode];
                        }
                    }
                    else
                    {
                        m_gamepadMenuRepeatSeconds[direction] = 0.0f;
                    }
                    m_previousGamepadMenuDirections[direction] = directions[direction];
                }

                const bool acceptHeld = gamepadActionHeld[keyboardActionIndex(KeyboardAction::MenuAccept)];
                const bool backHeld = gamepadActionHeld[keyboardActionIndex(KeyboardAction::MenuBack)];
                if (m_gamepadMenuFocusMode)
                {
                    m_frame.keyboardHeld[SDL_SCANCODE_RETURN] = acceptHeld;
                    if (acceptHeld && !m_previousGamepadMenuAcceptHeld
                        && m_frame.keyboardPressCounts[SDL_SCANCODE_RETURN] < std::numeric_limits<uint16_t>::max())
                    {
                        ++m_frame.keyboardPressCounts[SDL_SCANCODE_RETURN];
                    }
                }
                else
                {
                    leftMouseButtonHeld |= acceptHeld;
                    rightMouseButtonHeld |=
                        gamepadActionHeld[keyboardActionIndex(KeyboardAction::MenuSecondary)];
                }
                m_frame.keyboardHeld[SDL_SCANCODE_ESCAPE] = backHeld;
                if (backHeld && !m_previousGamepadMenuBackHeld
                    && m_frame.keyboardPressCounts[SDL_SCANCODE_ESCAPE] < std::numeric_limits<uint16_t>::max())
                {
                    ++m_frame.keyboardPressCounts[SDL_SCANCODE_ESCAPE];
                }
                m_previousGamepadMenuAcceptHeld = acceptHeld;
                m_previousGamepadMenuBackHeld = backHeld;
            }
            else
            {
                m_gamepadMenuFocusMode = false;
                m_previousGamepadMenuDirections.fill(false);
                m_gamepadMenuRepeatSeconds.fill(0.0f);
                m_previousGamepadMenuAcceptHeld = false;
                m_previousGamepadMenuBackHeld = false;
                leftMouseButtonHeld |= gamepadActionHeld[keyboardActionIndex(KeyboardAction::MenuAccept)];
                rightMouseButtonHeld |= gamepadActionHeld[keyboardActionIndex(KeyboardAction::MenuSecondary)];
            }
        }
        else
        {
            m_gamepadMenuFocusMode = false;
            m_gamepadCursorDriving = false;
            m_previousGamepadMenuDirections.fill(false);
            m_gamepadMenuRepeatSeconds.fill(0.0f);
            m_previousGamepadMenuAcceptHeld = false;
            m_previousGamepadMenuBackHeld = false;

            for (size_t actionIndex = 0; actionIndex < gamepadActionHeld.size(); ++actionIndex)
            {
                const KeyboardAction action = static_cast<KeyboardAction>(actionIndex);
                if (action != KeyboardAction::MenuAccept
                    && action != KeyboardAction::MenuBack
                    && action != KeyboardAction::MenuSecondary)
                {
                    actionHeld[actionIndex] |= gamepadActionHeld[actionIndex];
                }
            }

            const bool keyboardMovement =
                actionHeld[keyboardActionIndex(KeyboardAction::Forward)]
                || actionHeld[keyboardActionIndex(KeyboardAction::Backward)]
                || actionHeld[keyboardActionIndex(KeyboardAction::Left)]
                || actionHeld[keyboardActionIndex(KeyboardAction::Right)];
            actionHeld[keyboardActionIndex(KeyboardAction::Forward)] |= leftY < -GamepadDigitalThreshold;
            actionHeld[keyboardActionIndex(KeyboardAction::Backward)] |= leftY > GamepadDigitalThreshold;
            actionHeld[keyboardActionIndex(KeyboardAction::Left)] |= leftX < -GamepadDigitalThreshold;
            actionHeld[keyboardActionIndex(KeyboardAction::Right)] |= leftX > GamepadDigitalThreshold;
            const float movementMagnitude = std::clamp(std::hypot(leftX, leftY), 0.0f, 1.0f);
            if (!keyboardMovement && movementMagnitude > 0.0f)
            {
                m_frame.movementSpeedScale = movementMagnitude;
            }

            const float lookScale = GamepadLookPixelsPerSecond * lookSensitivityScale
                * std::clamp(frameDeltaSeconds, 0.0f, 0.05f);
            m_frame.relativeMouseX += rightX * lookScale;
            m_frame.relativeMouseY += (settings.gamepad.invertLookY ? -rightY : rightY) * lookScale;
        }
    }
    else
    {
        m_frame.gamepadCursorModeActive = false;
        if (!settings.gamepad.enabled)
        {
            m_gamepadInteractCursorMode = false;
        }
        m_gamepadMenuFocusMode = false;
        m_gamepadCursorDriving = false;
        m_previousGamepadMenuDirections.fill(false);
        m_gamepadMenuRepeatSeconds.fill(0.0f);
        m_previousGamepadMenuAcceptHeld = false;
        m_previousGamepadMenuBackHeld = false;
    }
#endif

#if defined(__ANDROID__) || defined(OPENYAMM_IOS)
    const bool useMobileGameplayTouchControls = mobileGameplayTouchControlsEnabled && !blockGameplayInput;
    const bool useMobileJumpGesture = useMobileGameplayTouchControls && mobileJumpGestureEnabled;
    const auto setMobileActionHeld =
        [&settings, &actionHeld, this](KeyboardAction action)
        {
            actionHeld[keyboardActionIndex(action)] = true;

            const SDL_Scancode scancode = settings.keyboard.keyboardBinding(action);
            if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT)
            {
                m_frame.keyboardHeld[scancode] = true;
            }
        };
    for (MobileTouchPoint &touch : m_mobileTouches)
    {
        if (!mobileFlightControlsEnabled
            && (touch.role == MobileTouchRole::FlyUp || touch.role == MobileTouchRole::FlyDown))
        {
            touch.role = MobileTouchRole::None;
            touch.startX = touch.x;
            touch.startY = touch.y;
            touch.deltaX = 0.0f;
            touch.deltaY = 0.0f;
        }

    }

    const float touchScale = mobileLogicalScale(screenHeight);
    const auto touchLogicalX =
        [screenWidth, touchScale](float normalizedX) -> float
        {
            return touchScale > 0.0f ? normalizedX * static_cast<float>(screenWidth) / touchScale : 0.0f;
        };
    const auto touchLogicalY =
        [screenHeight, touchScale](float normalizedY) -> float
        {
            return touchScale > 0.0f ? normalizedY * static_cast<float>(screenHeight) / touchScale : 0.0f;
        };
    // Drawing and touch capture use the same resolved YAML rectangles, including runtime party width.
    const auto controlAt = [touchControls, touchScale](float logicalX, float logicalY, GameplayTouchRole role)
    {
        return std::any_of(touchControls.begin(), touchControls.end(), [&](const GameplayTouchControl &control)
        {
            return control.role == role && control.contains(logicalX * touchScale, logicalY * touchScale);
        });
    };
    const auto touchStartsInHudZone =
        [&controlAt, &touchLogicalX, &touchLogicalY](float normalizedX, float normalizedY)
        {
            return controlAt(touchLogicalX(normalizedX), touchLogicalY(normalizedY), GameplayTouchRole::Hud);
        };
    const auto mobileInspectButtonAt = [&controlAt](float logicalX, float logicalY)
    {
        return controlAt(logicalX, logicalY, GameplayTouchRole::Inspect);
    };
    const auto mobileFlightControlAt =
        [&controlAt, mobileFlightControlsEnabled](float logicalX, float logicalY) -> MobileTouchRole
        {
            if (mobileFlightControlsEnabled)
            {
                if (controlAt(logicalX, logicalY, GameplayTouchRole::FlyUp))
                {
                    return MobileTouchRole::FlyUp;
                }
                if (controlAt(logicalX, logicalY, GameplayTouchRole::FlyDown))
                {
                    return MobileTouchRole::FlyDown;
                }
            }
            return MobileTouchRole::None;
        };

    bool inspectModifierHeld = false;
    bool longPressInspectHeld = false;
    const uint64_t nowNanoseconds = SDL_GetTicksNS();

    for (MobileTouchPoint &touch : m_mobileTouches)
    {
        if (!touch.active)
        {
            continue;
        }

        // A still finger held on an inspectable overlay becomes the right button at that finger.
        if (!blockGameplayInput
            && mobileLongPressInspectEnabled
            && touch.role == MobileTouchRole::Hud
            && touch.longPressCandidate
            && !touch.dragThresholdExceeded
            && nowNanoseconds >= touch.startTimestampNanoseconds
            && nowNanoseconds - touch.startTimestampNanoseconds >= MobileLongPressInspectNanoseconds)
        {
            touch.role = MobileTouchRole::LongPressInspect;
        }

        if (touch.role == MobileTouchRole::LongPressInspect)
        {
            longPressInspectHeld = true;
        }

        if (!blockGameplayInput
            && mobileInspectControlEnabled
            && touch.role == MobileTouchRole::None
            && mobileInspectButtonAt(touchLogicalX(touch.startX), touchLogicalY(touch.startY)))
        {
            touch.role = MobileTouchRole::InspectModifier;
        }

        if (!blockGameplayInput
            && mobileInspectControlEnabled
            && touch.role == MobileTouchRole::InspectModifier)
        {
            inspectModifierHeld = true;
        }
    }

    if (inspectModifierHeld)
    {
        for (MobileTouchPoint &touch : m_mobileTouches)
        {
            if (touch.active
                && touch.role != MobileTouchRole::InspectModifier
                && touch.role != MobileTouchRole::DebugConsoleGesture)
            {
                touch.role = MobileTouchRole::InspectTarget;
            }
        }

        m_mobilePendingHudTap = false;
        m_mobilePendingHudRelease = false;
        m_mobilePendingHudDragRelease = false;
        m_mobilePendingCameraTap = false;
        m_mobileJumpDoubleTapGesture.cancel();
    }

    bool hasMovementTouch = false;
    bool hasCameraTouch = false;

    for (const MobileTouchPoint &touch : m_mobileTouches)
    {
        if (touch.active && touch.role == MobileTouchRole::Movement)
        {
            hasMovementTouch = true;
        }
        else if (touch.active && touch.role == MobileTouchRole::Camera)
        {
            hasCameraTouch = true;
        }
    }

    for (MobileTouchPoint &touch : m_mobileTouches)
    {
        if (!touch.active || touch.role != MobileTouchRole::None)
        {
            continue;
        }

        const float startLogicalX = touchLogicalX(touch.startX);
        const float startLogicalY = touchLogicalY(touch.startY);
        const bool startsInMovementZone = pointInsideRect(
            startLogicalX,
            startLogicalY,
            MobileMovementZoneX,
            MobileMovementZoneY,
            MobileMovementZoneWidth,
            MobileMovementZoneHeight);
        const bool startsInHudZone = touchStartsInHudZone(touch.startX, touch.startY);
        const bool startsInCameraZone = startLogicalX >= MobileCameraZoneX;
        const MobileTouchRole flightControl = mobileFlightControlAt(startLogicalX, startLogicalY);

        if (!useMobileGameplayTouchControls)
        {
            touch.role = MobileTouchRole::Hud;
            touch.longPressCandidate = mobileLongPressInspectEnabled && !blockGameplayInput;
        }
        else if (flightControl != MobileTouchRole::None)
        {
            touch.role = flightControl;
        }
        else if (startsInHudZone)
        {
            touch.role = MobileTouchRole::Hud;
        }
        else if (startsInMovementZone && !hasMovementTouch)
        {
            touch.role = MobileTouchRole::Movement;
            hasMovementTouch = true;
        }
        else if (startsInCameraZone && !hasCameraTouch)
        {
            touch.role = MobileTouchRole::Camera;
            hasCameraTouch = true;
        }
        else
        {
            touch.role = MobileTouchRole::Hud;
        }

    }

    bool hasNonHudMobileTouch = false;
    bool hasHudTouch = false;
    bool hasDeferredHudTouch = false;
    bool hasInspectTargetTouch = false;
    float hudTouchX = 0.0f;
    float hudTouchY = 0.0f;
    float deferredHudTouchX = 0.0f;
    float deferredHudTouchY = 0.0f;
    float inspectTargetX = 0.0f;
    float inspectTargetY = 0.0f;

    for (MobileTouchPoint &touch : m_mobileTouches)
    {
        if (!touch.active)
        {
            continue;
        }

        if (touch.role == MobileTouchRole::FlyUp || touch.role == MobileTouchRole::FlyDown)
        {
            hasNonHudMobileTouch = true;

            if (useMobileGameplayTouchControls && mobileFlightControlsEnabled)
            {
                setMobileActionHeld(
                    touch.role == MobileTouchRole::FlyUp ? KeyboardAction::FlyUp : KeyboardAction::FlyDown);
            }
        }
        else if (touch.role == MobileTouchRole::Movement)
        {
            hasNonHudMobileTouch = true;
            const float startX = touch.startX * static_cast<float>(screenWidth);
            const float startY = touch.startY * static_cast<float>(screenHeight);
            const float deltaLogicalX = touchLogicalX(touch.x) - touchLogicalX(touch.startX);
            const float deltaLogicalY = touchLogicalY(touch.y) - touchLogicalY(touch.startY);
            const float distance = std::sqrt(deltaLogicalX * deltaLogicalX + deltaLogicalY * deltaLogicalY);
            const float clampedDistance = std::min(distance, MobileJoystickRadius);
            float clampedLogicalX = 0.0f;
            float clampedLogicalY = 0.0f;

            if (distance > 0.001f)
            {
                clampedLogicalX = deltaLogicalX * clampedDistance / distance;
                clampedLogicalY = deltaLogicalY * clampedDistance / distance;
            }

            m_frame.mobileJoystickActive = true;
            m_frame.mobileJoystickBaseX = startX;
            m_frame.mobileJoystickBaseY = startY;
            m_frame.mobileJoystickKnobX = startX + clampedLogicalX * touchScale;
            m_frame.mobileJoystickKnobY = startY + clampedLogicalY * touchScale;
            m_frame.movementSpeedScale = mobileJoystickMovementSpeedScale(
                deltaLogicalX,
                deltaLogicalY,
                MobileJoystickDeadZone,
                MobileJoystickFullSpeedRadius);

            const MobileJoystickDirection joystickDirection = quantizeMobileJoystickDirection(
                deltaLogicalX,
                deltaLogicalY,
                MobileJoystickDeadZone);

            switch (joystickDirection)
            {
                case MobileJoystickDirection::North:
                    setMobileActionHeld(KeyboardAction::Forward);
                    break;
                case MobileJoystickDirection::NorthEast:
                    setMobileActionHeld(KeyboardAction::Forward);
                    setMobileActionHeld(KeyboardAction::Right);
                    break;
                case MobileJoystickDirection::East:
                    setMobileActionHeld(KeyboardAction::Right);
                    break;
                case MobileJoystickDirection::SouthEast:
                    setMobileActionHeld(KeyboardAction::Backward);
                    setMobileActionHeld(KeyboardAction::Right);
                    break;
                case MobileJoystickDirection::South:
                    setMobileActionHeld(KeyboardAction::Backward);
                    break;
                case MobileJoystickDirection::SouthWest:
                    setMobileActionHeld(KeyboardAction::Backward);
                    setMobileActionHeld(KeyboardAction::Left);
                    break;
                case MobileJoystickDirection::West:
                    setMobileActionHeld(KeyboardAction::Left);
                    break;
                case MobileJoystickDirection::NorthWest:
                    setMobileActionHeld(KeyboardAction::Forward);
                    setMobileActionHeld(KeyboardAction::Left);
                    break;
                case MobileJoystickDirection::Neutral:
                    break;
            }
        }
        else if (touch.role == MobileTouchRole::Camera)
        {
            hasNonHudMobileTouch = true;
            m_frame.relativeMouseX += touch.deltaX * static_cast<float>(screenWidth);
            m_frame.relativeMouseY += touch.deltaY * static_cast<float>(screenHeight);

            if (touch.dragThresholdExceeded)
            {
                m_mobileJumpDoubleTapGesture.cancel();
            }
        }
        else if (touch.role == MobileTouchRole::InspectModifier)
        {
            hasNonHudMobileTouch = true;
        }
        else if ((touch.role == MobileTouchRole::InspectTarget || touch.role == MobileTouchRole::LongPressInspect)
            && !hasInspectTargetTouch)
        {
            hasNonHudMobileTouch = true;
            hasInspectTargetTouch = true;
            inspectTargetX = touch.x * static_cast<float>(screenWidth);
            inspectTargetY = touch.y * static_cast<float>(screenHeight);
        }
        else if (touch.role == MobileTouchRole::Hud
            && touch.longPressCandidate
            && !touch.dragThresholdExceeded
            && !hasHudTouch
            && !hasDeferredHudTouch)
        {
            // Undecided between a tap (clicks on release) and a long press (inspects); only the pointer follows.
            hasDeferredHudTouch = true;
            deferredHudTouchX = touch.x * static_cast<float>(screenWidth);
            deferredHudTouchY = touch.y * static_cast<float>(screenHeight);
        }
        else if (touch.role == MobileTouchRole::Hud && !hasHudTouch)
        {
            hasHudTouch = true;
            // A deferred press reaches the item under the finger's start when its drag begins.
            const bool pressAtStart = touch.longPressCandidate && !touch.dragStartDelivered;
            hudTouchX = (pressAtStart ? touch.startX : touch.x) * static_cast<float>(screenWidth);
            hudTouchY = (pressAtStart ? touch.startY : touch.y) * static_cast<float>(screenHeight);

            if (touch.dragThresholdExceeded)
            {
                m_frame.mobileTouchDragActive = true;
                m_frame.mobileTouchDragStartX = touch.startX * static_cast<float>(screenWidth);
                m_frame.mobileTouchDragStartY = touch.startY * static_cast<float>(screenHeight);

                if (!touch.dragStartDelivered)
                {
                    m_frame.mobileTouchDragStarted = true;
                    touch.dragStartDelivered = true;
                }
            }
        }
        else if (touch.role == MobileTouchRole::DebugConsoleGesture)
        {
            hasNonHudMobileTouch = true;
        }

        touch.deltaX = 0.0f;
        touch.deltaY = 0.0f;
    }

    const bool pendingHudDragIsUiGesture =
        m_mobilePendingHudDragRelease
        && (!useMobileGameplayTouchControls
            || touchStartsInHudZone(m_mobilePendingHudDragStartX, m_mobilePendingHudDragStartY));
    const bool hasMobilePointerInput = inspectModifierHeld || longPressInspectHeld || hasHudTouch
        || hasDeferredHudTouch || pendingHudDragIsUiGesture || m_mobilePendingHudTap || m_mobilePendingHudRelease;

    if (inspectModifierHeld || longPressInspectHeld)
    {
        if (hasInspectTargetTouch)
        {
            m_frame.pointerX = inspectTargetX;
            m_frame.pointerY = inspectTargetY;
        }

        leftMouseButtonHeld = false;
        rightMouseButtonHeld = true;
    }
    else if (pendingHudDragIsUiGesture)
    {
        m_frame.mobileTouchDragStartX = m_mobilePendingHudDragStartX * static_cast<float>(screenWidth);
        m_frame.mobileTouchDragStartY = m_mobilePendingHudDragStartY * static_cast<float>(screenHeight);
        if (!m_mobilePendingHudDragStartDelivered)
        {
            // Deliver the pickup before the drop even when the whole gesture fits between frames.
            m_frame.pointerX = m_frame.mobileTouchDragStartX;
            m_frame.pointerY = m_frame.mobileTouchDragStartY;
            m_frame.mobileTouchDragStarted = true;
            m_frame.mobileTouchDragActive = true;
            leftMouseButtonHeld = true;
            m_mobilePendingHudDragStartDelivered = true;
        }
        else
        {
            m_frame.pointerX = m_mobilePendingHudDragReleaseX * static_cast<float>(screenWidth);
            m_frame.pointerY = m_mobilePendingHudDragReleaseY * static_cast<float>(screenHeight);
            m_frame.mobileTouchDragReleased = true;
            leftMouseButtonHeld = false;
            m_mobilePendingHudDragRelease = false;
        }
    }
    else if (m_mobilePendingHudDragRelease)
    {
        m_mobilePendingHudDragRelease = false;
    }
    else if (hasHudTouch)
    {
        m_frame.pointerX = hudTouchX;
        m_frame.pointerY = hudTouchY;
        leftMouseButtonHeld = true;
        m_mobilePendingHudRelease = false;
    }
    else if (m_mobilePendingHudTap)
    {
        if (!useMobileGameplayTouchControls
            || touchStartsInHudZone(m_mobilePendingHudTapStartX, m_mobilePendingHudTapStartY))
        {
            m_frame.pointerX = m_mobilePendingHudTapX * static_cast<float>(screenWidth);
            m_frame.pointerY = m_mobilePendingHudTapY * static_cast<float>(screenHeight);
            leftMouseButtonHeld = true;
            m_mobilePendingHudRelease = true;
            m_mobilePendingHudReleaseX = m_mobilePendingHudTapX;
            m_mobilePendingHudReleaseY = m_mobilePendingHudTapY;
        }

        m_mobilePendingHudTap = false;
    }
    else if (m_mobilePendingHudRelease)
    {
        m_frame.pointerX = m_mobilePendingHudReleaseX * static_cast<float>(screenWidth);
        m_frame.pointerY = m_mobilePendingHudReleaseY * static_cast<float>(screenHeight);
        leftMouseButtonHeld = false;
        m_mobilePendingHudRelease = false;
    }
    else if (hasDeferredHudTouch)
    {
        m_frame.pointerX = deferredHudTouchX;
        m_frame.pointerY = deferredHudTouchY;
        leftMouseButtonHeld = false;
    }
    else if (m_mobilePendingCameraTap)
    {
        if (useMobileJumpGesture)
        {
            const float logicalX = touchLogicalX(m_mobilePendingCameraTapX);
            const float logicalY = touchLogicalY(m_mobilePendingCameraTapY);

            if (m_mobileJumpDoubleTapGesture.registerCameraTap(
                    m_mobilePendingCameraTapTimestampNanoseconds,
                    logicalX,
                    logicalY))
            {
                setMobileActionHeld(KeyboardAction::Jump);
            }
        }

        m_mobilePendingCameraTap = false;
    }

    if (!useMobileJumpGesture)
    {
        m_mobilePendingCameraTap = false;
        m_mobileJumpDoubleTapGesture.cancel();
    }

    if (hasNonHudMobileTouch && !hasHudTouch)
    {
        leftMouseButtonHeld = false;
    }

    if (hasMobilePointerInput && screenWidth > 0 && screenHeight > 0)
    {
        m_mobilePointerPosition = std::array<float, 2>{
            m_frame.pointerX / static_cast<float>(screenWidth),
            m_frame.pointerY / static_cast<float>(screenHeight)};
    }
#else
    static_cast<void>(mobileGameplayTouchControlsEnabled);
    static_cast<void>(mobileJumpGestureEnabled);
    static_cast<void>(mobileFlightControlsEnabled);
    static_cast<void>(mobileInspectControlEnabled);
    static_cast<void>(mobileLongPressInspectEnabled);
    static_cast<void>(touchControls);
#endif

    m_frame.leftMouseButton = buildButtonState(leftMouseButtonHeld, m_previousLeftMouseButtonHeld);
    m_frame.rightMouseButton = buildButtonState(rightMouseButtonHeld, m_previousRightMouseButtonHeld);
    m_frame.middleMouseButton = buildButtonState(middleMouseButtonHeld, m_previousMiddleMouseButtonHeld);

    for (size_t actionIndex = 0; actionIndex < actionHeld.size(); ++actionIndex)
    {
        m_frame.actions[actionIndex] = buildButtonState(actionHeld[actionIndex], m_previousActionHeld[actionIndex]);
        const InputBindingKind kind = settings.keyboard.bindings[actionIndex].kind;
        if (kind == InputBindingKind::MouseWheelUp || kind == InputBindingKind::MouseWheelDown)
        {
            m_frame.actions[actionIndex].pressed = actionHeld[actionIndex];
        }
    }

    m_previousKeyboardHeld = m_frame.keyboardHeld;
    m_previousActionHeld = actionHeld;
    m_previousLeftMouseButtonHeld = leftMouseButtonHeld;
    m_previousRightMouseButtonHeld = rightMouseButtonHeld;
    m_previousMiddleMouseButtonHeld = middleMouseButtonHeld;
}

const GameplayInputFrame &GameInputSystem::frame() const
{
    return m_frame;
}

GameplayInputFrame &GameInputSystem::frame()
{
    return m_frame;
}

void GameInputSystem::resetRelativeMouseMotion()
{
    m_pendingRelativeMouseX = 0.0f;
    m_pendingRelativeMouseY = 0.0f;
    m_frame.relativeMouseX = 0.0f;
    m_frame.relativeMouseY = 0.0f;
}

void GameInputSystem::suppressMouseButtonsUntilReleased()
{
    m_suppressLeftMouseButtonUntilReleased = true;
    m_suppressRightMouseButtonUntilReleased = true;
    m_suppressMiddleMouseButtonUntilReleased = true;
    m_previousLeftMouseButtonHeld = false;
    m_previousRightMouseButtonHeld = false;
    m_previousMiddleMouseButtonHeld = false;
    m_pendingAutoHoldLeftClickPresses = 0;
    m_autoHoldLeftClickLatched = false;
}

void GameInputSystem::guardClicksAfterWindowOpened()
{
    const uint64_t now = SDL_GetTicksNS();
    const bool buttonHeld = SDL_GetMouseState(nullptr, nullptr) != 0;
    const bool recentClick = m_lastMouseButtonDownNanoseconds != 0
        && now - std::min(now, m_lastMouseButtonDownNanoseconds) <= ClickOpenedWindowNanoseconds;

    if (!buttonHeld && !recentClick)
    {
        return;
    }

    suppressMouseButtonsUntilReleased();
    m_clickGuardUntilNanoseconds = now + WindowClickGuardNanoseconds;
}
} // namespace OpenYAMM::Game
