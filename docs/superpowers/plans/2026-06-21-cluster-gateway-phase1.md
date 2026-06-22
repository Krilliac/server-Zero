# Cluster Gateway — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the `gateway` binary that authenticates a client connection (vanilla 1.12.1 world auth + `AuthCrypt`) and tunnels its plaintext packets to one backend mangosd node, with a synthetic test client proving it end-to-end without a real WoW client.

**Architecture:** A new `src/gateway` binary (links `shared`, modeled on `src/realmd`) using ACE for sockets, like `WorldSocket`. It terminates the client `AuthCrypt`, validates `CMSG_AUTH_SESSION` against `account.sessionkey`, and forwards plaintext over an internal framed TCP protocol to a mangosd node that runs a new gateway-session intake. Phase 1 uses a single fixed node (no routing/migration yet).

**Tech Stack:** C++ (C++11), ACE (sockets/reactor), `shared` lib (`AuthCrypt`, `WorldPacket`/`ByteBuffer`, `Database`, `Config`, `Log`, `Sha1`, `BigNumber`), MariaDB, CMake/MSVC.

## Global Constraints

- Reuse `src/shared` — do NOT link the game lib into the gateway.
- Vanilla 1.12.1 protocol only; no client-side changes.
- The header cipher (`AuthCrypt`) is a stateful ARC4 stream: 6 crypted bytes client→server, 4 server→client; counters advance per byte. The gateway owns exactly one `AuthCrypt` per client.
- Internal gateway↔node framing reuses `ByteBuffer`; message type is a leading `uint8`.
- Node-side intake must be additive + gated: a mangosd with no gateway configured behaves exactly as today.
- Build: `cmake --build build --config Release --target <tgt> --parallel 2` (bare `--parallel` OOMs this box). New dirs/files need `cmake -S . -B build` reconfigure first (file(GLOB)).
- Auth digest (verbatim algorithm from `WorldSocket::HandleAuthSession`): `SHA1(account_upper + uint32(0) + clientSeed + serverSeed + K)`, where K = `account.sessionkey` (40-byte hex).

---

### Task 1: Gateway binary skeleton (compiles, starts, loads config, logs)

**Files:**
- Create: `src/gateway/CMakeLists.txt`
- Create: `src/gateway/Main.cpp`
- Create: `src/gateway/gateway.conf.dist.in`
- Modify: `src/CMakeLists.txt` (add `add_subdirectory(gateway)` next to `realmd`)

**Interfaces:**
- Produces: a `gateway` executable that reads `gateway.conf`, opens the login + character DBs, logs "Gateway online", and idles until Ctrl-C/stdin EOF.

- [ ] **Step 1: Copy realmd's CMakeLists as the template.** Read `src/realmd/CMakeLists.txt`; create `src/gateway/CMakeLists.txt` identical in structure but target `gateway`, globbing `src/gateway/*.cpp`. Link `shared`, `Threads::Threads`, `OpenSSL::Crypto`, `ace`.

- [ ] **Step 2: Write `Main.cpp`** modeled on `src/realmd/Main.cpp`: parse `-c <conf>`, `sConfig.SetSource`, init `sLog`, open `LoginDatabase` + `CharacterDatabase` from the conf's connection strings, print a banner, then run an idle loop reading stdin (exit on EOF, like mangosd). Reuse realmd's signal handling.

- [ ] **Step 3: Write `gateway.conf.dist.in`** with: `LoginDatabaseInfo`, `CharacterDatabaseInfo`, `GatewayPort` (the client-facing world port clients connect to, default 8085), `Node.1.Host`/`Node.1.GatewayPort` (the single backend node's internal intake address, Phase 1), `LogFile`, `PidFile`.

- [ ] **Step 4: Reconfigure + build.** Run `cmake -S . -B build` then `cmake --build build --config Release --target gateway --parallel 2`. Expected: `gateway.exe` produced, no errors.

- [ ] **Step 5: Smoke run.** `cp build/src/gateway/Release/gateway.exe run/`; `cd run && tail -f /dev/null | ./gateway.exe -c gateway.conf`. Expected log: DB connections established + "Gateway online". Ctrl-C / EOF exits clean.

- [ ] **Step 6: Commit.** `git add src/gateway src/CMakeLists.txt && git commit -m "gateway: binary skeleton (config + DB + idle loop)"`

---

### Task 2: Client accept loop + AuthChallenge

**Files:**
- Create: `src/gateway/ClientSocket.h`, `src/gateway/ClientSocket.cpp`
- Create: `src/gateway/ClientSocketMgr.h`, `src/gateway/ClientSocketMgr.cpp`
- Modify: `src/gateway/Main.cpp` (start the client acceptor on `GatewayPort`)

**Interfaces:**
- Consumes: ACE acceptor pattern from `src/game/Server/WorldSocket.{h,cpp}` + `WorldSocketMgr.{h,cpp}` (copy structure; strip game logic).
- Produces: `ClientSocket` (one per client TCP connection) holding an `AuthCrypt m_crypt`, an `m_seed` (server seed), and a recv assembler; sends `SMSG_AUTH_CHALLENGE` on connect.

- [ ] **Step 1: Copy WorldSocket/WorldSocketMgr structure** into `ClientSocket`/`ClientSocketMgr`, removing `WorldSession`/game references. Keep: ACE_Svc_Handler base, `open()`, `handle_input()` with header+payload assembly, `handle_close()`, `m_crypt` (`AuthCrypt`), `SendPacket(WorldPacket&)`.

- [ ] **Step 2: On `open()`** generate a random 4-byte `m_seed` and send `SMSG_AUTH_CHALLENGE` (opcode `0x1EC`) carrying it — mirror `WorldSocket::open` lines ~279-324. Outgoing header (server→client) is unencrypted until auth completes.

- [ ] **Step 3: Start the acceptor** in `Main.cpp` on `GatewayPort` via `ClientSocketMgr::StartNetwork`, mirroring `WorldSocketMgr::StartNetwork`.

- [ ] **Step 4: Build** (`--target gateway --parallel 2`). Expected: clean.

- [ ] **Step 5: Manual TCP probe.** Boot the gateway; from bash: `printf '' | nc 127.0.0.1 8085 | xxd | head` (or a tiny python socket). Expected: receive the SMSG_AUTH_CHALLENGE bytes (opcode 0x1EC + 4-byte seed). Confirms accept + challenge.

- [ ] **Step 6: Commit.** `git commit -am "gateway: client accept loop + SMSG_AUTH_CHALLENGE"`

---

### Task 3: Auth handshake (validate CMSG_AUTH_SESSION, init AuthCrypt)

**Files:**
- Modify: `src/gateway/ClientSocket.cpp` (add `HandleAuthSession`)
- Create: `src/gateway/GatewayAuth.h`, `src/gateway/GatewayAuth.cpp` (pure auth-digest validation, unit-testable)

**Interfaces:**
- Consumes: `account.sessionkey` lookup (LoginDatabase), `shared` `Sha1Hash`/`BigNumber`.
- Produces: `GatewayAuth::ValidateDigest(account, clientSeed, serverSeed, K, digest) -> bool`; on success `ClientSocket` calls `m_crypt.SetKey(K,40); m_crypt.Init()` and marks the connection authed.

- [ ] **Step 1: Write the failing unit test** `src/gateway/test/test_gateway_auth.cpp`: with a fixed account name, known K (40 bytes), fixed client+server seeds, assert `ValidateDigest` returns true for the matching digest (compute the expected digest in the test the same way) and false for a corrupted digest. (Add a tiny test target in the gateway CMake, or a standalone `gateway_tests` exe.)

- [ ] **Step 2: Run it, expect FAIL** (function undefined).

- [ ] **Step 3: Implement `GatewayAuth::ValidateDigest`** copying the algorithm from `WorldSocket::HandleAuthSession` lines ~907-927: `Sha1Hash sha; sha.UpdateData(account); uint8 t[4]={0}; sha.UpdateData(t,4); sha.UpdateData(&clientSeed,4); sha.UpdateData(&serverSeed,4); sha.UpdateBigNumbers(&K,nullptr); sha.Finalize();` then `memcmp(sha.GetDigest(), digest, 20)==0`.

- [ ] **Step 4: Run the test, expect PASS.**

- [ ] **Step 5: Wire into `ClientSocket::HandleAuthSession`** — parse `CMSG_AUTH_SESSION` (build, account, clientSeed, 20-byte digest), `SELECT id,gmlevel,sessionkey,locale FROM account WHERE username=?`, `K.SetHexStr(sessionkey)`, call `ValidateDigest`; on success `m_crypt.SetKey(K.AsByteArray(),40); m_crypt.Init();`, store `accountId`, and (placeholder for Task 5) log "client <account> authed". On failure send auth-failed + close.

- [ ] **Step 6: Build + run the gateway_tests target.** Expected: tests pass.

- [ ] **Step 7: Commit.** `git commit -am "gateway: world auth handshake + AuthCrypt init (unit-tested digest)"`

---

### Task 4: Synthetic test client (automatable end-to-end driver)

**Files:**
- Create: `src/tools/gwtestclient/CMakeLists.txt`, `src/tools/gwtestclient/Main.cpp` (or a standalone Python script `run/tools/gwtestclient.py` if faster — decide at impl time; a Python script needs no build and can speak the protocol).

**Interfaces:**
- Consumes: the gateway's client port; a known account + session key seeded into `account.sessionkey`.
- Produces: a CLI that connects, reads `SMSG_AUTH_CHALLENGE`, computes the digest from a supplied K, sends `CMSG_AUTH_SESSION`, and reports whether the gateway accepted (and later, round-trips a tunneled packet).

- [ ] **Step 1: Seed a test account's session key.** SQL: `UPDATE account SET sessionkey=<40-byte hex> WHERE username='ADMINISTRATOR'` (a fixed test K). Record it for the client.

- [ ] **Step 2: Write the client** (Python recommended — no build, runs from `run/`): TCP connect to `GatewayPort`; recv challenge, parse seed; build `CMSG_AUTH_SESSION` (build=5875, account, random clientSeed, digest computed via the §Global auth algorithm with the seeded K); send; then read the gateway's response and print PASS/FAIL. Mirror the digest computation exactly (hashlib.sha1 over account + 4 zero bytes + clientSeed + serverSeed + K).

- [ ] **Step 3: Run it against the live gateway.** Expected: gateway log shows "client ADMINISTRATOR authed"; client prints PASS. This is the Phase 1 automated auth test.

- [ ] **Step 4: Commit.** `git commit -am "tools: synthetic gateway test client (auth handshake)"`

---

### Task 5: Internal gateway↔node protocol + gateway-side node link

**Files:**
- Create: `src/gateway/NodeLink.h`, `src/gateway/NodeLink.cpp` (gateway's TCP client to a node's intake port)
- Create: `src/shared/Cluster/GatewayProtocol.h` (shared message enum + framing, usable by both gateway and mangosd)

**Interfaces:**
- Produces: `GatewayProtocol` message ids (`GW_SESSION_OPEN=1`, `GW_CLIENT_PACKET=2`, `GW_SESSION_RELEASE=3`, ... reserve `GW_SESSION_PREPARE/READY/MIGRATE_*` for later phases) + `Frame(ByteBuffer&, uint8 type, ByteBuffer const& payload)` mirroring `ClusterFrame::Build`. `NodeLink::Connect(host,port)`, `NodeLink::SendFrame(...)`, `NodeLink::OnFrame(callback)`.

- [ ] **Step 1: Define `GatewayProtocol.h`** — the message enum + `Frame()`/parse helpers (copy the `[uint32 len][uint8 type][payload]` shape from `src/game/Cluster/ClusterMessage.h`).

- [ ] **Step 2: Implement `NodeLink`** — an ACE connector to the node's gateway-intake port; sends `GW_SESSION_OPEN {clientId, accountId, locale, security}` when a client authenticates, `GW_CLIENT_PACKET {clientId, plaintext}` for each decrypted client packet, and surfaces inbound `GW_CLIENT_PACKET` (node→client) to the owning `ClientSocket` for encryption+send.

- [ ] **Step 3: Wire `ClientSocket`** post-auth: assign a `clientId`, open/attach a `NodeLink` to the single configured node, send `GW_SESSION_OPEN`. For each decrypted client packet, `SendFrame(GW_CLIENT_PACKET,...)`. For inbound node packets, `m_crypt.EncryptSend` the header and write to the client socket.

- [ ] **Step 4: Build.** Expected clean (no node side yet — link will fail to connect; that's fine, log it).

- [ ] **Step 5: Commit.** `git commit -am "gateway: internal node-link protocol + plaintext tunnel (gateway side)"`

---

### Task 6: Node-side gateway-session intake (mangosd)

**Files:**
- Create: `src/game/Server/GatewayIntake.h`, `src/game/Server/GatewayIntake.cpp` (ACE acceptor on a new `Gateway.IntakePort`)
- Create: `src/game/Server/GatewaySessionSocket.h`, `.cpp` (a `WorldSocket`-compatible shim that reads/writes plaintext frames to the gateway instead of an encrypted client socket)
- Modify: `src/game/WorldHandlers/World.cpp` (start the intake when `Gateway.IntakePort` configured), `World.h` (config), `src/mangosd/mangosd.conf.dist.in`

**Interfaces:**
- Consumes: `GatewayProtocol.h`, `WorldSession`, `sWorld.AddSession`.
- Produces: on `GW_SESSION_OPEN`, create a `WorldSession(accountId, gatewaySessionSocket, security, 0, locale)` and `sWorld.AddSession`; route `GW_CLIENT_PACKET` payloads into the session's opcode queue exactly as a normal client packet; the session's outgoing `SendPacket` writes a `GW_CLIENT_PACKET` frame back to the gateway (no `AuthCrypt`).

- [ ] **Step 1: Config + gating.** Add `Gateway.IntakePort` (default 0 = disabled) in `World.h`/`World.cpp`/conf. When 0, nothing starts → single-node behavior unchanged.

- [ ] **Step 2: Implement `GatewaySessionSocket`** — minimal `WorldSocket` work-alike that satisfies what `WorldSession` calls (`SendPacket`, `GetRemoteAddress`, refcount) but serializes outgoing packets as `GW_CLIENT_PACKET` frames to the gateway link; no crypt.

- [ ] **Step 3: Implement `GatewayIntake`** — acceptor; on `GW_SESSION_OPEN` build the session + `AddSession`; on `GW_CLIENT_PACKET` feed the payload into the session like `WorldSocket::ProcessIncoming` does for an authed packet; on `GW_SESSION_RELEASE` drop the session.

- [ ] **Step 4: Build mangosd** (`--target mangosd --parallel 2`). Expected clean; with `Gateway.IntakePort=0` a normal boot is unchanged.

- [ ] **Step 5: Commit.** `git commit -am "mangosd: gateway-session intake (plaintext pre-authed sessions, gated)"`

---

### Task 7: End-to-end Phase 1 (synthetic client → gateway → node → back)

**Files:**
- Modify: test client (Task 4) to send one post-auth opcode (e.g. `CMSG_PING`) and await `SMSG_PONG`.
- Test scaffolding only; no new production files.

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: Configure** node1 `Gateway.IntakePort=9100`; gateway `Node.1` → `127.0.0.1:9100`, `GatewayPort=8085`. Boot node1 then the gateway.

- [ ] **Step 2: Extend the test client** to, after auth, send `CMSG_PING` (opcode `0x1DC`, payload: ping seq + latency) and read until `SMSG_PONG` (`0x1DD`).

- [ ] **Step 3: Run end-to-end.** Expected: gateway log "client authed" + `GW_SESSION_OPEN` to node; node log shows an authed session + receives the ping; the gateway encrypts `SMSG_PONG` back; the test client prints "PONG ok". This proves: client auth at the gateway → plaintext tunnel → node session → reply path through the `AuthCrypt`.

- [ ] **Step 4: Commit.** `git commit -am "gateway: Phase 1 end-to-end ping round-trip via node intake"`

---

## Self-Review

**Spec coverage (Phase 1 scope):** §2 encryption-termination → Tasks 2,3,5,6 (gateway owns AuthCrypt; node gets plaintext). §3 topology gateway binary → Task 1. §4 step 1 auth → Tasks 2,3. §6 internal protocol → Task 5. §7 node-side intake → Task 6. §9 synthetic test client → Tasks 4,7. Phase-1 routing is a single fixed node (no §4 step 3 affinity routing — that's Phase 2, correctly deferred). Migration (§5) deferred to Phase 3/4. Covered.

**Placeholder scan:** message ids beyond `GW_SESSION_OPEN/CLIENT_PACKET/SESSION_RELEASE` are explicitly "reserved for later phases" (not placeholders — out of Phase-1 scope). The Python-vs-C++ test-client choice is a deliberate impl-time decision, not a gap. No vague "add error handling" steps.

**Type consistency:** `GatewayProtocol.h` message enum + `Frame()` used identically by `NodeLink` (Task 5) and `GatewayIntake` (Task 6). `ValidateDigest` signature (Task 3) consumed by `ClientSocket::HandleAuthSession` (Task 3) and mirrored by the test client (Task 4). `clientId` is gateway-assigned (Task 5) and used as the node-side session key (Task 6). Consistent.
