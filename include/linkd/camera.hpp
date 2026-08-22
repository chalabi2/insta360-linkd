#pragma once

#include "linkd/control.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace linkd {

struct UsbIdentity {
    std::uint16_t vendor_id;
    std::uint16_t product_id;
    std::string product_name;
};

struct CameraState {
    Position position;
    ZoomRatio zoom;
    bool tracking_enabled;
};

class CameraError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class CameraBackend {
public:
    virtual ~CameraBackend() = default;

    [[nodiscard]] virtual const UsbIdentity& identity() const noexcept = 0;
    [[nodiscard]] virtual const CameraCapabilities& capabilities() const noexcept = 0;
    [[nodiscard]] virtual CameraState read_state() = 0;

    virtual void set_position(Position position) = 0;
    virtual void set_zoom(ZoomRatio zoom) = 0;
    virtual void set_tracking(bool enabled) = 0;
};

[[nodiscard]] std::unique_ptr<CameraBackend> connect_camera();

}  // namespace linkd
