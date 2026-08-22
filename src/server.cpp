#include "linkd/server.hpp"

#include "linkd/controller.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace linkd {
namespace {

using json = nlohmann::json;

class RequestError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

json range_json(const Range<Degrees>& range) {
    return {
        {"minimum", range.minimum.value()},
        {"maximum", range.maximum.value()},
        {"step", range.step.value()},
    };
}

json range_json(const Range<ZoomRatio>& range) {
    return {
        {"minimum", range.minimum.value()},
        {"maximum", range.maximum.value()},
        {"step", range.step.value()},
    };
}

json state_json(CameraController& controller) {
    const CameraState state = controller.status();
    const auto& identity = controller.identity();
    json result{
        {"camera", {
            {"name", identity.product_name},
            {"vendor_id", identity.vendor_id},
            {"product_id", identity.product_id},
        }},
        {"position", {
            {"pan", state.position.pan.value()},
            {"tilt", state.position.tilt.value()},
        }},
        {"zoom", state.zoom.value()},
        {"tracking", state.tracking_enabled},
    };
    if (const auto error = controller.background_error()) {
        result["background_error"] = *error;
    }
    return result;
}

json capabilities_json(const CameraCapabilities& capabilities) {
    return {
        {"pan", range_json(capabilities.pan)},
        {"tilt", range_json(capabilities.tilt)},
        {"zoom", range_json(capabilities.zoom)},
        {"tracking", capabilities.supports_tracking},
        {"velocity_lease_ms", 250},
        {"requires_active_video", capabilities.requires_active_video},
    };
}

json parse_body(const httplib::Request& request) {
    try {
        return json::parse(request.body);
    } catch (const json::exception&) {
        throw RequestError("Request body must be valid JSON");
    }
}

double required_number(const json& body, const char* name) {
    if (!body.contains(name) || !body.at(name).is_number()) {
        throw RequestError(std::string("Missing numeric field: ") + name);
    }
    const double value = body.at(name).get<double>();
    if (!std::isfinite(value)) {
        throw RequestError(std::string("Field must be finite: ") + name);
    }
    return value;
}

bool required_boolean(const json& body, const char* name) {
    if (!body.contains(name) || !body.at(name).is_boolean()) {
        throw RequestError(std::string("Missing boolean field: ") + name);
    }
    return body.at(name).get<bool>();
}

void send_json(httplib::Response& response, int status, const json& body) {
    response.status = status;
    response.set_content(body.dump(2) + '\n', "application/json");
}

template <typename Handler>
auto guarded(Handler handler) {
    return [handler = std::move(handler)](
               const httplib::Request& request, httplib::Response& response) {
        try {
            handler(request, response);
        } catch (const RequestError& error) {
            send_json(response, 400, {{"error", error.what()}});
        } catch (const CameraError& error) {
            send_json(response, 503, {{"error", error.what()}});
        } catch (const std::exception& error) {
            send_json(response, 500, {{"error", error.what()}});
        }
    };
}

void require_in_range(double value, double minimum, double maximum, const char* name) {
    if (value < minimum || value > maximum) {
        throw RequestError(
            std::string(name) + " must be between " + std::to_string(minimum) +
            " and " + std::to_string(maximum));
    }
}

}  // namespace

class ApiServer::Implementation {
public:
    Implementation(CameraController& controller, std::string host, std::uint16_t port)
        : controller_(controller), host_(std::move(host)), port_(port) {
        configure_routes();
    }

    ~Implementation() {
        stop();
    }

    std::uint16_t start() {
        if (started_) {
            return port_;
        }
        if (!server_.bind_to_port(host_, static_cast<int>(port_))) {
            throw CameraError(
                "Could not bind the API server to " + host_ + ':' + std::to_string(port_));
        }
        started_ = true;
        thread_ = std::thread([this] {
            if (!server_.listen_after_bind()) {
                std::cerr << "linkd: HTTP server stopped after a listening error\n";
            }
        });
        return port_;
    }

    void stop() {
        if (!started_) {
            return;
        }
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
        started_ = false;
    }

private:
    void configure_routes() {
        server_.set_default_headers({
            {"Access-Control-Allow-Origin", "*"},
            {"Access-Control-Allow-Headers", "Content-Type"},
            {"Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS"},
        });

        server_.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& response) {
            response.status = 204;
        });

        server_.Get("/v1/status", guarded([this](const auto&, auto& response) {
            send_json(response, 200, state_json(controller_));
        }));

        server_.Get("/v1/capabilities", guarded([this](const auto&, auto& response) {
            send_json(response, 200, capabilities_json(controller_.capabilities()));
        }));

        server_.Post("/v1/center", guarded([this](const auto&, auto& response) {
            controller_.center();
            send_json(response, 200, state_json(controller_));
        }));

        server_.Put("/v1/position", guarded([this](const auto& request, auto& response) {
            const json body = parse_body(request);
            const double pan_value = required_number(body, "pan");
            const double tilt_value = required_number(body, "tilt");
            const auto& capabilities = controller_.capabilities();
            require_in_range(
                pan_value,
                capabilities.pan.minimum.value(),
                capabilities.pan.maximum.value(),
                "pan");
            require_in_range(
                tilt_value,
                capabilities.tilt.minimum.value(),
                capabilities.tilt.maximum.value(),
                "tilt");
            controller_.set_position({*Degrees::from(pan_value), *Degrees::from(tilt_value)});
            send_json(response, 200, state_json(controller_));
        }));

        server_.Put("/v1/velocity", guarded([this](const auto& request, auto& response) {
            const json body = parse_body(request);
            const double pan_value = required_number(body, "pan");
            const double tilt_value = required_number(body, "tilt");
            require_in_range(pan_value, -180.0, 180.0, "pan");
            require_in_range(tilt_value, -180.0, 180.0, "tilt");
            controller_.set_velocity({
                *DegreesPerSecond::from(pan_value),
                *DegreesPerSecond::from(tilt_value),
            });
            send_json(response, 202, {{"accepted", true}, {"lease_ms", 250}});
        }));

        server_.Put("/v1/zoom", guarded([this](const auto& request, auto& response) {
            const json body = parse_body(request);
            const double value = required_number(body, "ratio");
            const auto& range = controller_.capabilities().zoom;
            require_in_range(value, range.minimum.value(), range.maximum.value(), "ratio");
            controller_.set_zoom(*ZoomRatio::from(value));
            send_json(response, 200, state_json(controller_));
        }));

        server_.Put("/v1/tracking", guarded([this](const auto& request, auto& response) {
            if (!controller_.capabilities().supports_tracking) {
                throw RequestError("Tracking is not supported by this camera profile");
            }
            const bool enabled = required_boolean(parse_body(request), "enabled");
            controller_.set_tracking(enabled);
            send_json(response, 200, state_json(controller_));
        }));

        server_.set_error_handler([](
            const httplib::Request&, httplib::Response& response) {
            if (response.status == 404) {
                send_json(response, 404, {{"error", "Endpoint not found"}});
            }
        });
    }

    CameraController& controller_;
    std::string host_;
    std::uint16_t port_;
    httplib::Server server_;
    std::thread thread_;
    bool started_ = false;
};

ApiServer::ApiServer(CameraController& controller, std::string host, std::uint16_t port)
    : implementation_(std::make_unique<Implementation>(controller, std::move(host), port)) {}

ApiServer::~ApiServer() = default;

std::uint16_t ApiServer::start() {
    return implementation_->start();
}

void ApiServer::stop() {
    implementation_->stop();
}

}  // namespace linkd
