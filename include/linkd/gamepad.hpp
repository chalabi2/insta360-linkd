#pragma once

#include <memory>

namespace linkd {

class CameraController;

class GamepadInput {
public:
    explicit GamepadInput(CameraController& controller);
    ~GamepadInput();

    GamepadInput(const GamepadInput&) = delete;
    GamepadInput& operator=(const GamepadInput&) = delete;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace linkd
