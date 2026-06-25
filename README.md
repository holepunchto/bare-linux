# bare-linux

An example Linux app that embeds the [Bare](https://github.com/holepunchto/bare) runtime (via [bare-kit](https://github.com/holepunchto/bare-kit)) in a native C host. It runs a shared on/off switch: launch two copies and flipping the switch in one flips it in the other - peer-to-peer over a distributed hash table, with no server. It is the Linux/C counterpart of [bare-macos](https://github.com/holepunchto/bare-macos).

The peer-to-peer half is JavaScript - a Hyperswarm node plus the switch state - shared as the [bare-switch-core](https://github.com/holepunchto/bare-switch-core) package and run on a Bare _worklet_ (a background JS thread the host starts). The native half is C: it boots the worklet and exchanges messages with it over bare-kit's IPC channel.

## Building and running

### Prerequisites

- A Linux machine on x64 or arm64 (glibc) - the prebuilt runtime is published for those. On a Mac, use a Linux environment instead - a remote host, a local VM (Lima, UTM, Multipass, ...), or a container. A worked example using Lima is below.
- Node 22+.
- A C toolchain - `build-essential`, `cmake`, and `clang` - plus `curl` and `unzip` (used to fetch the prebuilt runtime). `bare-make` brings its own Ninja, so you do not install that.

Run `npm install` and the build inside the Linux environment, since `node_modules` holds platform-specific native binaries.

### Build

From the repo root (each step auto-detects the host arch):

```sh
npm install
npm run fetch    # download the prebuilt libbare-kit.so for this arch
npm run bundle   # link the native addons + pack the worklet
npm run build    # generate + build the C host
npm start        # run it (Ctrl-C to stop)
```

On start the host prints `[host] worklet up ...` followed by an `[host] ipc frame: N bytes` line (the worklet's initial state). Launch a second copy in another terminal; once the two find each other on the DHT - usually within a minute - each prints more `ipc frame` lines - that is the instances syncing the switch. The worklet's own `console.log` does not surface on the host's stdout in this bare-kit build, so these `[host] ...` lines are how you know the channel is live.

To check a build non-interactively, `npm run smoke` boots the host, confirms an IPC frame arrives, and exits non-zero if none does - this is exactly what CI runs.

### Example: a Lima VM on an Apple Silicon Mac

The setup that worked for us - a Lima Ubuntu arm64 VM with this repo mounted, so you edit on the Mac and build inside the VM at native speed:

```sh
# Install Lima and start an Ubuntu arm64 VM with this repo mounted writable.
brew install lima
limactl start --name=bare --vm-type=vz --mount="$PWD:w" template:ubuntu-lts

# Install the toolchain inside the VM.
limactl shell bare sudo apt-get update
limactl shell bare sudo apt-get install -y build-essential cmake clang curl unzip
limactl shell bare bash -lc 'curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash - && sudo apt-get install -y nodejs'
```

Then open a shell in the VM - `limactl shell bare`, which lands in this same mounted path - and run the [Build](#build) steps above. `--vm-type=vz` uses Apple's Virtualization framework so the arm64 guest runs at native speed; `--mount="$PWD:w"` mounts the repo writable at the same path inside the guest.

## How it works

The native addons the worklet needs (`sodium-native`, `udx-native`, ...) are `dlopen`ed by the Bare runtime at runtime, not linked into the host. `npm run bundle` collects them into `app/addons/lib`, and the build records that directory (plus `app/lib`, which holds the prebuilt `libbare-kit.so`) in the executable's `DT_RPATH`, so everything resolves with no `LD_LIBRARY_PATH`. This mirrors how the iOS and Android bare-kit hosts make their `bare-link`ed addons discoverable, using the Linux rpath mechanism.

## License

Apache-2.0
