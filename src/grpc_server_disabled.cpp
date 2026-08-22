#include "linkd/grpc_server.hpp"

#include "linkd/camera.hpp"

#include <utility>

namespace linkd {

bool grpc_available() noexcept {
    return false;
}

class GrpcServer::Implementation {};

GrpcServer::GrpcServer(CameraController&, std::string, std::uint16_t)
    : implementation_(std::make_unique<Implementation>()) {}

GrpcServer::~GrpcServer() = default;

std::uint16_t GrpcServer::start() {
    throw CameraError(
        "gRPC support was disabled at build time; rebuild with LINKD_ENABLE_GRPC=ON");
}

void GrpcServer::stop() {}

}  // namespace linkd
