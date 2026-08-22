#include "linkd/gamepad.hpp"

#include "linkd/controller.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace linkd {
namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(16);
constexpr auto kReconnectInterval = std::chrono::seconds(1);

NormalizedAxis normalized_axis(std::int16_t raw) {
    const double value = raw >= 0
        ? static_cast<double>(raw) / 32767.0
        : static_cast<double>(raw) / 32768.0;
    return *NormalizedAxis::from(std::clamp(value, -1.0, 1.0));
}

double positive_axis(std::int16_t raw) {
    return std::clamp(static_cast<double>(raw) / 32767.0, 0.0, 1.0);
}

DegreesPerSecond speed(double value) {
    return *DegreesPerSecond::from(value);
}

GamepadMapping default_mapping() {
    return {
        .curve = {.deadzone = 0.12, .exponent = 2.0},
        .maximum_pan_speed = speed(90.0),
        .maximum_tilt_speed = speed(60.0),
        .invert_pan = false,
        .invert_tilt = true,
    };
}

}  // namespace

class GamepadInput::Implementation {
public:
    explicit Implementation(CameraController& controller)
        : controller_(controller),
          thread_([this] { run(); }) {}

    ~Implementation() {
        stop_requested_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void open_gamepad() {
        int count = 0;
        SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
        if (gamepads == nullptr) {
            return;
        }
        for (int index = 0; index < count && gamepad_ == nullptr; ++index) {
            gamepad_ = SDL_OpenGamepad(gamepads[index]);
        }
        SDL_free(gamepads);

        if (gamepad_ != nullptr) {
            const char* name = SDL_GetGamepadName(gamepad_);
            std::cerr << "linkd: gamepad connected: "
                      << (name != nullptr ? name : "unknown gamepad") << '\n';
            zoom_ = controller_.status().zoom.value();
        }
    }

    void close_gamepad() {
        if (gamepad_ != nullptr) {
            SDL_CloseGamepad(gamepad_);
            gamepad_ = nullptr;
            std::cerr << "linkd: gamepad disconnected\n";
        }
    }

    void poll_gamepad(double seconds) {
        const auto horizontal = normalized_axis(SDL_GetGamepadAxis(
            gamepad_, SDL_GAMEPAD_AXIS_LEFTX));
        const auto vertical = normalized_axis(SDL_GetGamepadAxis(
            gamepad_, SDL_GAMEPAD_AXIS_LEFTY));
        const Velocity velocity = map_gamepad(horizontal, vertical, mapping_);
        const bool stick_active = std::abs(velocity.pan.value()) > 0.0001 ||
            std::abs(velocity.tilt.value()) > 0.0001;
        if (stick_active || stick_was_active_) {
            controller_.set_velocity(velocity);
        }
        stick_was_active_ = stick_active;

        const double zoom_out = positive_axis(SDL_GetGamepadAxis(
            gamepad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        const double zoom_in = positive_axis(SDL_GetGamepadAxis(
            gamepad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        const double zoom_velocity = zoom_in - zoom_out;
        if (std::abs(zoom_velocity) > 0.05) {
            const auto& range = controller_.capabilities().zoom;
            zoom_ = std::clamp(
                zoom_ + zoom_velocity * seconds,
                range.minimum.value(),
                range.maximum.value());
            controller_.set_zoom(*ZoomRatio::from(zoom_));
        }

        const bool center_pressed = SDL_GetGamepadButton(
            gamepad_, SDL_GAMEPAD_BUTTON_SOUTH);
        if (center_pressed && !center_was_pressed_) {
            controller_.center();
        }
        center_was_pressed_ = center_pressed;

        const bool tracking_pressed = SDL_GetGamepadButton(
            gamepad_, SDL_GAMEPAD_BUTTON_NORTH);
        if (tracking_pressed && !tracking_was_pressed_) {
            const bool enabled = !controller_.status().tracking_enabled;
            controller_.set_tracking(enabled);
        }
        tracking_was_pressed_ = tracking_pressed;
    }

    void run() {
        if (!SDL_Init(SDL_INIT_GAMEPAD)) {
            std::cerr << "linkd: SDL gamepad initialization failed: " << SDL_GetError() << '\n';
            return;
        }

        auto next_reconnect = std::chrono::steady_clock::now();
        auto previous = next_reconnect;
        while (!stop_requested_.load()) {
            SDL_PumpEvents();
            const auto now = std::chrono::steady_clock::now();

            if (gamepad_ != nullptr && !SDL_GamepadConnected(gamepad_)) {
                close_gamepad();
            }
            if (gamepad_ == nullptr && now >= next_reconnect) {
                try {
                    open_gamepad();
                } catch (const std::exception& error) {
                    std::cerr << "linkd: gamepad setup failed: " << error.what() << '\n';
                }
                next_reconnect = now + kReconnectInterval;
            }
            if (gamepad_ != nullptr) {
                try {
                    poll_gamepad(std::chrono::duration<double>(now - previous).count());
                } catch (const std::exception& error) {
                    std::cerr << "linkd: gamepad command failed: " << error.what() << '\n';
                }
            }
            previous = now;
            std::this_thread::sleep_for(kPollInterval);
        }

        close_gamepad();
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }

    CameraController& controller_;
    GamepadMapping mapping_ = default_mapping();
    SDL_Gamepad* gamepad_ = nullptr;
    double zoom_ = 1.0;
    bool center_was_pressed_ = false;
    bool tracking_was_pressed_ = false;
    bool stick_was_active_ = false;
    std::atomic_bool stop_requested_{false};
    std::thread thread_;
};

GamepadInput::GamepadInput(CameraController& controller)
    : implementation_(std::make_unique<Implementation>(controller)) {}

GamepadInput::~GamepadInput() = default;

}  // namespace linkd
