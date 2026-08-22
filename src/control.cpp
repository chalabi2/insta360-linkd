#include "linkd/control.hpp"

#include <algorithm>
#include <cmath>

namespace linkd {

std::optional<Degrees> Degrees::from(double value) {
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    return Degrees(value);
}

std::optional<DegreesPerSecond> DegreesPerSecond::from(double value) {
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    return DegreesPerSecond(value);
}

std::optional<NormalizedAxis> NormalizedAxis::from(double value) {
    if (!std::isfinite(value) || value < -1.0 || value > 1.0) {
        return std::nullopt;
    }
    return NormalizedAxis(value);
}

std::optional<ZoomRatio> ZoomRatio::from(double value) {
    if (!std::isfinite(value) || value <= 0.0) {
        return std::nullopt;
    }
    return ZoomRatio(value);
}

NormalizedAxis shape_axis(NormalizedAxis raw, AxisCurve curve) {
    const double magnitude = std::abs(raw.value());
    if (magnitude <= curve.deadzone) {
        return *NormalizedAxis::from(0.0);
    }

    const double rescaled = (magnitude - curve.deadzone) / (1.0 - curve.deadzone);
    const double shaped = std::pow(rescaled, curve.exponent);
    return *NormalizedAxis::from(std::copysign(shaped, raw.value()));
}

Velocity map_gamepad(
    NormalizedAxis horizontal,
    NormalizedAxis vertical,
    const GamepadMapping& mapping) {
    const double pan_direction = mapping.invert_pan ? -1.0 : 1.0;
    const double tilt_direction = mapping.invert_tilt ? -1.0 : 1.0;
    const auto pan_axis = shape_axis(horizontal, mapping.curve);
    const auto tilt_axis = shape_axis(vertical, mapping.curve);

    return {
        *DegreesPerSecond::from(
            pan_axis.value() * mapping.maximum_pan_speed.value() * pan_direction),
        *DegreesPerSecond::from(
            tilt_axis.value() * mapping.maximum_tilt_speed.value() * tilt_direction),
    };
}

Position advance_position(
    Position current,
    Velocity velocity,
    std::chrono::duration<double> elapsed,
    const CameraCapabilities& capabilities) {
    const double pan = std::clamp(
        current.pan.value() + velocity.pan.value() * elapsed.count(),
        capabilities.pan.minimum.value(),
        capabilities.pan.maximum.value());
    const double tilt = std::clamp(
        current.tilt.value() + velocity.tilt.value() * elapsed.count(),
        capabilities.tilt.minimum.value(),
        capabilities.tilt.maximum.value());

    return {
        *Degrees::from(pan),
        *Degrees::from(tilt),
    };
}

}  // namespace linkd
