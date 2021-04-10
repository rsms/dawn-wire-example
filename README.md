# dawn client-server example

The goal of this demo is to create a minimal but functional
[Dawn](https://dawn.googlesource.com/dawn)-based WebGPU client and server
with the following traits:

- Client is like a web page that uses a high-level WebGPU API to create
  scenes and perform computations.
- Client does not have any access to the host OS except for one duplex
  file descriptor connected to the server.
- Server speaks with the OS, has an OS window which it draws to with dawn_native.
- The client may live on a different computer than the server
  (i.e. connected over a network)


## Build & run

1. Run `./setup.sh` which will fetch dawn, libev and build libev
2. Run `./build.sh server client` to build the client and server programs
3. In two terminals, run `out/debug/server` and `out/debug/client`

> Note: Tested on macOS 10.15 (x86_64) with clang 12

Watch-build-run mode is available with `-w` and `-run` to the build script:

```sh
./build.sh -w -run=./out/debug/server server
```

```sh
./build.sh -w -run=./out/debug/client client
```

Note: `-w` requires `fswatch` to be installed.
On macOS you can get it from homebrew with `brew install fswatch`
