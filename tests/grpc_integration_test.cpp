#include "linkd/camera.hpp"
#include "linkd/controller.hpp"
#include "linkd/grpc_server.hpp"
#include "linkd/v1/control.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

namespace {

using namespace linkd;
namespace rpc = ::insta360::linkd::v1;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

Degrees degrees(double value) {
    return *Degrees::from(value);
}

ZoomRatio zoom(double value) {
    return *ZoomRatio::from(value);
}

class FakeCamera final : public CameraBackend {
public:
    [[nodiscard]] const UsbIdentity& identity() const noexcept override {
        return identity_;
    }

    [[nodiscard]] const CameraCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] CameraState read_state() override {
        return state_;
    }

    void set_position(Position position) override {
        state_.position = position;
        state_.tracking_enabled = false;
    }

    void set_zoom(ZoomRatio ratio) override {
        state_.zoom = ratio;
    }

    void set_tracking(bool enabled) override {
        state_.tracking_enabled = enabled;
    }

private:
    UsbIdentity identity_{0x2e1a, 0x4c06, "Fake Insta360 Link 2 Pro"};
    CameraCapabilities capabilities_{
        .pan = {degrees(-145.0), degrees(145.0), degrees(1.0)},
        .tilt = {degrees(-90.0), degrees(100.0), degrees(1.0)},
        .zoom = {zoom(1.0), zoom(4.0), zoom(0.01)},
        .supports_tracking = true,
        .requires_active_video = true,
    };
    CameraState state_{
        .position = {degrees(0.0), degrees(0.0)},
        .zoom = zoom(1.0),
        .tracking_enabled = false,
    };
};

}  // namespace

int main() {
    CameraController controller(std::make_unique<FakeCamera>());
    GrpcServer server(controller, "127.0.0.1", 0);
    const std::uint16_t port = server.start();
    auto channel = grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    auto stub = rpc::LinkControl::NewStub(channel);

    {
        grpc::ClientContext context;
        rpc::StatusRequest request;
        rpc::CameraState response;
        const grpc::Status status = stub->GetStatus(&context, request, &response);
        require(status.ok(), "GetStatus must succeed");
        require(response.camera_name() == "Fake Insta360 Link 2 Pro", "camera name must map");
    }

    {
        grpc::ClientContext context;
        rpc::ControlCommand request;
        request.mutable_position()->set_pan_degrees(20.0);
        request.mutable_position()->set_tilt_degrees(-10.0);
        rpc::PublishReply response;
        const grpc::Status status = stub->Publish(&context, request, &response);
        require(status.ok(), "position Publish must succeed");
        require(response.accepted(), "Publish must acknowledge the command");
        require(response.state().position().pan_degrees() == 20.0, "Publish must return state");
    }

    {
        grpc::ClientContext context;
        rpc::ControlCommand request;
        request.mutable_position()->set_pan_degrees(999.0);
        rpc::PublishReply response;
        const grpc::Status status = stub->Publish(&context, request, &response);
        require(
            status.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
            "out-of-range position must fail at the RPC boundary");
    }

    {
        grpc::ClientContext context;
        rpc::SubscribeRequest request;
        request.set_interval_ms(50);
        std::unique_ptr<grpc::ClientReader<rpc::CameraState>> reader =
            stub->Subscribe(&context, request);
        rpc::CameraState response;
        require(reader->Read(&response), "Subscribe must emit an initial state");
        require(response.position().pan_degrees() == 20.0, "Subscribe must use cached state");
        context.TryCancel();
        static_cast<void>(reader->Finish());
    }

    server.stop();
    std::cout << "all gRPC integration tests passed\n";
    return 0;
}
