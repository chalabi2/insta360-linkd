# Architecture rationale

## Problem

`linkd` must control one Insta360 Link webcam from gamepads and arbitrary software on Linux, macOS, and eventually Windows. The Link 2 Pro exposes standard UVC pan, tilt, and zoom controls, but each operating system reaches those controls through a different API. Continuous joystick motion also needs a safe stop path because the camera exposes absolute positions rather than a portable velocity control.

## Shape

The core owns semantic values such as degrees, degrees per second, normalized axes, zoom ratio, position, and velocity. Raw CLI strings, JSON numbers, SDL axis integers, USB bytes, and V4L2 values become those types at their respective boundaries. Core motion and gamepad mapping remain pure functions.

One `CameraBackend` instance owns the camera connection. A controller serializes
access from HTTP, gRPC, and the gamepad loop. Backends implement standard UVC
control through IOKit on macOS and V4L2 on Linux. Vendor extension-unit
commands live behind the same backend and use a profile selected from USB
identity and descriptors.

The distributable is one executable. SDL3, the HTTP server, JSON parser, gRPC,
and Protobuf are compiled into it. A Linux udev rule is the only optional
installed file.

## Synthesis decision

The selected design uses a typed C++ core, SDL3 for input, embedded localhost
HTTP and gRPC servers, and native per-OS UVC backends. This keeps the public API
and gamepad behavior identical without pretending that one USB implementation
works on every operating system.

## Tradeoffs accepted

- We accept thin OS-specific backends in exchange for reliable coexistence with each operating system's webcam driver.
- We accept a statically linked SDL3 dependency in exchange for consistent gamepad mappings and hot-plug behavior.
- We accept a small motion loop in exchange for portable velocity control over cameras that only advertise absolute pan and tilt.
- We accept localhost HTTP/1.1 in exchange for an API that shell scripts and desktop applications can call without an SDK.
- We accept the gRPC build cost in exchange for a versioned, generated-client
  API and a streaming state subscription.

## Alternatives considered

- Raw libusb lost because macOS already claims the video-control interface and does not reliably allow detaching it.
- Wrapping Insta360 Link Controller lost because the vendor does not ship it for Linux and its private WebSocket protocol adds a required background application.
- Separate shell scripts for each OS lost because they cannot provide one gamepad mapping, deadman stop, or stable API.

## Current hardware boundary

- Link 2 and Link 2 Pro are the explicit product profiles. Other Link generations need their extension-unit layout verified before being added.
- Pan, tilt, and zoom use standard UVC controls. Tracking uses the verified one-byte control at extension unit 11, selector 2.
- macOS hardware access and coexistence with active video have been proven on a Link 2 Pro (`2e1a:4c06`). The Linux implementation compiles against V4L2/UVC but still needs a physical-device test.
- Link firmware keeps the motors asleep without an active video consumer.
  `linkd` reports that capability but does not claim the video stream itself.
- LAN listening is opt-in through `--listen`; it has no authentication and defaults to loopback.

## Next validation step

Exercise the Linux build against a physical Link 2 or Link 2 Pro and confirm that `uvcvideo` exposes extension unit 11 unchanged.
