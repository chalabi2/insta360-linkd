#include "linkd/grpc_server.hpp"

#include "linkd/controller.hpp"
#include "linkd/v1/control.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>

#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace linkd {
namespace {

namespace rpc = ::insta360::linkd::v1;

constexpr std::uint32_t kDefaultSubscriptionIntervalMs = 100;
constexpr std::uint32_t kMinimumSubscriptionIntervalMs = 50;
constexpr std::uint32_t kMaximumSubscriptionIntervalMs = 1000;
constexpr std::uint32_t kVelocityLeaseMs = 250;

void enable_reflection() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    });
}

class RequestError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require_range(double value, double minimum, double maximum, const char* name) {
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        throw RequestError(
            std::string(name) + " must be between " + std::to_string(minimum) +
            " and " + std::to_string(maximum));
    }
}

void fill_state(
    CameraController& controller,
    CameraState state,
    rpc::CameraState& response) {
    const auto& identity = controller.identity();
    response.set_camera_name(identity.product_name);
    response.set_usb_vendor_id(identity.vendor_id);
    response.set_usb_product_id(identity.product_id);
    response.mutable_position()->set_pan_degrees(state.position.pan.value());
    response.mutable_position()->set_tilt_degrees(state.position.tilt.value());
    response.set_zoom_ratio(state.zoom.value());
    response.set_tracking_enabled(state.tracking_enabled);
    if (const auto error = controller.background_error()) {
        response.set_background_error(*error);
    }
}

void fill_range(const Range<Degrees>& source, rpc::NumericRange& target) {
    target.set_minimum(source.minimum.value());
    target.set_maximum(source.maximum.value());
    target.set_step(source.step.value());
}

void fill_range(const Range<ZoomRatio>& source, rpc::NumericRange& target) {
    target.set_minimum(source.minimum.value());
    target.set_maximum(source.maximum.value());
    target.set_step(source.step.value());
}

grpc::Status request_failure(const RequestError& error) {
    return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
}

grpc::Status camera_failure(const CameraError& error) {
    return {grpc::StatusCode::UNAVAILABLE, error.what()};
}

class LinkControlService final : public rpc::LinkControl::Service {
public:
    explicit LinkControlService(CameraController& controller) : controller_(controller) {}

    grpc::Status GetStatus(
        grpc::ServerContext*,
        const rpc::StatusRequest*,
        rpc::CameraState* response) override {
        try {
            fill_state(controller_, controller_.status(), *response);
            return grpc::Status::OK;
        } catch (const CameraError& error) {
            return camera_failure(error);
        }
    }

    grpc::Status GetCapabilities(
        grpc::ServerContext*,
        const rpc::CapabilitiesRequest*,
        rpc::CameraCapabilities* response) override {
        const auto& capabilities = controller_.capabilities();
        fill_range(capabilities.pan, *response->mutable_pan_degrees());
        fill_range(capabilities.tilt, *response->mutable_tilt_degrees());
        fill_range(capabilities.zoom, *response->mutable_zoom_ratio());
        response->set_supports_tracking(capabilities.supports_tracking);
        response->set_velocity_lease_ms(kVelocityLeaseMs);
        response->set_requires_active_video(capabilities.requires_active_video);
        return grpc::Status::OK;
    }

    grpc::Status Publish(
        grpc::ServerContext*,
        const rpc::ControlCommand* request,
        rpc::PublishReply* response) override {
        try {
            apply(*request);
            response->set_accepted(true);
            response->set_velocity_lease_ms(
                request->action_case() == rpc::ControlCommand::kVelocity
                    ? kVelocityLeaseMs
                    : 0U);
            fill_state(controller_, controller_.cached_status(), *response->mutable_state());
            return grpc::Status::OK;
        } catch (const RequestError& error) {
            return request_failure(error);
        } catch (const CameraError& error) {
            return camera_failure(error);
        }
    }

    grpc::Status Subscribe(
        grpc::ServerContext* context,
        const rpc::SubscribeRequest* request,
        grpc::ServerWriter<rpc::CameraState>* writer) override {
        std::uint32_t interval_ms = request->interval_ms();
        if (interval_ms == 0) {
            interval_ms = kDefaultSubscriptionIntervalMs;
        }
        if (interval_ms < kMinimumSubscriptionIntervalMs ||
            interval_ms > kMaximumSubscriptionIntervalMs) {
            return {
                grpc::StatusCode::INVALID_ARGUMENT,
                "interval_ms must be between 50 and 1000",
            };
        }

        while (!context->IsCancelled()) {
            rpc::CameraState response;
            fill_state(controller_, controller_.cached_status(), response);
            if (!writer->Write(response)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
        return grpc::Status::OK;
    }

private:
    void apply(const rpc::ControlCommand& command) {
        const auto& capabilities = controller_.capabilities();
        switch (command.action_case()) {
            case rpc::ControlCommand::kPosition: {
                const double pan = command.position().pan_degrees();
                const double tilt = command.position().tilt_degrees();
                require_range(
                    pan,
                    capabilities.pan.minimum.value(),
                    capabilities.pan.maximum.value(),
                    "pan_degrees");
                require_range(
                    tilt,
                    capabilities.tilt.minimum.value(),
                    capabilities.tilt.maximum.value(),
                    "tilt_degrees");
                controller_.set_position({*Degrees::from(pan), *Degrees::from(tilt)});
                return;
            }
            case rpc::ControlCommand::kVelocity: {
                const double pan = command.velocity().pan_degrees_per_second();
                const double tilt = command.velocity().tilt_degrees_per_second();
                require_range(pan, -180.0, 180.0, "pan_degrees_per_second");
                require_range(tilt, -180.0, 180.0, "tilt_degrees_per_second");
                controller_.set_velocity({
                    *DegreesPerSecond::from(pan),
                    *DegreesPerSecond::from(tilt),
                });
                return;
            }
            case rpc::ControlCommand::kZoom: {
                const double ratio = command.zoom().ratio();
                require_range(
                    ratio,
                    capabilities.zoom.minimum.value(),
                    capabilities.zoom.maximum.value(),
                    "zoom_ratio");
                controller_.set_zoom(*ZoomRatio::from(ratio));
                return;
            }
            case rpc::ControlCommand::kTracking:
                if (!capabilities.supports_tracking) {
                    throw RequestError("Tracking is not supported by this camera");
                }
                controller_.set_tracking(command.tracking().enabled());
                return;
            case rpc::ControlCommand::kCenter:
                controller_.center();
                return;
            case rpc::ControlCommand::ACTION_NOT_SET:
                throw RequestError("ControlCommand.action is required");
        }
        throw RequestError("Unknown ControlCommand.action");
    }

    CameraController& controller_;
};

}  // namespace

bool grpc_available() noexcept {
    return true;
}

class GrpcServer::Implementation {
public:
    Implementation(CameraController& controller, std::string host, std::uint16_t port)
        : service_(controller),
          endpoint_(std::move(host) + ':' + std::to_string(port)) {}

    ~Implementation() {
        stop();
    }

    std::uint16_t start() {
        if (server_ != nullptr) {
            return port_;
        }

        grpc::ServerBuilder builder;
        enable_reflection();
        int selected_port = 0;
        builder.AddListeningPort(
            endpoint_, grpc::InsecureServerCredentials(), &selected_port);
        builder.RegisterService(&service_);
        server_ = builder.BuildAndStart();
        if (server_ == nullptr || selected_port <= 0 || selected_port > 65535) {
            server_.reset();
            throw CameraError("Could not bind the gRPC server to " + endpoint_);
        }
        port_ = static_cast<std::uint16_t>(selected_port);
        thread_ = std::jthread([this] { server_->Wait(); });
        return port_;
    }

    void stop() {
        if (server_ == nullptr) {
            return;
        }
        server_->Shutdown();
        if (thread_.joinable()) {
            thread_.join();
        }
        server_.reset();
    }

private:
    LinkControlService service_;
    std::string endpoint_;
    std::unique_ptr<grpc::Server> server_;
    std::jthread thread_;
    std::uint16_t port_ = 0;
};

GrpcServer::GrpcServer(
    CameraController& controller,
    std::string host,
    std::uint16_t port)
    : implementation_(
          std::make_unique<Implementation>(controller, std::move(host), port)) {}

GrpcServer::~GrpcServer() = default;

std::uint16_t GrpcServer::start() {
    return implementation_->start();
}

void GrpcServer::stop() {
    implementation_->stop();
}

}  // namespace linkd
