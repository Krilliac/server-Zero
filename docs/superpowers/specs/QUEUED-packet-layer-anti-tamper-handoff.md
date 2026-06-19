# QUEUED (not now) — Packet-Layer Anti-Tamper & Obfuscation

Status: **deferred** per user (2026-06-19). This is the "opcode mapping" capstone —
do it LATER, after the current commands/anti-cheat work. Captured verbatim so it's
not lost.

## Prime directive
Reconstruct "WPE sees garbage + single-byte tamper = instant kick" **entirely
server-side, unmodified retail 1.12.1 client.** No client mod ever. Defensive only.

## Verified vanilla protocol facts (do NOT "upgrade")
- Cipher `src/shared/Auth/AuthCrypt` is an **additive rolling cipher (NOT RC4)**.
  `EncryptSend` enciphers first `CRYPTED_SEND_LEN=4` bytes (SMSG hdr: u16 size + u16 opcode);
  `DecryptRecv` deciphers first `CRYPTED_RECV_LEN=6` (CMSG hdr: u16 size + u32 opcode).
  Header only; bodies plaintext. Stateful ciphertext feedback (_send_j/_recv_j +
  _send_i/_recv_i) → one tampered header byte cascades (the mechanism to lean on).
- Session key = SRP6 K (server already holds it; never transmitted).
- Client-native compressed opcodes: `SMSG_COMPRESSED_UPDATE_OBJECT=0x1F6`,
  `SMSG_COMPRESSED_MOVES=0x2FB`. NO generic bundling opcode (WotLK only) — out of scope.
- zlib already linked/used in `src/game/WorldHandlers/UpdateData.cpp` — reuse it.
- Socket layer is ACE (`ACE_Svc_Handler`); socket I/O on the network reactor thread,
  NOT the map thread.

## Phases
0. **Audit** AuthCrypt.{h,cpp}; WorldSocket.{h,cpp} (inbound decrypt+`ClientPktHeader`
   cast ~L474-478, opcode guard `if (opcode >= NUM_MSG_TYPES)` ~L648-650 = hook point;
   outbound `ServerPktHeader`+`EncryptSend` ~L1048-1068 = compression hook); UpdateData.cpp
   compression threshold; Opcodes.{h,cpp} (`NUM_MSG_TYPES`, the two compressed opcodes,
   PROCESS_* threading). Output audit summary BEFORE coding.
1. **Outbound obfuscation (compression-only, object-update + movement ONLY):**
   policy-driven object-update compression threshold (down to always); route batched
   movement via `SMSG_COMPRESSED_MOVES` (u32 decompressed-size prefix + deflate, mirror
   the compressed-update wire format); reuse the zlib path; configurable level; CPU
   guardrail (size-gated, skippable). No opcode remap / body XOR (would need client mod).
2. **Inbound tamper detection:** extend the existing opcode-range guard with a decrypted-
   header sanity battery (opcode < NUM_MSG_TYPES + has handler; per-opcode size table
   exact/cap; absolute cap). Structured `PacketTamper` event {opcode, size, reason}.
   Desync hysteresis (sliding window; lone anomaly = clean disconnect w/o penalty,
   repeated = KICK/BAN; config-driven). Confirmed desync ⇒ close cleanly (no resync path).
3. **Body validation hook:** per-opcode body-sanity interface after header validation,
   before dispatch; WIRE TO EXISTING validators (movement/spell/interaction/inventory) —
   integration not rewrite. Soft by default; promote high-confidence checks individually.
4. **Action policy:** reuse `warden_action` LOG/KICK/BAN convention +
   `CONFIG_UINT32_PACKET_TAMPER_ACTION`-style config. High-confidence vs soft tiers.
   **LOG-first rollout mandatory.** All thresholds config-driven.

## Constraints
ACE threading (reactor vs map thread; body validators marshal/snapshot). MSVC+GCC clean.
Respect `#pragma pack` on the headers; no UB in the cast. Reuse zlib/guard/action convention.
No stubs — per-opcode size table fully populated. Little-endian wire.

## Deliverables
Audit summary; Phase1 compression (stock client plays normally, capture shows blobs);
Phase2 strict header validation + PacketTamper + clean desync close; Phase3 body hook;
Phase4 tiered policy LOG-first; `PACKET_HARDENING.md` with the honest ceiling (compression
≠ encryption; defeats Winsock editors not memory readers; pair with authoritative validators).

Commit sequence: phase0 → phase1-compress → phase2-tamper → phase3-body → phase4-policy.
