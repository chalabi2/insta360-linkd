#pragma once

#include "linkd/camera.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace linkd {

class CameraController {
public:
    explicit CameraController(std::unique_ptr<CameraBackend> camera);
    ~CameraController();

    CameraController(const CameraController&) = delete;
    CameraController& operator=(const CameraController&) = delete;

    [[nodiscard]] const UsbIdentity& identity() const noexcept;
    [[nodiscard]] const CameraCapabilities& capabilities() const noexcept;
    [[nodiscard]] CameraState status();
    [[nodiscard]] CameraState cached_status() const;
    [[nodiscard]] std::optional<std::string> background_error() const;

    void center();
    void set_position(Position position);
    void set_zoom(ZoomRatio zoom);
    void set_tracking(bool enabled);
    void set_velocity(Velocity velocity);

private:
    void motion_loop();

    std::unique_ptr<CameraBackend> camera_;
    mutable std::mutex mutex_;
    std::optional<CameraState> state_;
    Velocity velocity_;
    std::chrono::steady_clock::time_point velocity_expires_at_{};
    std::optional<std::string> background_error_;
    std::atomic_bool stop_requested_{false};
    std::thread motion_thread_;
};

}  // namespace linkd
