# gRPC control design

## Problem

`linkd` already serializes gamepad and HTTP commands through one
`CameraController`. gRPC clients need to publish the same typed commands and
subscribe to state without creating a second owner for the USB device. The
wire contract must distinguish position, velocity, zoom, tracking, and center
commands so an invalid mixture cannot reach the controller.

## Shape

The protobuf schema in `proto/linkd/v1/control.proto` is the source of truth.
`ControlCommand` uses a `oneof`, so every publish request contains exactly one
command variant. The gRPC adapter validates protobuf scalars at the network
boundary, converts them to the existing semantic C++ types, and calls the same
`CameraController` used by HTTP and gamepad input.

`Publish` is a unary RPC. `Subscribe` is a server stream of cached camera state.
Publishers and subscribers have independent lifetimes, which matches pub/sub
clients and avoids making a command producer keep a response stream open. The
stream never reads USB state itself, so adding subscribers does not multiply
camera traffic.

The gRPC listener uses `127.0.0.1:8766` by default. HTTP stays on port 8765.
Both adapters share one controller and its 250 ms velocity lease. Server
reflection is enabled for generic clients such as `grpcurl`.

## Synthesis decision

The selected design uses separate `Publish` and `Subscribe` methods with a
protobuf `oneof` command. The strongest alternative was one bidirectional
stream, but that couples command and subscription lifetimes. A state cache was
kept from the polling design, while direct USB reads per subscriber were
rejected because each subscriber would add hardware contention.

## Tradeoffs accepted

- We accept periodic state messages in exchange for simple cancellation and
  predictable subscriber behavior.
- We accept a separate gRPC port in exchange for keeping the existing HTTP API
  compatible.
- We accept the gRPC and protobuf build cost in release binaries in exchange
  for generated clients in multiple languages.

## Alternatives considered

- A bidirectional `Control` stream lost because a publisher that does not need
  state should not own a response stream.
- Mapping gRPC-shaped JSON onto the HTTP server lost because it would not be
  compatible with protobuf clients or HTTP/2 gRPC tooling.
- Reading the camera on every subscription interval lost because subscribers
  would contend for the same UVC interface.

## Future considerations

- A future version may publish discrete change events in addition to state
  snapshots.
- Remote TLS and authentication should be added before exposing gRPC outside a
  trusted network.
