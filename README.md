[![Build Status](https://github.com/neutrinolabs/xorgxrdp/actions/workflows/build.yml/badge.svg)](https://github.com/neutrinolabs/xorgxrdp/actions)

[![Latest Version](https://img.shields.io/github/v/release/neutrinolabs/xorgxrdp.svg?label=Latest%20Version)](https://github.com/neutrinolabs/xorgxrdp/releases)

# xorgxrdp

## Fork: GBM-backed glamor + Intel Xe (dma-buf hardware capture)

This fork of xorgxrdp adds the GPU surface plumbing needed for **dma-buf hardware
video capture/encode** — the companion to the VA-API H.264 encoder in
[pletch/xrdp-vaapi-encode](https://github.com/pletch/xrdp-vaapi-encode). The
`xrdp_accel_assist` hook itself is already upstream; this fork makes the screen pixmap
exportable as a dma-buf and enables recent Intel GPUs.

### What changed
* **glamor: back the screen pixmap with GBM BOs** — makes the screen pixmap
  dma-buf-exportable, the input to the hardware encoder
  (cherry-picked from [FlyGoat/xorgxrdp](https://github.com/FlyGoat/xorgxrdp))
* **dri3: authenticate primary-node client fds** (FlyGoat)
* **glamor: stop using raw DRM ioctls** (FlyGoat)
* **xrdpdev: add `xe` to `DRMAllowList`** — engage DRI3/glamor on the new Intel Xe
  KMD (e.g. UHD 770 / Alder Lake-S)
* **sockets under `/var/run/xrdp/<uid>` with `X11-` display names** — align socket
  layout/naming with the xrdp side
* **AVC444 capability signalling** — a session-capability message tells
  `xrdp_accel_assist` whether the connected client negotiated AVC444, so the helper
  encodes the auxiliary chroma view only when the client can use it
* **adaptive capture pacing** — the capture interval is per connection and
  steered by the client's own acknowledgement round trip, so one session can
  serve clients of very different speeds
* **capture-ahead** — a second capture buffer, each carrying the damage it
  missed, so the pipeline is not idle for a whole round trip
* **configurable refresh rate** — `XORGXRDP_VFREQ` replaces a hard-coded 50 Hz
* **capture timing** — `XORGXRDP_TIMING=1` reports per-frame capture, handoff,
  send-to-ack, idle, in-flight, client round trip and the current interval

### Used with
[pletch/xrdp-vaapi-encode](https://github.com/pletch/xrdp-vaapi-encode) built with
`--enable-vaapi`. Verified end-to-end on Intel UHD 770 with the iHD driver: hardware
VA-API H.264 with roughly 5× less server CPU than software x264. The encoder probes
for a usable H.264 profile/entrypoint rather than requiring one, so it is not limited
to iHD — see that repo's README for the candidate list.

### Adaptive capture pacing

`msFrameInterval` is the minimum spacing between captures. Fixed, it has to be
tuned to the slowest client that will connect, and then throttles every other
client sharing the session. Measured on one host, same session and same
content: a native client acknowledged frames in **11-18 ms** while a browser
client at HiDPI took **222-289 ms**. No single constant serves both.

`XORGXRDP_ADAPTIVE_PACE=1` moves the interval to the connection and steers it
from the round trip **xrdp** measures to that client, which arrives on the
frame-acknowledgement message and is reported as `client rtt`. The loop moves a
quarter of the way towards the smoothed round trip in both directions, backing
off on the first bad sample and taking rate back after four good ones.
`XORGXRDP_PACE_MIN_MS` and `_MAX_MS` bound it (default 20-100), and
`h264_frame_interval` in `xrdp.ini` becomes only the seed.

**The signal has to come from xrdp.** The send-to-acknowledge gap xorgxrdp can
measure locally starts at *its own* send, and it does not send again until
`msFrameInterval` has elapsed — so that gap contains the interval being set,
and steering by it is positive feedback that settles against the ceiling. It
read 18-20 ms while the true client latency ranged 1-13, and under load moved
fourfold where the true figure moved twentyfold.

Measured effect on a native client with a saturating source, together with
capture depth 2: **26 fps to 51**, blocked capture callbacks from 155-173 per
hundred frames down to 1-6, idle time from 36 ms to 18-20.

### Advertised refresh rate

The virtual output has no physical refresh rate, and `rdpRRConnectOutput` has
reported a hard-coded 50 Hz since 2017, in a commit whose subject was
"randr change to prevent xrandr app crashes" — the number was there to give
`xrandr` something to print. It matters more than that: applications that pace
to the RandR mode will not paint faster than it, so 60 fps content on a 50 Hz
output drops every sixth frame at irregular intervals.

`XORGXRDP_VFREQ` overrides it (1-240, default 50). Raising it raises the damage
rate for **every** client on the session, so a client that cannot keep up pays
in latency rather than frames — on one host 60 Hz measurably helped a native
client and slightly hurt a browser client sharing the same session.

### Capture buffers and the AVC444 auxiliary view

`rdpCaptureSufA2` / `rdpCaptureGfxA2` copy only the **current damage boxes** into the
accel-assist pixmap, and there is deliberately **one** buffer per monitor. That is
self-correcting: the buffer accumulates every box ever copied, so it converges on a
complete picture.

`XORGXRDP_CAPTURE_DEPTH=2` allocates a second buffer and alternates them, so the
next frame can be captured while the previous is still in flight. Alternating
naively breaks that invariant — neither buffer is ever complete, since each holds
only the boxes that happened to land in it. The main view survives, because its
shader reads back exactly the rects just written, but the **AVC444 auxiliary view
is rendered full-frame** and samples the whole source texture, so outside the
current damage it reads stale pixels. That showed as blocks of stale content after
anything scattering damage across the screen, such as dragging a window.

Each buffer therefore carries a debt: the damage it has missed since it was last
written. A capture copies the current damage unioned with that debt, clears it, and
adds the current damage to the other buffer's. The rects grow to even boundaries
like the ones sent to the client, since chroma is half resolution and an odd edge
leaves half a sample unwritten. A newly registered buffer owes the whole screen.
The client is still told only about the current damage; only the GPU copy widens,
and that measures 0 ms.

Depth 2 matters because lockstep leaves the pipeline idle for a whole round trip.
With adaptive pacing driving a native client to a 16 ms interval it was the binding
constraint: blocked callbacks ran at 155-173 per hundred frames and the frame rate
stayed at 27 fps however low the interval went. With both, 51 fps.

### Multi-GPU hosts: both sides must pick the same GPU

This is the most likely setup failure on a machine with more than one render node
(discrete card plus integrated graphics, or two discrete cards).

xorgxrdp and `xrdp-accel-assist` open a DRM device **independently**, and both default
to `/dev/dri/renderD128` — whichever GPU happened to enumerate first, which is not
guaranteed to be the one you want, or the same one across reboots. xorgxrdp allocates
the glamor screen pixmap's GBM BO on its device and exports it as a dma-buf; the
encoder imports that dma-buf into a VA surface on its device. If the two are different
GPUs, the import fails and the session falls back to software encoding.

| side | xorg.conf option | environment override |
| ---- | ---------------- | -------------------- |
| xorgxrdp | `Option "DRMDevice" "/dev/dri/renderDxxx"` | `XORGXRDP_DRM_DEVICE` |
| xrdp-accel-assist | — | `XRDP_VAAPI_DEVICE` (sesman `[SessionVariables]`) |

The environment variable wins where both are set: `rdpProbe` reads the xorg.conf value,
and `rdpPreInit` overrides it from `XORGXRDP_DRM_DEVICE` afterwards.

Identify the nodes first — `ls -l /dev/dri/by-path/` maps them to PCI addresses, and
`vainfo --display drm --device /dev/dri/renderD129` shows which one has the H.264
encode entrypoints. Then pin **both** sides to that node explicitly rather than relying
on the `renderD128` default. Each side logs the device it opened:

```
rdpPreInit: /dev/dri/renderD128 open ok, fd 12          # Xorg log
vaapi_init: VA-API 1.22 driver 'Intel iHD driver ...'   # xrdp-accel-assist.NN.log
```

Note also `Option "DRMAllowList"` (default `"amdgpu i915 xe msm radeon"`): glamor is
only enabled when the DRM driver name matches an entry, so an unlisted driver silently
takes the software path with `rdpPreInit: unsupported render node` in the Xorg log.

## Overview

**xorgxrdp** is a collection of modules to be used with a pre-existing X.Org
install to make the X server act like X11rdp. Unlike X11rdp, you don't have to
recompile the whole X Window System. Instead, additional modules are installed to
a location where the existing Xorg installation would pick them.

xorgxrdp is to be used together with [xrdp](https://github.com/neutrinolabs/xrdp)
and X.Org Server. It is pretty useless using xorgxrdp alone.

![xorgxrdp overview](https://github.com/neutrinolabs/xorgxrdp/raw/gh-pages/docs/xorgxrdp_overview.png)

## Features

xorgxrdp supports screen resizing. When an RDP client connects, the screen is
resized to the size supplied by the client.

xorgxrdp uses 24 bits per pixel internally. xrdp translates the color depth for
the RDP client as requested. RDP clients can disconnect and reconnect to the same
session even if they use different color depths.

## Compiling

### Pre-requisites

To compile xorgxrdp from the packaged sources, you need basic build tools - a
compiler (**gcc** or **clang**) and the **make** program. Additionally, you would
need **nasm** (Netwide Assembler) and the development package for X Window System
(look for **xserver-xorg-dev**, **xorg-x11-server-sdk** or
**xorg-x11-server-devel** in your distro).

To compile xorgxrdp from a checked out git repository, you would additionally
need **autoconf**, **automake**, **libtool** and **pkgconfig**.

### Get the source and build it

xorgxrdp requires a header file from xrdp. So it's preferred that xrdp is
compiled and installed first.

If compiling from the packaged source, unpack the tarball and change to the
resulting directory.

If compiling from a checked out repository, run `./bootstrap` first to create
`configure` and other required files.

Then run following commands to compile and install xorgxrdp:

```
./configure
make
sudo make install
```

If you don't want to install xrdp first, you can compile xorgxrdp against xrdp
sources by specifying XRDP_CFLAGS on the `configure` command line.

```
./configure XRDP_CFLAGS=-I/path/to/xrdp/common
```

## Usage

When logging in to xrdp using an RDP client, make sure to select Xorg on the
login screen. xrdp will tell xrdp-sesman to start Xorg with the configuration
file that activates the xorgxrdp modules.

Make sure your system has the X.Org server (typically **xserver-xorg-core** or
**xorg-x11-server-Xorg** package). Check that Xorg is in the standard path
(normally in `/usr/bin`).

## Contributing

First off, thanks for taking your time for improve xorgxrdp.

If you contribute to xorgxrdp, checkout **devel** branch and make changes to
the branch. Please make pull requests also versus **devel** branch.

To debug xorgxrdp, you can run Xorg with xorgxrdp manually:

```
Xorg :10 -config xrdp/xorg.conf
```

See also the `tests` directory for the tests that exercise xorgxrdp modules.
