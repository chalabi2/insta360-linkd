# insta360-linkd

[![Release](https://github.com/chalabi2/insta360-linkd/actions/workflows/release.yml/badge.svg)](https://github.com/chalabi2/insta360-linkd/actions/workflows/release.yml)
[![Latest release](https://img.shields.io/github/v/release/chalabi2/insta360-linkd)](https://github.com/chalabi2/insta360-linkd/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Control an Insta360 Link 2 or Link 2 Pro gimbal from a gamepad, shell command,
HTTP, or gRPC. `linkd` talks to the camera's UVC controls directly on macOS and
Linux.

The camera firmware stays untouched. Insta360 Link Controller is not required,
and macOS can keep using the webcam as a video source while `linkd` moves it.

## What it does

- Pan, tilt, zoom, center, and tracking controls
- Optional SDL3 gamepad input with hot-plug support
- Local HTTP/JSON API with CORS
- Typed gRPC publish/subscribe API with server reflection
- Direct one-shot CLI commands
- A 250 ms deadman for velocity commands
- Native IOKit backend on macOS and V4L2/UVC backend on Linux

## Download

Download the archives and `SHA256SUMS` from the
[latest release](https://github.com/chalabi2/insta360-linkd/releases/latest).

| Archive | Host |
| --- | --- |
| `insta360-linkd-v0.1.0-darwin-arm64.tar.gz` | Apple silicon Mac |
| `insta360-linkd-v0.1.0-linux-amd64.tar.gz` | 64-bit x86 Linux |

### macOS

```sh
tar -xzf insta360-linkd-v0.1.0-darwin-arm64.tar.gz
cd insta360-linkd-v0.1.0-darwin-arm64
./bin/linkd status
```

The release binary has an ad-hoc code signature but is not notarized. If a
browser adds a quarantine attribute, macOS may block the first launch. After
checking the release checksum, remove that attribute with:

```sh
xattr -d com.apple.quarantine ./bin/linkd
```

### Linux

```sh
tar -xzf insta360-linkd-v0.1.0-linux-amd64.tar.gz
cd insta360-linkd-v0.1.0-linux-amd64
sudo install -m 0644 share/insta360-linkd/99-insta360-linkd.rules \
  /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
./bin/linkd status
```

Reconnect the camera after installing the udev rule. Some distributions also
require your account to belong to the `video` group.

## Keep the camera awake

The Link 2 Pro enters privacy mode roughly 10 seconds after the last video or
audio application releases it. The USB control request can still succeed while
the motors remain asleep. Open the camera in OBS, a meeting application, or
another video consumer before moving the gimbal. If it is already asleep,
starting that video source wakes it again.

`linkd` deliberately does not capture video itself. On Linux, doing so could
claim the V4L2 device and prevent the application that needs the picture from
opening it. This behavior comes from the camera firmware; Insta360 documents
the sleep and wake behavior in its
[privacy mode guide](https://onlinemanual.insta360.com/link2pro/en-us/faq/operation-guide/privacy).

The `requires_active_video` field in both capability APIs exposes this behavior
to clients.

## Start the server

API only, which is the default:

```sh
./bin/linkd serve
```

Enable the gamepad explicitly:

```sh
./bin/linkd serve --gamepad-enabled=true
```

HTTP listens on `http://127.0.0.1:8765/v1` and gRPC listens on
`127.0.0.1:8766`. The camera must be connected when the process starts. An
enabled gamepad can connect or reconnect later.

### Gamepad controls

| Input | Action |
| --- | --- |
| Left stick | Pan and tilt |
| Right trigger, R2 | Zoom in |
| Left trigger, L2 | Zoom out |
| South button, Cross, A | Center |
| North button, Triangle, Y | Toggle tracking |

Stick input uses a deadzone and a precision curve. Manual movement turns
tracking off before sending position changes.

## HTTP API

| Method | Path | JSON body | Result |
| --- | --- | --- | --- |
| `GET` | `/v1/status` | none | Current position, zoom, and tracking |
| `GET` | `/v1/capabilities` | none | Camera ranges and feature support |
| `POST` | `/v1/center` | none | Center the gimbal |
| `PUT` | `/v1/position` | `{"pan": 20, "tilt": -10}` | Set degrees |
| `PUT` | `/v1/velocity` | `{"pan": 45, "tilt": 0}` | Set degrees per second |
| `PUT` | `/v1/zoom` | `{"ratio": 1.5}` | Set optical zoom ratio |
| `PUT` | `/v1/tracking` | `{"enabled": false}` | Toggle AI tracking |

Read the live camera state:

```sh
curl http://127.0.0.1:8765/v1/status
```

Move to an absolute position:

```sh
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"pan": 20, "tilt": -10}' \
  http://127.0.0.1:8765/v1/position
```

Velocity requests expire after 250 ms. A client that wants continuous motion
should refresh `/v1/velocity` at 10 to 20 Hz. This prevents a crashed client
from leaving the gimbal moving.

The server binds to loopback by default. Listening on another interface exposes
an unauthenticated control endpoint, so do this only on a trusted network:

```sh
./bin/linkd serve --listen 0.0.0.0 --port 8765
```

## gRPC pub/sub API

The versioned service is `insta360.linkd.v1.LinkControl`:

| RPC | Direction | Purpose |
| --- | --- | --- |
| `Publish` | Unary | Send one typed position, velocity, zoom, tracking, or center command |
| `Subscribe` | Server stream | Receive camera-state snapshots at a requested interval |
| `GetStatus` | Unary | Read current hardware state |
| `GetCapabilities` | Unary | Discover ranges, features, and wake requirements |

Server reflection is enabled, so a generic client can inspect and call it:

```sh
grpcurl -plaintext 127.0.0.1:8766 list
grpcurl -plaintext -d '{"position":{"panDegrees":20,"tiltDegrees":-10}}' \
  127.0.0.1:8766 insta360.linkd.v1.LinkControl/Publish
grpcurl -plaintext -d '{"intervalMs":100}' \
  127.0.0.1:8766 insta360.linkd.v1.LinkControl/Subscribe
```

The authoritative schema is
[`proto/linkd/v1/control.proto`](proto/linkd/v1/control.proto), and it is also
included under `share/insta360-linkd/proto` in release archives. Generate a
client with any standard gRPC toolchain. Disable this listener with
`--grpc-enabled=false`, or change it with `--grpc-listen` and `--grpc-port`.
Like HTTP, a non-loopback listener is unauthenticated and should only be used
on a trusted network.

## Direct CLI

The one-shot commands do not start the HTTP server:

```sh
./bin/linkd status
./bin/linkd center
./bin/linkd position 20 -10
./bin/linkd zoom 1.5
./bin/linkd tracking off
```

Run `./bin/linkd` without arguments to print all options.

## Build from source

You need CMake 3.25 or newer, a C++20 compiler, Git, and the platform's USB
development headers. Linux gamepad builds also need `libudev` headers.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake downloads pinned SDL3, cpp-httplib, nlohmann/json, gRPC, and Protobuf
revisions during the first configure. It links them into the `linkd`
executable. For a smaller HTTP-only build, configure with
`-DLINKD_ENABLE_GRPC=OFF`.

Install to a chosen prefix with:

```sh
cmake --install build --prefix "$HOME/.local" --component linkd
```

## Hardware status

| Camera or platform | Status |
| --- | --- |
| Link 2 Pro `2e1a:4c06` on macOS | Verified with active video |
| Link 2 `2e1a:4c04` | Profile included, physical test needed |
| Linux V4L2/UVC backend | Builds and tests, physical test needed |
| Windows | Backend not implemented |

The verified Link 2 Pro reports pan from -145 to 145 degrees, tilt from -90 to
100 degrees, and zoom from 1.0 to 4.0. `linkd` reads the actual ranges from the
camera instead of hard-coding them.

Read [the architecture rationale](docs/architecture.md) for the USB control,
threading, and safety decisions.

## Releases

Pushing a tag shaped like `v1.2.3` runs the release workflow. It builds and
tests both hosts with gRPC enabled, packages the binaries, schema,
documentation, and Linux udev rule, writes SHA-256 checksums, and creates or
updates the matching GitHub release. A manual workflow run performs the same
build and packaging checks without publishing a release.

## License

[MIT](LICENSE)
