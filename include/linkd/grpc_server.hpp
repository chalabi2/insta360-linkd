#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace linkd {

class CameraController;

[[nodiscard]] bool grpc_available() noexcept;

class GrpcServer {
public:
    GrpcServer(CameraController& controller, std::string host, std::uint16_t port);
    ~GrpcServer();

    GrpcServer(const GrpcServer&) = delete;
    GrpcServer& operator=(const GrpcServer&) = delete;

    [[nodiscard]] std::uint16_t start();
    void stop();

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace linkd
