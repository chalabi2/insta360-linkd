#include "linkd/camera.hpp"

namespace linkd {

std::unique_ptr<CameraBackend> connect_camera() {
    throw CameraError("This operating system does not have a linkd camera backend yet");
}

}  // namespace linkd
