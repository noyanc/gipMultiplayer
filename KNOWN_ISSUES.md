# Known Issues

## Open

### Winsock `sendto` failure on teardown
- **Seen:** `game_martyr/martyr_log_1.txt` and `martyr_log_2.txt`, both
  client and P2P client paths, during connection teardown.
- **Symptom:** `ZDT: sendto failed: Either the application has not called
  WSAStartup, or WSAStartup failed.`
- **Likely cause:** a UDP socket sends after Winsock is torn down on that
  thread (WSACleanup already ran, or WSAStartup was never called on the
  thread doing the send). Needs tracing in znet's UDP backend teardown
  order.

### TCP connection dropped mid-handshake with the broker
- **Seen:** `game_martyr/martyr_log_2.txt`, P2P flow, talking to
  `martyr.irrl.dev`.
- **Symptom:** `Closing connection due to an error: A request to send or
  receive data was disallowed because the socket had already been shut
  down in that direction with a previous shutdown call.` Happens right
  after a clean connect; inconsistent — the connection before it in the
  same log round-tripped without error.
- **Likely cause:** shutdown-direction ordering bug in znet's TCP
  transport layer, not application logic.

Both are in the znet layer this repo depends on — flag for whoever
continues the znet-side work with the team.

### Auth exists but nothing downstream checks it
- **Where:** match/room joining (`NetworkManager::joinLobby`, the dedicated
  server, `GameBackend`) never references the master server's login/session
  system at all.
- **Problem:** the master server's login/register/token-login flow
  (`MasterMain.cpp`) only produces an account and a session token used for
  the profile UI ("Logged in as ..."). Joining and playing a match needs
  nothing but a typed display name; no session token is checked anywhere on
  the game/room path.
- **Impact:** authentication is decorative for anything except the account
  display name. Two people can play under the same account at once, under
  different accounts with the same display name, or with no account at all
  — the server can't tell the difference.
- **Fix direction:** decide what account identity is actually meant to gate
  (ranked play, persistent stats, anti-impersonation, or nothing) before
  building this. If it should gate anything, the room-join packet needs to
  carry the session token and the room/dedicated server needs to validate
  it against the master server before accepting the player.

### No single active session per account
- **Where:** `MasterMain.cpp`, `CreateSessionForUser()` (called from both
  the login and register handlers).
- **Problem:** every successful login/register inserts a new `SESSIONS`
  row. Nothing revokes or even looks at existing sessions for that
  `user_id` first.
- **Impact:** the same email+password can be logged in from two machines
  at once, indefinitely — both tokens stay valid until the 30-day prune.
  Direct cause of "two people, one account, both playing."
- **Fix direction:** on successful login, either delete existing sessions
  for that `user_id` before inserting the new one (strict single-session
  accounts), or keep multiple sessions but track "currently in a match" per
  `user_id` server-side and reject a second concurrent match-join for the
  same account.

### Session token auto-login has no rate limiting
- **Where:** `MasterMain.cpp`, `OnPacket(gMasterUserTokenLoginPacket)`.
- **Problem:** the password-login handler checks `gRateLimiter` before
  touching the database; the token auto-login handler never calls it.
- **Impact:** no lockout, no attempt tracking on this path. Token guessing
  is impractical at 256 bits, but there's no defense-in-depth here, and
  it's an easy way to hammer the DB with unauthenticated lookups.
- **Fix direction:** run token logins through `gRateLimiter` the same way
  password logins do, keyed by IP.

### Registration has no rate limiting
- **Where:** `MasterMain.cpp`, `OnPacket(gMasterUserRegisterPacket)`.
- **Problem:** unlike login, the register handler never touches
  `gRateLimiter`.
- **Impact:** nothing stops scripted mass account creation.
- **Fix direction:** apply the same IP-based limiter (or a stricter one) to
  registration.

### No minimum password strength
- **Where:** `MasterMain.cpp`, register handler — the only password check
  is `p->password.empty()`.
- **Problem:** a one-character password is accepted and PBKDF2-hashed like
  any other.
- **Impact:** weak accounts are easy to guess; rate limiting only slows
  brute force, it doesn't stop a short/common password from winning early.
- **Fix direction:** enforce a minimum length (e.g. 8 chars); reject the
  worst common passwords.

### Auto-login rejected with "session expired"
- **Where:** `MasterMain.cpp`, `CreateSessionForUser()` and the
  `gMasterUserTokenLoginPacket` handler.
- **Symptom:** a saved session is refused on the next launch with "Session
  expired or invalid. Please log in.", and the master logs `[MasterServer]
  Session token rejected for: <email>`.
- **Ruled out:** the client side works. `saved_auth.dat` decrypts (the
  message only exists server-side, so the token did reach the master and
  was rejected on its merits), the email is lowercased on both sides before
  `LOWER(U.email) = ?`, and the raw-token/`SHA256`-at-rest split matches
  between `CreateSessionForUser()` and the token-login query. A password
  login also mints a fresh token and overwrites the file, so a stale token
  cannot explain a failure on the launch right after a successful login.
- **Leading suspect:** `CreateSessionForUser()` does not check what
  `sqlite3_step()` returned before returning the raw token. `token_hash` is
  `UNIQUE NOT NULL` and `user_id` carries a foreign key, so a refused
  insert still hands the client a token the `SESSIONS` table never stored —
  which fails on every later auto-login, permanently.
- **Also possible:** `sqlite3_open("users.db")` is relative to the working
  directory, so starting the master from elsewhere silently opens a new
  empty database. Password login failing too would point here rather than
  at the insert.
- **Fix direction:** check the `sqlite3_step()` result in
  `CreateSessionForUser()`, return an empty token when the insert fails so
  the client does not save one, and log `sqlite3_errmsg()`. Resolve the
  database path against a known directory rather than the working one.

## Resolved

Kept for reference so the root cause isn't lost and isn't reintroduced.

### Main thread render stall (voice)
- **Commit:** `363c6ee`
- **Cause:** the voice worker thread held `workmutex` for its entire
  ~1ms loop iteration (incoming processing, encode, mix, cleanup). The
  main thread's `setTransmitting()` / `setLocalMuted()` also locked the
  same mutex just to flip a bool, and blocked until the worker's full
  iteration finished.
- **Fix:** `setTransmitting`/`setLocalMuted` no longer lock at all —
  state is atomic-only now. Worker loop's lock scope narrowed to just
  the processing block, released before `sleep_for`.

### V-key freeze (voice)
- **Commit:** `abe6198`
- **Cause:** redundant mutex locks on Windows keyboard auto-repeat
  events, plus the audio device not being pre-warmed, made
  start/stop-transmission expensive on every repeated key event.
- **Fix:** pre-initialize the audio device at match/session startup,
  debounce start/stop transmission to no-op when state already matches,
  avoid redundant locks on repeat events.

### Chat delivery, team gate, history leaks
- **Commit:** `b34ad56`
- **Cause:** chat messages could fail to reach clients correctly, the
  team-only gate wasn't enforced properly, and message history leaked
  (not cleaned up / scoped correctly).
- **Fix:** corrected delivery path, team gate check, and history
  lifecycle.

### Concurrent TCP punch race
- **Commit:** `0fbd418`
- **Cause:** race condition in znet's concurrent TCP hole-punch path.
- **Fix:** fixed upstream in znet; this repo bumped its pinned znet
  version to pick up the fix.
