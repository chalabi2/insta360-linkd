#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace linkd {

class CameraController;

class ApiServer {
public:
    ApiServer(CameraController& controller, std::string host, std::uint16_t port);
    ~ApiServer();

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    [[nodiscard]] std::uint16_t start();
    void stop();

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace linkd
