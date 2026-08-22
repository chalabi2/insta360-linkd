#pragma once

#include <chrono>
#include <optional>

namespace linkd {

class Degrees {
public:
    static std::optional<Degrees> from(double value);

    [[nodiscard]] constexpr double value() const noexcept { return value_; }

    friend constexpr bool operator==(Degrees, Degrees) = default;

private:
    explicit constexpr Degrees(double value) : value_(value) {}
    double value_;
};

class DegreesPerSecond {
public:
    static std::optional<DegreesPerSecond> from(double value);

    [[nodiscard]] constexpr double value() const noexcept { return value_; }

    friend constexpr bool operator==(DegreesPerSecond, DegreesPerSecond) = default;

private:
    explicit constexpr DegreesPerSecond(double value) : value_(value) {}
    double value_;
};

class NormalizedAxis {
public:
    static std::optional<NormalizedAxis> from(double value);

    [[nodiscard]] constexpr double value() const noexcept { return value_; }

    friend constexpr bool operator==(NormalizedAxis, NormalizedAxis) = default;

private:
    explicit constexpr NormalizedAxis(double value) : value_(value) {}
    double value_;
};

class ZoomRatio {
public:
    static std::optional<ZoomRatio> from(double value);

    [[nodiscard]] constexpr double value() const noexcept { return value_; }

    friend constexpr bool operator==(ZoomRatio, ZoomRatio) = default;

private:
    explicit constexpr ZoomRatio(double value) : value_(value) {}
    double value_;
};

template <typename T>
struct Range {
    T minimum;
    T maximum;
    T step;
};

struct Position {
    Degrees pan;
    Degrees tilt;
};

struct Velocity {
    DegreesPerSecond pan;
    DegreesPerSecond tilt;
};

struct CameraCapabilities {
    Range<Degrees> pan;
    Range<Degrees> tilt;
    Range<ZoomRatio> zoom;
    bool supports_tracking;
    bool requires_active_video;
};

struct AxisCurve {
    double deadzone = 0.12;
    double exponent = 2.0;
};

struct GamepadMapping {
    AxisCurve curve;
    DegreesPerSecond maximum_pan_speed;
    DegreesPerSecond maximum_tilt_speed;
    bool invert_pan = false;
    bool invert_tilt = true;
};

[[nodiscard]] NormalizedAxis shape_axis(NormalizedAxis raw, AxisCurve curve);

[[nodiscard]] Velocity map_gamepad(
    NormalizedAxis horizontal,
    NormalizedAxis vertical,
    const GamepadMapping& mapping);

[[nodiscard]] Position advance_position(
    Position current,
    Velocity velocity,
    std::chrono::duration<double> elapsed,
    const CameraCapabilities& capabilities);

}  // namespace linkd
