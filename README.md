# dawn client-server example

1. Run `./setup.sh` which will fetch dawn, libev and build libev
2. Run `./build.sh server client` to build the client and server programs
3. In two terminals, run `out/debug/server` and `out/debug/client`

> Note: Tested on macOS 10.15 (x86_64) with clang 12

Watch-build-run mode is available with `-w` and `-run` to the build script:

```sh
./build.sh -w -run=./out/debug/a-server -- a-server
```

```sh
./build.sh -w -run=./out/debug/a-client -- a-client
```

Note: `-w` requires `fswatch` to be installed.
On macOS you can get it from homebrew with `brew install fswatch`
