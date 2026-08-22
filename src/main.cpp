#include "linkd/camera.hpp"
#include "linkd/controller.hpp"
#include "linkd/gamepad.hpp"
#include "linkd/grpc_server.hpp"
#include "linkd/server.hpp"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace linkd;

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) {
    stop_requested = 1;
}

void usage(std::ostream& output) {
    output
        << "usage:\n"
        << "  linkd serve [--listen <address>] [--port <port>] "
           "[--gamepad-enabled=<true|false>]\n"
        << "              [--grpc-listen <address>] [--grpc-port <port>] "
           "[--grpc-enabled=<true|false>]\n"
        << "  linkd status\n"
        << "  linkd center\n"
        << "  linkd position <pan-degrees> <tilt-degrees>\n"
        << "  linkd zoom <ratio>\n"
        << "  linkd tracking <on|off>\n";
}

double parse_number(std::string_view input, std::string_view name) {
    const std::string text(input);
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (text.empty() || end == text.c_str() ||
        end != text.c_str() + text.size() || errno == ERANGE ||
        !std::isfinite(value)) {
        throw CameraError("Invalid " + std::string(name) + ": " + std::string(input));
    }
    return value;
}

std::uint16_t parse_port(std::string_view input) {
    unsigned int value = 0;
    const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
    if (result.ec != std::errc{} || result.ptr != input.data() + input.size() ||
        value == 0 || value > 65535) {
        throw CameraError("Port must be an integer between 1 and 65535");
    }
    return static_cast<std::uint16_t>(value);
}

bool parse_boolean(std::string_view input, std::string_view name) {
    if (input == "true") {
        return true;
    }
    if (input == "false") {
        return false;
    }
    throw CameraError(
        std::string(name) + " must be 'true' or 'false', got: " + std::string(input));
}

void print_status(CameraBackend& camera) {
    const auto state = camera.read_state();
    const auto& identity = camera.identity();
    const auto& capabilities = camera.capabilities();

    std::cout << std::fixed << std::setprecision(2)
              << "{\n"
              << "  \"camera\": \"" << identity.product_name << "\",\n"
              << "  \"usb\": \"" << std::hex << std::setw(4) << std::setfill('0')
              << identity.vendor_id << ':' << std::setw(4) << identity.product_id << std::dec
              << "\",\n"
              << "  \"position\": {\"pan\": " << state.position.pan.value()
              << ", \"tilt\": " << state.position.tilt.value() << "},\n"
              << "  \"zoom\": " << state.zoom.value() << ",\n"
              << "  \"tracking\": " << (state.tracking_enabled ? "true" : "false") << ",\n"
              << "  \"requires_active_video\": "
              << (capabilities.requires_active_video ? "true" : "false") << ",\n"
              << "  \"ranges\": {\n"
              << "    \"pan\": [" << capabilities.pan.minimum.value() << ", "
              << capabilities.pan.maximum.value() << "],\n"
              << "    \"tilt\": [" << capabilities.tilt.minimum.value() << ", "
              << capabilities.tilt.maximum.value() << "],\n"
              << "    \"zoom\": [" << capabilities.zoom.minimum.value() << ", "
              << capabilities.zoom.maximum.value() << "]\n"
              << "  }\n"
              << "}\n";
}

int serve(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8765;
    bool gamepad_enabled = false;
    std::string grpc_host = "127.0.0.1";
    std::uint16_t grpc_port = 8766;
    bool grpc_enabled = grpc_available();

    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        constexpr std::string_view gamepad_prefix = "--gamepad-enabled=";
        constexpr std::string_view grpc_prefix = "--grpc-enabled=";
        if (argument.starts_with(gamepad_prefix)) {
            gamepad_enabled = parse_boolean(
                argument.substr(gamepad_prefix.size()), "--gamepad-enabled");
        } else if (argument.starts_with(grpc_prefix)) {
            grpc_enabled = parse_boolean(
                argument.substr(grpc_prefix.size()), "--grpc-enabled");
        } else if (argument == "--listen" && index + 1 < argc) {
            host = argv[++index];
        } else if (argument == "--port" && index + 1 < argc) {
            port = parse_port(argv[++index]);
        } else if (argument == "--grpc-listen" && index + 1 < argc) {
            grpc_host = argv[++index];
        } else if (argument == "--grpc-port" && index + 1 < argc) {
            grpc_port = parse_port(argv[++index]);
        } else {
            throw CameraError("Unknown or incomplete serve option: " + std::string(argument));
        }
    }

    CameraController controller(connect_camera());
    std::unique_ptr<GamepadInput> gamepad;
    if (gamepad_enabled) {
        gamepad = std::make_unique<GamepadInput>(controller);
    }
    ApiServer server(controller, host, port);
    const std::uint16_t listening_port = server.start();
    std::unique_ptr<GrpcServer> grpc_server;
    std::uint16_t listening_grpc_port = 0;
    if (grpc_enabled) {
        grpc_server = std::make_unique<GrpcServer>(controller, grpc_host, grpc_port);
        listening_grpc_port = grpc_server->start();
    }

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    std::cout << "linkd: controlling " << controller.identity().product_name << '\n'
              << "linkd: API listening at http://" << host << ':' << listening_port << "/v1\n"
              << "linkd: the gimbal wakes only while an application captures camera video\n";
    if (grpc_server != nullptr) {
        std::cout << "linkd: gRPC listening at " << grpc_host << ':'
                  << listening_grpc_port << '\n';
    }
    std::cout << "linkd: press Ctrl-C to stop\n";

    while (stop_requested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (grpc_server != nullptr) {
        grpc_server->stop();
    }
    server.stop();
    return EXIT_SUCCESS;
}

int run_one_shot(int argc, char** argv) {
    auto camera = connect_camera();
    const std::string_view command(argv[1]);

    if (command == "status" && argc == 2) {
        print_status(*camera);
    } else if (command == "center" && argc == 2) {
        camera->set_tracking(false);
        camera->set_position({*Degrees::from(0.0), *Degrees::from(0.0)});
        print_status(*camera);
    } else if (command == "position" && argc == 4) {
        const auto pan = Degrees::from(parse_number(argv[2], "pan"));
        const auto tilt = Degrees::from(parse_number(argv[3], "tilt"));
        if (!pan || !tilt) {
            throw CameraError("Position must contain finite degree values");
        }
        const auto& capabilities = camera->capabilities();
        if (pan->value() < capabilities.pan.minimum.value() ||
            pan->value() > capabilities.pan.maximum.value() ||
            tilt->value() < capabilities.tilt.minimum.value() ||
            tilt->value() > capabilities.tilt.maximum.value()) {
            throw CameraError("Position is outside the camera's advertised range");
        }
        camera->set_tracking(false);
        camera->set_position({*pan, *tilt});
        print_status(*camera);
    } else if (command == "zoom" && argc == 3) {
        const auto zoom = ZoomRatio::from(parse_number(argv[2], "zoom"));
        if (!zoom) {
            throw CameraError("Zoom must be a positive finite ratio");
        }
        const auto& range = camera->capabilities().zoom;
        if (zoom->value() < range.minimum.value() || zoom->value() > range.maximum.value()) {
            throw CameraError("Zoom is outside the camera's advertised range");
        }
        camera->set_zoom(*zoom);
        print_status(*camera);
    } else if (command == "tracking" && argc == 3) {
        const std::string_view value(argv[2]);
        if (value != "on" && value != "off") {
            throw CameraError("Tracking must be 'on' or 'off'");
        }
        camera->set_tracking(value == "on");
        print_status(*camera);
    } else {
        usage(std::cerr);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage(std::cerr);
            return EXIT_FAILURE;
        }
        if (std::string_view(argv[1]) == "serve") {
            return serve(argc, argv);
        }
        return run_one_shot(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "linkd: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
