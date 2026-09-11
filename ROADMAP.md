# gipMultiplayer Roadmap

Planned work, grouped by priority.

## High Priority

### 1. CI/CD pipeline (GitHub Actions)

Catches regressions early and verifies builds on every PR and push to `main`.

- Windows (MSVC and MinGW): build plugin, build example app, run voice tests
- Linux (Ubuntu): build plugin, run tests
- Android (NDK): build plugin, optionally run on an emulator
- Cache dependencies (zstd, opus, OpenSSL)

### 2. Android platform integration

- Runtime `RECORD_AUDIO` permission request helper (Java/Kotlin bridge)
- ProGuard rules for znet and Opus
- Gradle integration docs

### 3. iOS/macOS platform integration

- `NSMicrophoneUsageDescription` guidance
- CocoaPods and Swift Package Manager notes
- Audio session category setup (playAndRecord, mixWithOthers)

### 4. Authentication hardening

Login/register/session-token flow exists (`MasterMain.cpp`) but nothing
downstream of it is actually enforced. Found during a security review of
the master server; full writeup of each item in `KNOWN_ISSUES.md`.

- Tie match/room join to the session token. Right now `joinLobby` and the
  dedicated server never check identity at all — a match needs nothing but
  a typed display name. Decide what auth is actually meant to gate before
  building this.
- Single active session per account. `CreateSessionForUser()` always
  inserts a new `SESSIONS` row and never revokes prior ones for that
  `user_id` — the same email+password can be logged in from two machines
  at once, indefinitely.
- Rate-limit session-token auto-login (`gMasterUserTokenLoginPacket`) —
  currently the only auth packet that never touches `gRateLimiter`.
- Rate-limit registration (`gMasterUserRegisterPacket`) — same gap,
  nothing stops scripted mass account creation.
- Minimum password length/strength on registration — the only current
  check is `password.empty()`.

## Medium Priority: Voice Quality and UX

### 5. Echo cancellation

- Integrate WebRTC AECM
- Enable when headphones are not detected
- Configurable through `voice.setEchoCancellation(true)`

### 6. Voice activity detection

- WebRTC VAD or Opus built-in DTX
- Voice activation mode: transmit only when speaking, no push-to-talk key
- Configurable sensitivity

### 7. Adaptive bitrate and Opus features

- DTX (discontinuous transmission) toggle
- FEC (forward error correction) toggle
- DRED (redundant audio) toggle
- Dynamic bitrate based on packet loss and RTT

### 8. Jitter buffer tuning

- Expose `initial_jitter_packets` and `max_jitter_packets` in the example
- Jitter and packet loss graph in the demo app

## Medium Priority: Architecture

### 9. Move singleton managers into `gApp`

`NetworkManager`, `ChatManager`, and similar managers are accessed through
`getInstance()` from anywhere in the codebase (39 call sites as of this
writing).

Why this is worth changing: lifetime isn't controlled (allocated on first
use, destructed whenever the runtime decides, not when the app decides);
it's a hidden dependency (nothing in a function's signature shows it
touches network/chat state); every caller shares one instance, so unrelated
systems (chat, voice, ping) end up coupled through shared global state; and
it isn't swappable for tests.

Suggested direction: own these managers as members of `gApp` (or an
equivalent root object) and pass references down, instead of a global
`getInstance()`.

Why it's not done yet: touches ~39 call sites across the plugin and the
game. Large mechanical change, no functional upside on its own, better done
as a dedicated pass rather than mixed into feature work.

### 10. Audit locks for lock-free (atomic) alternatives

Current count: 25+ distinct `std::mutex` members across `NetworkManager`,
`ChatManager`, `GameBackend*`, `NetworkSynchronizer`, `gVoiceAudioProcessor`,
`gTeamVoice`, `gTeamVoiceServer` (~159 lock-related lines total).

Many of these guard a single flag or counter (e.g. transmitting state, mute
state, ping snapshot) — plausible candidates for `std::atomic<T>` instead of
`mutex` + `lock_guard`, removing the lock entirely for that piece of state.

Rule to apply when touching any of these: never hold a lock across
blocking or slow work (device I/O, `sleep_for`, network calls). Every real
incident so far (see `KNOWN_ISSUES.md`) came from a lock held across real
work on one thread while another thread waited on it for something
trivial, not from lock count by itself.

Why it's not done yet: intentionally deferred. znet-side changes are in
progress that will affect this layer; better to do the lock-free pass once
that lands so it isn't reworked twice.

## Lower Priority: Example and Ecosystem

### 11. `GlistApp-TeamVoice` enhancements

- Lobby and room list through the master server
- Player list with per-player mute and volume sliders
- Text chat overlay
- Settings screen: mic gain, output device, push-to-talk key bind

### 12. Documentation site

MkDocs or GitHub Pages covering getting started, a Doxygen API reference, platform-specific guides and migration guides (znet 3.x to 4.0).

## How to Contribute

1. Pick an item from high or medium priority.
2. Create a branch named `feature/<short-name>`.
3. Open a draft PR early for discussion.
4. Check that Release and Debug builds pass, existing tests pass, the example app runs on Windows, and no new compiler warnings appear.
5. Request review.
