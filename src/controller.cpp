#include "linkd/controller.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace linkd {
namespace {

constexpr auto kMotionInterval = std::chrono::milliseconds(50);
constexpr auto kVelocityLease = std::chrono::milliseconds(250);

Degrees degrees(double value) {
    return *Degrees::from(value);
}

DegreesPerSecond degrees_per_second(double value) {
    return *DegreesPerSecond::from(value);
}

Velocity stopped_velocity() {
    return {degrees_per_second(0.0), degrees_per_second(0.0)};
}

bool is_moving(Velocity velocity) {
    return std::abs(velocity.pan.value()) > 0.0001 ||
        std::abs(velocity.tilt.value()) > 0.0001;
}

}  // namespace

CameraController::CameraController(std::unique_ptr<CameraBackend> camera)
    : camera_(std::move(camera)),
      velocity_(stopped_velocity()) {
    state_ = camera_->read_state();
    motion_thread_ = std::thread([this] { motion_loop(); });
}

CameraController::~CameraController() {
    stop_requested_.store(true);
    if (motion_thread_.joinable()) {
        motion_thread_.join();
    }
}

const UsbIdentity& CameraController::identity() const noexcept {
    return camera_->identity();
}

const CameraCapabilities& CameraController::capabilities() const noexcept {
    return camera_->capabilities();
}

CameraState CameraController::status() {
    std::scoped_lock lock(mutex_);
    state_ = camera_->read_state();
    return *state_;
}

CameraState CameraController::cached_status() const {
    std::scoped_lock lock(mutex_);
    return *state_;
}

std::optional<std::string> CameraController::background_error() const {
    std::scoped_lock lock(mutex_);
    return background_error_;
}

void CameraController::center() {
    set_position({degrees(0.0), degrees(0.0)});
}

void CameraController::set_position(Position position) {
    std::scoped_lock lock(mutex_);
    velocity_ = stopped_velocity();
    if (state_->tracking_enabled) {
        camera_->set_tracking(false);
    }
    camera_->set_position(position);
    state_->position = position;
    state_->tracking_enabled = false;
    background_error_.reset();
}

void CameraController::set_zoom(ZoomRatio zoom) {
    std::scoped_lock lock(mutex_);
    camera_->set_zoom(zoom);
    state_->zoom = zoom;
    background_error_.reset();
}

void CameraController::set_tracking(bool enabled) {
    std::scoped_lock lock(mutex_);
    velocity_ = stopped_velocity();
    camera_->set_tracking(enabled);
    state_->tracking_enabled = enabled;
    background_error_.reset();
}

void CameraController::set_velocity(Velocity velocity) {
    std::scoped_lock lock(mutex_);
    velocity_ = velocity;
    velocity_expires_at_ = std::chrono::steady_clock::now() + kVelocityLease;
}

void CameraController::motion_loop() {
    auto previous = std::chrono::steady_clock::now();
    bool was_moving = false;

    while (!stop_requested_.load()) {
        std::this_thread::sleep_for(kMotionInterval);
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(mutex_);

        const Velocity active_velocity = now <= velocity_expires_at_
            ? velocity_
            : stopped_velocity();
        if (!is_moving(active_velocity)) {
            was_moving = false;
            previous = now;
            continue;
        }

        try {
            if (!was_moving) {
                state_ = camera_->read_state();
                if (state_->tracking_enabled) {
                    camera_->set_tracking(false);
                    state_->tracking_enabled = false;
                }
                previous = now;
                was_moving = true;
            }

            const auto elapsed = std::min(
                std::chrono::duration<double>(now - previous),
                std::chrono::duration<double>(0.1));
            const Position next = advance_position(
                state_->position, active_velocity, elapsed, camera_->capabilities());
            camera_->set_position(next);
            state_->position = next;
            background_error_.reset();
            previous = now;
        } catch (const std::exception& error) {
            velocity_ = stopped_velocity();
            background_error_ = error.what();
            was_moving = false;
            previous = now;
        }
    }
}

}  // namespace linkd
