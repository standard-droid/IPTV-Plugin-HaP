# Changelog

All notable changes to HaP will be documented here.

## [1.3.6] - 2026-11-07

Fixes found while testing 1.3.5 on an Amlogic S912 box with Vimu.

### Changed

- When the AceServe process dies but HTTPAceProxy still answers, the
  watchdog now restarts AceServe alone and leaves the proxy running, so
  players stay connected and the proxy's reconnect resumes them. Before,
  it restarted both, which closed every player connection. It also acts on
  the first check that finds the process gone instead of waiting for a
  second one.
- The proxy's stream reconnect keeps trying for at least 75 seconds after an
  outage starts (8 seconds apart once the first 5 attempts are used), long
  enough to cover an AceServe restart by the watchdog. The window can be
  changed with the `RECONNECT_WINDOW` environment variable.

### Fixed

- The null-packet keepalive sent to players during a reconnect never
  switched on when the engine's output did not start on an MPEG-TS packet
  boundary (seen on a live channel: 116 bytes before the first sync byte).
  The proxy now looks for three sync bytes 188 bytes apart at any offset in
  the first chunks of the stream.

## [1.3.5] - 2026-11-07

Two changes cherry-picked from the albersg/IPTV-Plugin-HaP fork, and stream
recovery adapted from the pedrosc10/IPTV-Plugin-HaP fork.

### Added

- When the engine stream of a channel drops while players are still
  connected, HTTPAceProxy keeps their connections open and starts a new
  engine session, up to 5 times (2, 4, 6, 8 and 8 seconds apart; the count
  resets after 60 seconds of streaming; 1.3.6 extends this). Until the new session delivers data,
  players get MPEG-TS null packets, which they discard, so their read timeout
  does not fire. HLS streams hand over to this after 5 failed playlist
  refreshes instead of retrying forever.
- A watchdog checks AceServe and HTTPAceProxy every 15 seconds while HaP is
  meant to be running. If one stops answering for two checks in a row it
  restarts it, at most once a minute, and stops trying after three
  recoveries in a row that do not bring it back.

### Changed

- The HTTPAceProxy broadcast reader no longer waits on each client in turn.
  A client that falls behind now drops its own oldest chunk instead of
  stalling the engine reader and every other client on the same channel, and
  clients share one buffer per chunk instead of a copy each.
- The proxy's HTTP server limits concurrent connections (32; extra requests
  get 503), times out clients that connect and send nothing, backs off when
  it runs out of file descriptors instead of spinning, and sends large bodies
  in full. Channel peer tests stop their engine session when the client goes
  away.

### Fixed

- `main_android.py` told the engine to use a live cache of 1048570000 bytes
  (about 1000 MB) whenever it asked for cache settings, overriding the "Live
  cache" value set in the Ace engine section. It now uses the same value,
  which HaP passes in `ACE_LIVE_CACHE_BYTES` (default 100 MB).
- The same code also told the engine to keep that cache on disk, while HaP
  starts it with `--live-cache-type memory`. It now says `memory` too, so the
  live cache can no longer end up on internal storage.
- A new client could be attached to a channel whose stream had already
  ended and then wait forever for data. Such a channel is now replaced with
  a fresh one.
- The engine connection queued every STATUS message (about one a second)
  for the whole broadcast; it now keeps only the newest 5.

## [1.3.4] - 2026-11-03

### Added

- An "Ace engine" section in the HaP screen to set the live memory cache
  size, UPnP port mapping, engine debug logging, and extra engine options
  (one option or value per line, as in `acestream.conf`). Saving restarts
  the engine if HaP is running. The defaults pass the same arguments as
  before (100 MB live cache, UPnP off, no debug logging).

### Changed

- The bundled AceServe `acestream.conf` enabled engine debug logging
  (`--log-debug 355`, `--log-modules root:D` and four `--debug-*` flags). The
  engine reads this file from its working directory on every start, so HaP
  now strips those options from the extracted copy during `prepare()`. This
  also applies to existing installs without re-extracting the zip.
- AceServe output is no longer mirrored line by line to logcat, where Android
  was dropping hundreds of `HaP-AceServe` lines every few seconds. It still
  goes to the in-app log; enable logcat output with
  `adb shell setprop log.tag.HaP-AceServe DEBUG` and restart HaP.
- The cache monitor only logs when the cache size changes instead of every
  15 seconds.

### Fixed

- The `ace-patcher` thread in `main_android.py` scanned every live Python
  object (`gc.get_objects()`) every 0.5 seconds for as long as the engine
  ran, because `CoreApp` is never created on the `aceserve.main()` start
  path. It now polls quickly only for the first 60 seconds and then every
  30 seconds.

## [1.3.3] - 2026-09-09

### Fixed

- The 1.3.2 per-ABI APKs only dropped the other ABI's unused *native
  libraries* (~20MB); the AceServe engine's per-ABI zip
  (`assets/aceserve/<abi>/ace-<abi>.zip`, ~40-45MB) still shipped in full in
  every APK, including the per-ABI ones, because `android.splits.abi` only
  filters `lib/<abi>/`, not `assets/`. Per-ABI product flavors now also give
  each ABI its own asset source set, dropping the other ABI's AceServe zip
  too. The `armeabi-v7a`/`arm64-v8a` APKs are now ~65-70MB (down from
  ~105-110MB in 1.3.2), instead of the universal APK's ~125MB.

## [1.3.2] - 2026-09-09

### Fixed

- The universal APK (both ABIs bundled) is ~125MB because of the kubo (IPFS)
  binary; on storage-constrained 32-bit Android TV boxes this could make a
  clean install fail with a generic "app not installed" error even though
  the armeabi-v7a native library itself was present and valid. The release
  now also publishes per-ABI APKs
  (`IPTV-Plugin-HaP-<version>-armeabi-v7a.apk`, `-arm64-v8a.apk`), each about
  20MB smaller by dropping the other ABI's unused native libraries, alongside
  the existing universal APK.

## [1.3.1] - 2026-09-08

### Fixed

- Enabling the external player server no longer sends a proxy restart and a
  service start at the same time. With the service already running, the
  finished supervisor thread let `ACTION_START` spawn a new one alongside the
  `proxy-restart` thread, so HTTPAceProxy was stopped and started twice at
  once; when the readiness check lost the race, the supervisor tore down
  AceServe and the proxy and left the service in the `Failed` phase.
  `HapBridge` now picks a single service action per configuration change: a
  stopped service starts (it reads the listen host on start), a running one
  only restarts the proxy, and only when the listen host actually changed.
- `cast.rewriteUrl` in `StreamVaultHapPluginService` used the same
  server-mode-then-start pair and had the same double restart; it now goes
  through the shared `HapBridge.enableServerModeAndStart()` helper.

## [1.3.0] - 2026-09-02

### Added

- IPFS panel with a master switch that prepares and runs a bundled kubo
  (IPFS) daemon per device ABI (`arm64-v8a`, `armeabi-v7a`), independent from
  the AceServe/HTTPAceProxy runtime supervisor.
- Local IPFS gateway (default `http://127.0.0.1:8080`) that binds to the LAN
  when the existing "LAN server" switch is on, mirroring HTTPAceProxy
  exposure.
- Advanced IPFS settings: external node mode (host/port of an already-running
  IPFS node instead of the bundled one), gateway port, and disk storage
  quota.
- `tools/fetch-kubo.sh` to build the `libipfs.so` binaries locally (arm64-v8a
  needs only Go; armeabi-v7a requires the Android NDK for cgo linking); CI
  builds and caches them before assembling the release APK.
- `:app:preBuild` Gradle check that fails with a clear message if the IPFS
  binaries are missing from `jniLibs`.

## [1.2.2] - 2026-06-28

### Added

- Local TS proxy (startTsfProxy) in `handlePreparePlayback` that strips AceStream 72-byte framing headers from streams before forwarding to ExoPlayer, using raw TCP sockets instead of HttpURLConnection
- Triple 0x47 sync-byte verification at 188-byte intervals to handle AceStream framing headers of any size
- Batch packet writes to reduce syscall overhead during TS streaming
- Automatic retry mechanism (up to 60 attempts, 2s interval) when the HaP proxy resets the connection — the ExoPlayer socket stays alive during retries
- HTTP redirect following (302) in the proxy thread
- `PeerResultInfo.errorResult()` public factory method for creating error results from outside the package
- Diagnostic logging with `HaP-TSF` tag for TS proxy debugging

### Fixed

- Channel list height now uses WRAP_CONTENT instead of dp(320), fixing scrolling on phone form factors where nested ScrollViews intercepted touch events
- Test errors now store a `PeerResultInfo` in `peerResults` map so each channel row displays the error (red pill + error text) instead of only showing it in the feedback area

## [1.2.1] - 2026-06-01

### Added

- Added transparent support for `api.acestream.me/all` and `api.acestream.me/search` sources, formatting AceStream API JSON results as M3U channels internally.

## [1.2.0] - 2026-06-01

### Added

- Added standalone launcher support for phones and Android TV/Leanback so HaP can be opened without StreamVault.
- Added an External player server mode that enables LAN exposure, starts HaP, and remembers the mode for automatic startup after boot or app update.
- Added local and LAN M3U/EPG URL rows in the configuration UI for players such as TiviMate, OTT, and other IPTV clients.
- Added the installed app version next to the HaP title in the configuration UI.

### Changed

- Updated app labeling and plugin metadata to describe HaP as both a StreamVault companion and an external IPTV player server.
- Refreshed the launcher icon and Android TV banner with HaP lettering and multimedia branding.
- Made Channel status collapsed by default.

## [1.1.5] - 2026-05-18

### Fixed

- Fixed a race condition on first start where the HTTP proxy could fail to open because AceServe's API port (62062) was not yet ready when the HTTP port (6878) responded first. The supervisor now requires both ports to be accepting connections before starting the proxy.

## [1.1.4] - 2026-05-17

### Added

- Transparent support for M3U sources protected by the TestCookie-Nginx JavaScript challenge. Sources that respond with a `slowAES`-based page now have the AES-CBC cookie puzzle solved locally using `javax.crypto` (no new dependencies) and are retried automatically with the resulting `__test` cookie. Solved cookies are cached per hostname for six hours, matching the server-side TTL. Both the runtime M3U downloader (`NativeHttpBridge`) and the source validator (`SourceValidator`) participate in challenge resolution.

## [1.1.3] - 2026-05-14

### Changed

- Advertise a canonical `x-tvg-url` in generated HaP playlists and serve `/aio/epg.xml` as a local XMLTV endpoint for StreamVault EPG sync.
- Preserve EPG URLs declared by user M3U sources and route StreamVault EPG requests to the first XMLTV source, falling back to a valid empty XMLTV document when no source EPG is available.

## [1.1.2] - 2026-05-14

### Changed

- Improved M3U source feedback so validation errors and successful additions are shown next to the form.
- Added direct gateway fallback for `dweb.link` IPFS/IPNS playlist URLs when Chromecast cannot connect to the gateway host.
- Made Channel status open by default, prepare HaP before loading lists, and show TV-focusable channel controls.

## [1.1.1] - 2026-05-12

### Changed

- Hide the plugin APK from Android and Android TV launchers while keeping StreamVault activity configuration available.

## [1.1.0] - 2026-05-12

### Added

- Initial public release of the StreamVault HaP Plugin APK.
- StreamVault plugin API service with support for M3U provider, playback preparation, Cast URL rewriting, and native activity configuration.
- Native Android/TV configuration activity for runtime controls, source management, connected clients, channel status testing, and logs.
- User-managed M3U sources with validation for AceStream-compatible content.
- Custom channel editor for manually adding AceStream IDs.
- Bundled AceServe and HTTPAceProxy runtime integration for local playback.
- Signed APK release automation through GitHub Actions.
- GitHub Release publishing with versioned and stable APK download assets.
