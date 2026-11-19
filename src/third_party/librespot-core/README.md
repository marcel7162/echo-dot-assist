# librespot-core 0.8.0, patched for hassmic

The core of [librespot](https://github.com/librespot-org/librespot), as published on crates.io (`librespot-core`
0.8.0), used by the Spotify speaker (`src/spotify/`, `[patch.crates-io]` in its `Cargo.toml`). Four changes,
each marked "hassmic" in the code:

- `src/util.rs` `solve_hash_cash`: the hash cash challenges Spotify sets before a client token and a login5 token
  are solved on every core instead of one. Each challenge has 5 s; one core of an Echo Dot 3 (MediaTek MT8516,
  Cortex-A35 in 32-bit mode) missed that for one in three, and the speaker never logged in.
- `build.rs`: fixed version strings instead of asking git and the clock (inside this repository git named hassmic's
  commit, so the binary changed with every commit). Its build dependencies (vergen-gitcl, rand, rand_distr) are out of
  `Cargo.toml` with it.
- `src/config.rs` `OS`: "linux" on Android. Built for the Echos (Android), librespot spoke to Spotify as its
  Android app (client id, platform, version), and Spotify refused that client the key of every track ("error audio
  key 0 1"): nothing played. As on Linux it is the desktop client librespot is tested as (upstream's handshake
  already sends Linux for Android).
- `src/lib.rs`: one `allow` for a lint upstream's code sets off, which Cargo shows for a path dependency (one from
  crates.io has its lints capped).

Everything else is upstream's, byte for byte (`Cargo.toml.orig` is upstream's manifest).

## Licence

MIT, as librespot:

    The MIT License (MIT)

    Copyright (c) 2015 Paul Lietar

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
