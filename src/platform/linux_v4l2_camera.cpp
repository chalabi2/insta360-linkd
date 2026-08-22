#include "linkd/camera.hpp"

#include <fcntl.h>
#include <linux/uvcvideo.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace linkd {
namespace {

constexpr std::uint16_t kInsta360VendorId = 0x2e1a;
constexpr std::uint8_t kTrackingUnitId = 11;
constexpr std::uint8_t kTrackingSelector = 0x02;
constexpr std::uint8_t kUvcSetCurrent = 0x01;
constexpr std::uint8_t kUvcGetCurrent = 0x81;

bool is_supported_product(std::uint16_t product_id) {
    return product_id == 0x4c04 || product_id == 0x4c06;
}

std::string system_error(std::string message) {
    return message + ": " + std::strerror(errno);
}

int retry_ioctl(int descriptor, unsigned long request, void* argument) {
    int result = 0;
    do {
        result = ioctl(descriptor, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

std::optional<std::uint16_t> read_hex_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string value;
    if (!(input >> value)) {
        return std::nullopt;
    }

    try {
        const auto parsed = std::stoul(value, nullptr, 16);
        if (parsed > 0xffffU) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(parsed);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::pair<std::uint16_t, std::uint16_t>> usb_identity_for(
    const std::filesystem::path& video_device) {
    std::error_code error;
    auto path = std::filesystem::canonical(
        std::filesystem::path("/sys/class/video4linux") / video_device.filename() / "device",
        error);
    if (error) {
        return std::nullopt;
    }

    while (path != path.root_path()) {
        const auto vendor = read_hex_file(path / "idVendor");
        const auto product = read_hex_file(path / "idProduct");
        if (vendor && product) {
            return std::pair{*vendor, *product};
        }
        path = path.parent_path();
    }
    return std::nullopt;
}

v4l2_queryctrl query_control(int descriptor, std::uint32_t id) {
    v4l2_queryctrl query{};
    query.id = id;
    if (retry_ioctl(descriptor, VIDIOC_QUERYCTRL, &query) == -1) {
        throw CameraError(system_error("Could not query a V4L2 camera control"));
    }
    if ((query.flags & V4L2_CTRL_FLAG_DISABLED) != 0U) {
        throw CameraError("A required V4L2 camera control is disabled");
    }
    return query;
}

class LinuxV4l2Camera final : public CameraBackend {
public:
    LinuxV4l2Camera(
        int descriptor,
        std::filesystem::path path,
        UsbIdentity identity,
        CameraCapabilities capabilities)
        : descriptor_(descriptor),
          path_(std::move(path)),
          identity_(std::move(identity)),
          capabilities_(std::move(capabilities)) {}

    ~LinuxV4l2Camera() override {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

    LinuxV4l2Camera(const LinuxV4l2Camera&) = delete;
    LinuxV4l2Camera& operator=(const LinuxV4l2Camera&) = delete;

    [[nodiscard]] const UsbIdentity& identity() const noexcept override {
        return identity_;
    }

    [[nodiscard]] const CameraCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] CameraState read_state() override {
        const auto pan = get_control(V4L2_CID_PAN_ABSOLUTE);
        const auto tilt = get_control(V4L2_CID_TILT_ABSOLUTE);
        const auto zoom = get_control(V4L2_CID_ZOOM_ABSOLUTE);

        std::uint8_t tracking = 0;
        extension_query(kUvcGetCurrent, tracking);
        return {
            .position = {
                *Degrees::from(static_cast<double>(pan) / 3600.0),
                *Degrees::from(static_cast<double>(tilt) / 3600.0),
            },
            .zoom = *ZoomRatio::from(static_cast<double>(zoom) / 100.0),
            .tracking_enabled = tracking != 0,
        };
    }

    void set_position(Position position) override {
        set_control(
            V4L2_CID_PAN_ABSOLUTE,
            static_cast<std::int32_t>(std::llround(position.pan.value() * 3600.0)));
        set_control(
            V4L2_CID_TILT_ABSOLUTE,
            static_cast<std::int32_t>(std::llround(position.tilt.value() * 3600.0)));
    }

    void set_zoom(ZoomRatio zoom) override {
        set_control(
            V4L2_CID_ZOOM_ABSOLUTE,
            static_cast<std::int32_t>(std::llround(zoom.value() * 100.0)));
    }

    void set_tracking(bool enabled) override {
        std::uint8_t value = enabled ? 1 : 0;
        extension_query(kUvcSetCurrent, value);
    }

private:
    std::int32_t get_control(std::uint32_t id) {
        v4l2_control control{};
        control.id = id;
        if (retry_ioctl(descriptor_, VIDIOC_G_CTRL, &control) == -1) {
            throw CameraError(system_error("Could not read a V4L2 camera control"));
        }
        return control.value;
    }

    void set_control(std::uint32_t id, std::int32_t value) {
        v4l2_control control{};
        control.id = id;
        control.value = value;
        if (retry_ioctl(descriptor_, VIDIOC_S_CTRL, &control) == -1) {
            throw CameraError(system_error("Could not write a V4L2 camera control"));
        }
    }

    void extension_query(std::uint8_t request, std::uint8_t& value) {
        uvc_xu_control_query query{};
        query.unit = kTrackingUnitId;
        query.selector = kTrackingSelector;
        query.query = request;
        query.size = 1;
        query.data = &value;
        if (retry_ioctl(descriptor_, UVCIOC_CTRL_QUERY, &query) == -1) {
            throw CameraError(system_error("Could not access the Insta360 tracking control"));
        }
    }

    int descriptor_;
    std::filesystem::path path_;
    UsbIdentity identity_;
    CameraCapabilities capabilities_;
};

std::unique_ptr<CameraBackend> try_camera(const std::filesystem::path& path) {
    const auto usb_identity = usb_identity_for(path);
    if (!usb_identity || usb_identity->first != kInsta360VendorId ||
        !is_supported_product(usb_identity->second)) {
        return nullptr;
    }

    const int descriptor = open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (descriptor == -1) {
        if (errno == EACCES) {
            throw CameraError(
                "Permission denied opening " + path.string() +
                "; install the supplied udev rule or add the user to the video group");
        }
        return nullptr;
    }

    v4l2_capability device_capability{};
    if (retry_ioctl(descriptor, VIDIOC_QUERYCAP, &device_capability) == -1) {
        close(descriptor);
        return nullptr;
    }

    try {
        const auto pan = query_control(descriptor, V4L2_CID_PAN_ABSOLUTE);
        const auto tilt = query_control(descriptor, V4L2_CID_TILT_ABSOLUTE);
        const auto zoom = query_control(descriptor, V4L2_CID_ZOOM_ABSOLUTE);

        const CameraCapabilities capabilities{
            .pan = {
                *Degrees::from(static_cast<double>(pan.minimum) / 3600.0),
                *Degrees::from(static_cast<double>(pan.maximum) / 3600.0),
                *Degrees::from(static_cast<double>(pan.step) / 3600.0),
            },
            .tilt = {
                *Degrees::from(static_cast<double>(tilt.minimum) / 3600.0),
                *Degrees::from(static_cast<double>(tilt.maximum) / 3600.0),
                *Degrees::from(static_cast<double>(tilt.step) / 3600.0),
            },
            .zoom = {
                *ZoomRatio::from(static_cast<double>(zoom.minimum) / 100.0),
                *ZoomRatio::from(static_cast<double>(zoom.maximum) / 100.0),
                *ZoomRatio::from(static_cast<double>(zoom.step) / 100.0),
            },
            .supports_tracking = usb_identity->second == 0x4c04 ||
                usb_identity->second == 0x4c06,
            .requires_active_video = true,
        };
        const UsbIdentity identity{
            .vendor_id = usb_identity->first,
            .product_id = usb_identity->second,
            .product_name = reinterpret_cast<const char*>(device_capability.card),
        };
        return std::make_unique<LinuxV4l2Camera>(
            descriptor, path, identity, capabilities);
    } catch (const CameraError&) {
        close(descriptor);
        return nullptr;
    }
}

}  // namespace

std::unique_ptr<CameraBackend> connect_camera() {
    for (int index = 0; index < 64; ++index) {
        const auto path = std::filesystem::path("/dev") / ("video" + std::to_string(index));
        if (!std::filesystem::exists(path)) {
            continue;
        }
        if (auto camera = try_camera(path)) {
            return camera;
        }
    }
    throw CameraError("No controllable Insta360 Link V4L2 device was found");
}

}  // namespace linkd
