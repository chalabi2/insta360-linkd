#include "linkd/camera.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace linkd {
namespace {

constexpr std::uint16_t kInsta360VendorId = 0x2e1a;
constexpr std::uint8_t kCameraTerminalUnitId = 1;
constexpr std::uint8_t kZoomAbsoluteSelector = 0x0b;
constexpr std::uint8_t kPanTiltAbsoluteSelector = 0x0d;
constexpr std::uint8_t kTrackingUnitId = 11;
constexpr std::uint8_t kTrackingSelector = 0x02;

constexpr std::uint8_t kUvcSetCurrent = 0x01;
constexpr std::uint8_t kUvcGetCurrent = 0x81;
constexpr std::uint8_t kUvcGetMinimum = 0x82;
constexpr std::uint8_t kUvcGetMaximum = 0x83;
constexpr std::uint8_t kUvcGetResolution = 0x84;

std::string io_error(std::string message, IOReturn code) {
    std::ostringstream output;
    output << message << " (IOKit 0x" << std::hex << static_cast<std::uint32_t>(code) << ')';
    return output.str();
}

std::uint16_t read_u16_le(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::int32_t read_i32_le(const std::uint8_t* bytes) {
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
    return static_cast<std::int32_t>(value);
}

void write_u16_le(std::uint8_t* bytes, std::uint16_t value) {
    bytes[0] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void write_i32_le(std::uint8_t* bytes, std::int32_t value) {
    const auto bits = static_cast<std::uint32_t>(value);
    bytes[0] = static_cast<std::uint8_t>(bits & 0xffU);
    bytes[1] = static_cast<std::uint8_t>((bits >> 8U) & 0xffU);
    bytes[2] = static_cast<std::uint8_t>((bits >> 16U) & 0xffU);
    bytes[3] = static_cast<std::uint8_t>((bits >> 24U) & 0xffU);
}

template <typename Integer>
Integer registry_integer(io_service_t service, CFStringRef key) {
    CFTypeRef property = IORegistryEntrySearchCFProperty(
        service, kIOUSBPlane, key, kCFAllocatorDefault, 0);
    if (property == nullptr || CFGetTypeID(property) != CFNumberGetTypeID()) {
        if (property != nullptr) {
            CFRelease(property);
        }
        throw CameraError("USB device is missing a numeric registry property");
    }

    std::int64_t value = 0;
    const bool converted = CFNumberGetValue(
        static_cast<CFNumberRef>(property), kCFNumberSInt64Type, &value);
    CFRelease(property);
    if (!converted) {
        throw CameraError("Could not read a USB registry number");
    }
    return static_cast<Integer>(value);
}

std::string registry_string(io_service_t service, CFStringRef key) {
    CFTypeRef property = IORegistryEntrySearchCFProperty(
        service, kIOUSBPlane, key, kCFAllocatorDefault, 0);
    if (property == nullptr || CFGetTypeID(property) != CFStringGetTypeID()) {
        if (property != nullptr) {
            CFRelease(property);
        }
        return "Insta360 Link";
    }

    std::array<char, 256> buffer{};
    const bool converted = CFStringGetCString(
        static_cast<CFStringRef>(property),
        buffer.data(),
        static_cast<CFIndex>(buffer.size()),
        kCFStringEncodingUTF8);
    CFRelease(property);
    return converted ? std::string(buffer.data()) : std::string("Insta360 Link");
}

class MacUvcCamera final : public CameraBackend {
public:
    explicit MacUvcCamera(io_service_t device)
        : identity_{
              registry_integer<std::uint16_t>(device, CFSTR(kUSBVendorID)),
              registry_integer<std::uint16_t>(device, CFSTR(kUSBProductID)),
              registry_string(device, CFSTR(kUSBProductString))} {
        open_control_interface(device);
        capabilities_ = read_capabilities();
    }

    ~MacUvcCamera() override {
        if (interface_ != nullptr) {
            if (interface_owned_) {
                (*interface_)->USBInterfaceClose(interface_);
            }
            (*interface_)->Release(interface_);
        }
    }

    MacUvcCamera(const MacUvcCamera&) = delete;
    MacUvcCamera& operator=(const MacUvcCamera&) = delete;

    [[nodiscard]] const UsbIdentity& identity() const noexcept override {
        return identity_;
    }

    [[nodiscard]] const CameraCapabilities& capabilities() const noexcept override {
        return *capabilities_;
    }

    [[nodiscard]] CameraState read_state() override {
        const auto position_bytes = get_control<8>(
            kUvcGetCurrent, kCameraTerminalUnitId, kPanTiltAbsoluteSelector);
        const auto zoom_bytes = get_control<2>(
            kUvcGetCurrent, kCameraTerminalUnitId, kZoomAbsoluteSelector);
        const auto tracking_bytes = get_control<1>(
            kUvcGetCurrent, kTrackingUnitId, kTrackingSelector);

        return {
            .position = {
                *Degrees::from(static_cast<double>(read_i32_le(position_bytes.data())) / 3600.0),
                *Degrees::from(static_cast<double>(read_i32_le(position_bytes.data() + 4)) / 3600.0),
            },
            .zoom = *ZoomRatio::from(static_cast<double>(read_u16_le(zoom_bytes.data())) / 100.0),
            .tracking_enabled = tracking_bytes[0] != 0,
        };
    }

    void set_position(Position position) override {
        std::array<std::uint8_t, 8> bytes{};
        const auto pan = static_cast<std::int32_t>(std::llround(position.pan.value() * 3600.0));
        const auto tilt = static_cast<std::int32_t>(std::llround(position.tilt.value() * 3600.0));
        write_i32_le(bytes.data(), pan);
        write_i32_le(bytes.data() + 4, tilt);
        set_control(kCameraTerminalUnitId, kPanTiltAbsoluteSelector, bytes);
    }

    void set_zoom(ZoomRatio zoom) override {
        std::array<std::uint8_t, 2> bytes{};
        const auto raw = static_cast<std::uint16_t>(std::llround(zoom.value() * 100.0));
        write_u16_le(bytes.data(), raw);
        set_control(kCameraTerminalUnitId, kZoomAbsoluteSelector, bytes);
    }

    void set_tracking(bool enabled) override {
        const std::array<std::uint8_t, 1> bytes{
            static_cast<std::uint8_t>(enabled ? 1 : 0),
        };
        set_control(kTrackingUnitId, kTrackingSelector, bytes);
    }

private:
    void open_control_interface(io_service_t device) {
        IOCFPlugInInterface** device_plugin = nullptr;
        SInt32 score = 0;
        kern_return_t result = IOCreatePlugInInterfaceForService(
            device,
            kIOUSBDeviceUserClientTypeID,
            kIOCFPlugInInterfaceID,
            &device_plugin,
            &score);
        if (result != kIOReturnSuccess || device_plugin == nullptr) {
            throw CameraError(io_error("Could not create the USB device interface", result));
        }

        IOUSBDeviceInterface** device_interface = nullptr;
        const HRESULT query_result = (*device_plugin)->QueryInterface(
            device_plugin,
            CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
            reinterpret_cast<LPVOID*>(&device_interface));
        IODestroyPlugInInterface(device_plugin);
        if (query_result != 0 || device_interface == nullptr) {
            throw CameraError("Could not access the USB device");
        }

        IOUSBFindInterfaceRequest request{
            .bInterfaceClass = kUSBVideoInterfaceClass,
            .bInterfaceSubClass = kUSBVideoControlSubClass,
            .bInterfaceProtocol = kIOUSBFindInterfaceDontCare,
            .bAlternateSetting = kIOUSBFindInterfaceDontCare,
        };
        io_iterator_t iterator = IO_OBJECT_NULL;
        result = (*device_interface)->CreateInterfaceIterator(device_interface, &request, &iterator);
        (*device_interface)->Release(device_interface);
        if (result != kIOReturnSuccess || iterator == IO_OBJECT_NULL) {
            throw CameraError(io_error("Could not enumerate the UVC control interface", result));
        }

        io_service_t control_service = IOIteratorNext(iterator);
        IOObjectRelease(iterator);
        if (control_service == IO_OBJECT_NULL) {
            throw CameraError("The camera does not expose a UVC control interface");
        }

        IOCFPlugInInterface** interface_plugin = nullptr;
        result = IOCreatePlugInInterfaceForService(
            control_service,
            kIOUSBInterfaceUserClientTypeID,
            kIOCFPlugInInterfaceID,
            &interface_plugin,
            &score);
        IOObjectRelease(control_service);
        if (result != kIOReturnSuccess || interface_plugin == nullptr) {
            throw CameraError(io_error("Could not create the UVC interface plugin", result));
        }

        const HRESULT interface_result = (*interface_plugin)->QueryInterface(
            interface_plugin,
            CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID),
            reinterpret_cast<LPVOID*>(&interface_));
        IODestroyPlugInInterface(interface_plugin);
        if (interface_result != 0 || interface_ == nullptr) {
            throw CameraError("Could not access the UVC control interface");
        }

        result = (*interface_)->GetInterfaceNumber(interface_, &interface_number_);
        if (result != kIOReturnSuccess) {
            throw CameraError(io_error("Could not read the UVC interface number", result));
        }
    }

    void ensure_open() {
        if (interface_open_) {
            return;
        }
        const IOReturn result = (*interface_)->USBInterfaceOpen(interface_);
        if (result == kIOReturnSuccess) {
            interface_open_ = true;
            interface_owned_ = true;
            return;
        }
        if (result == kIOReturnExclusiveAccess) {
            interface_open_ = true;
            interface_owned_ = false;
            return;
        }
        if (result != kIOReturnSuccess) {
            throw CameraError(io_error("Could not open the UVC control interface", result));
        }
    }

    template <std::size_t Size>
    std::array<std::uint8_t, Size> get_control(
        std::uint8_t request,
        std::uint8_t unit,
        std::uint8_t selector) {
        std::array<std::uint8_t, Size> bytes{};
        ensure_open();
        IOUSBDevRequest control_request{
            .bmRequestType = 0xa1,
            .bRequest = request,
            .wValue = static_cast<UInt16>(static_cast<UInt16>(selector) << 8U),
            .wIndex = static_cast<UInt16>(
                (static_cast<UInt16>(unit) << 8U) | interface_number_),
            .wLength = static_cast<UInt16>(Size),
            .pData = bytes.data(),
            .wLenDone = 0,
        };
        const IOReturn result = (*interface_)->ControlRequest(interface_, 0, &control_request);
        if (result != kIOReturnSuccess || control_request.wLenDone != Size) {
            throw CameraError(io_error("UVC read failed", result));
        }
        return bytes;
    }

    template <std::size_t Size>
    void set_control(
        std::uint8_t unit,
        std::uint8_t selector,
        const std::array<std::uint8_t, Size>& bytes) {
        ensure_open();
        IOUSBDevRequest control_request{
            .bmRequestType = 0x21,
            .bRequest = kUvcSetCurrent,
            .wValue = static_cast<UInt16>(static_cast<UInt16>(selector) << 8U),
            .wIndex = static_cast<UInt16>(
                (static_cast<UInt16>(unit) << 8U) | interface_number_),
            .wLength = static_cast<UInt16>(Size),
            .pData = const_cast<std::uint8_t*>(bytes.data()),
            .wLenDone = 0,
        };
        const IOReturn result = (*interface_)->ControlRequest(interface_, 0, &control_request);
        if (result != kIOReturnSuccess || control_request.wLenDone != Size) {
            throw CameraError(io_error("UVC write failed", result));
        }
    }

    CameraCapabilities read_capabilities() {
        const auto minimum = get_control<8>(
            kUvcGetMinimum, kCameraTerminalUnitId, kPanTiltAbsoluteSelector);
        const auto maximum = get_control<8>(
            kUvcGetMaximum, kCameraTerminalUnitId, kPanTiltAbsoluteSelector);
        const auto resolution = get_control<8>(
            kUvcGetResolution, kCameraTerminalUnitId, kPanTiltAbsoluteSelector);
        const auto zoom_minimum = get_control<2>(
            kUvcGetMinimum, kCameraTerminalUnitId, kZoomAbsoluteSelector);
        const auto zoom_maximum = get_control<2>(
            kUvcGetMaximum, kCameraTerminalUnitId, kZoomAbsoluteSelector);
        const auto zoom_resolution = get_control<2>(
            kUvcGetResolution, kCameraTerminalUnitId, kZoomAbsoluteSelector);

        return {
            .pan = {
                *Degrees::from(static_cast<double>(read_i32_le(minimum.data())) / 3600.0),
                *Degrees::from(static_cast<double>(read_i32_le(maximum.data())) / 3600.0),
                *Degrees::from(static_cast<double>(read_i32_le(resolution.data())) / 3600.0),
            },
            .tilt = {
                *Degrees::from(static_cast<double>(read_i32_le(minimum.data() + 4)) / 3600.0),
                *Degrees::from(static_cast<double>(read_i32_le(maximum.data() + 4)) / 3600.0),
                *Degrees::from(static_cast<double>(read_i32_le(resolution.data() + 4)) / 3600.0),
            },
            .zoom = {
                *ZoomRatio::from(static_cast<double>(read_u16_le(zoom_minimum.data())) / 100.0),
                *ZoomRatio::from(static_cast<double>(read_u16_le(zoom_maximum.data())) / 100.0),
                *ZoomRatio::from(static_cast<double>(read_u16_le(zoom_resolution.data())) / 100.0),
            },
            .supports_tracking = identity_.product_id == 0x4c04 || identity_.product_id == 0x4c06,
            .requires_active_video = true,
        };
    }

    UsbIdentity identity_;
    std::optional<CameraCapabilities> capabilities_;
    IOUSBInterfaceInterface** interface_ = nullptr;
    UInt8 interface_number_ = 0;
    bool interface_open_ = false;
    bool interface_owned_ = false;
};

}  // namespace

std::unique_ptr<CameraBackend> connect_camera() {
    constexpr std::array<std::uint16_t, 2> supported_products{
        0x4c06,  // Link 2 Pro
        0x4c04,  // Link 2
    };

    for (const std::uint16_t product_id : supported_products) {
        CFMutableDictionaryRef matching = IOServiceMatching(kIOUSBDeviceClassName);
        if (matching == nullptr) {
            throw CameraError("Could not create a USB device query");
        }

        CFMutableDictionaryRef properties = CFDictionaryCreateMutable(
            kCFAllocatorDefault,
            2,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFNumberRef vendor = CFNumberCreate(
            kCFAllocatorDefault, kCFNumberSInt16Type, &kInsta360VendorId);
        CFNumberRef product = CFNumberCreate(
            kCFAllocatorDefault, kCFNumberSInt16Type, &product_id);
        CFDictionarySetValue(properties, CFSTR(kUSBVendorID), vendor);
        CFDictionarySetValue(properties, CFSTR(kUSBProductID), product);
        CFRelease(vendor);
        CFRelease(product);
        CFDictionarySetValue(matching, CFSTR(kIOPropertyMatchKey), properties);
        CFRelease(properties);

        io_service_t device = IOServiceGetMatchingService(kIOMainPortDefault, matching);
        if (device != IO_OBJECT_NULL) {
            auto camera = std::make_unique<MacUvcCamera>(device);
            IOObjectRelease(device);
            return camera;
        }
    }

    throw CameraError("No supported Insta360 Link camera was found");
}

}  // namespace linkd
