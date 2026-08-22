#include "linkd/control.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using namespace linkd;

constexpr double kTolerance = 1e-9;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool near(double left, double right) {
    return std::abs(left - right) <= kTolerance;
}

Degrees degrees(double value) {
    return *Degrees::from(value);
}

DegreesPerSecond degrees_per_second(double value) {
    return *DegreesPerSecond::from(value);
}

NormalizedAxis axis(double value) {
    return *NormalizedAxis::from(value);
}

ZoomRatio zoom(double value) {
    return *ZoomRatio::from(value);
}

CameraCapabilities link_2_pro_capabilities() {
    return {
        .pan = {degrees(-145.0), degrees(145.0), degrees(1.0)},
        .tilt = {degrees(-90.0), degrees(100.0), degrees(1.0)},
        .zoom = {zoom(1.0), zoom(4.0), zoom(0.01)},
        .supports_tracking = true,
        .requires_active_video = true,
    };
}

void test_axis_deadzone_and_curve() {
    const AxisCurve curve{.deadzone = 0.1, .exponent = 2.0};

    require(near(shape_axis(axis(0.09), curve).value(), 0.0), "deadzone must suppress drift");
    require(near(shape_axis(axis(-0.1), curve).value(), 0.0), "deadzone edge must be neutral");
    require(near(shape_axis(axis(1.0), curve).value(), 1.0), "positive full scale must survive shaping");
    require(near(shape_axis(axis(-1.0), curve).value(), -1.0), "negative full scale must survive shaping");
    require(near(shape_axis(axis(0.55), curve).value(), 0.25), "axis must be rescaled then squared");
}

void test_gamepad_mapping() {
    const GamepadMapping mapping{
        .curve = {.deadzone = 0.0, .exponent = 1.0},
        .maximum_pan_speed = degrees_per_second(90.0),
        .maximum_tilt_speed = degrees_per_second(60.0),
        .invert_pan = false,
        .invert_tilt = true,
    };

    const auto velocity = map_gamepad(axis(0.5), axis(-0.25), mapping);
    require(near(velocity.pan.value(), 45.0), "horizontal stick must map to pan speed");
    require(near(velocity.tilt.value(), 15.0), "vertical stick must use configured inversion");
}

void test_position_integration_and_clamping() {
    const auto capabilities = link_2_pro_capabilities();
    const Position start{degrees(140.0), degrees(95.0)};
    const Velocity velocity{degrees_per_second(20.0), degrees_per_second(20.0)};

    const auto next = advance_position(start, velocity, std::chrono::duration<double>(1.0), capabilities);
    require(near(next.pan.value(), 145.0), "pan must clamp to the camera range");
    require(near(next.tilt.value(), 100.0), "tilt must clamp to the camera range");
}

void test_boundary_types_reject_invalid_values() {
    require(!Degrees::from(INFINITY), "degrees must reject infinity");
    require(!NormalizedAxis::from(1.01), "axis must reject values above one");
    require(!NormalizedAxis::from(-1.01), "axis must reject values below negative one");
    require(!ZoomRatio::from(0.0), "zoom must be positive");
}

}  // namespace

int main() {
    test_axis_deadzone_and_curve();
    test_gamepad_mapping();
    test_position_integration_and_clamping();
    test_boundary_types_reject_invalid_values();
    std::cout << "all core tests passed\n";
    return 0;
}
