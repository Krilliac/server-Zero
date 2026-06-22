#!/usr/bin/env python3
"""
Synthetic gateway auth test client.

Authenticates through the cluster gateway end-to-end WITHOUT a WoW client,
proving the auth handshake (challenge -> session -> response) works using
INDEPENDENT crypto: Python's hashlib for the SHA1 digest and a hand-ported
copy of the server's AuthCrypt header cipher. If the digest order, seed
endianness, or header cipher were wrong, this would not line up with the
server's Sha1/AuthCrypt and the test would fail.

It seeds a known 40-byte session key for account ADMINISTRATOR (so we don't
depend on a real SRP login), connects to 127.0.0.1:8085, completes the
handshake, and decrypts the encrypted SMSG_AUTH_RESPONSE to confirm AUTH_OK.

Prints "AUTH PASS" / "AUTH FAIL: <reason>" and exits 0/1 accordingly.

Usage:
    python tools/gwtestclient.py [host] [port]
"""

import hashlib
import os
import socket
import struct
import subprocess
import sys

# --- Configuration ---------------------------------------------------------

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8085

# Optional: a character guid to enter-world with after char-enum. When given
# (3rd positional arg or LOGIN_GUID env), the client sends CMSG_PLAYER_LOGIN so
# the gateway resolves the character's owning node and re-homes the session if
# needed. This drives the Phase 2 end-to-end affinity-routing proof.
def _login_guid():
    if len(sys.argv) > 3:
        return int(sys.argv[3])
    env = os.environ.get("LOGIN_GUID")
    return int(env) if env else None


ACCOUNT = "ADMINISTRATOR"
BUILD = 5875

# Fixed, known 40-byte session key (80 hex chars). Seeded into the DB below so
# the digest the server recomputes matches the one we send.
#
# The leading byte is intentionally non-zero (0xFF). The server round-trips K
# through a BigNumber (SetHexStr -> AsByteArray); a leading 0x00 byte would be
# dropped as an insignificant high byte, shrinking K to 39 bytes for the digest
# and breaking interop. A non-zero MSB keeps it a clean 40 bytes.
K_HEX = ("ff112233445566778899aabbccddeeff"
         "00112233445566778899aabbccddeeff"
         "0011223344556677")
assert len(K_HEX) == 80, "K must be 40 bytes (80 hex chars)"
# IMPORTANT byte-order note (the interop gotcha):
# The server stores K via BigNumber::SetHexStr (BN_hex2bn = big-endian hex)
# and serializes it with AsByteArray(), which does BN_bn2bin (big-endian) then
# std::reverse -> LITTLE-endian byte order. Both the SHA1 digest
# (UpdateBigNumbers -> AsByteArray) and the AuthCrypt key (SetKey(AsByteArray))
# therefore use K in little-endian. So we reverse the hex bytes here.
K_BYTES = bytes.fromhex(K_HEX)[::-1]

# Native Windows path (this script runs under the Windows Python interpreter,
# which cannot resolve the Git-Bash-style "/c/Program Files/..." path).
MYSQL = r"C:\Program Files\MariaDB 10.5\bin\mysql.exe"

# Opcodes (mirror the gateway's locally-defined constants).
SMSG_AUTH_CHALLENGE = 0x1EC
CMSG_AUTH_SESSION = 0x1ED
SMSG_AUTH_RESPONSE = 0x1EE
AUTH_OK = 0x0C

# Round-trip opcodes (session-handled, so they traverse the full tunnel).
CMSG_CHAR_ENUM = 0x37
SMSG_CHAR_ENUM = 0x3B

# Enter-world: the gateway intercepts this to resolve character->node affinity.
CMSG_PLAYER_LOGIN = 0x3D

# The decisive proof opcode. SMSG_LOGIN_VERIFY_WORLD (0x236) is sent by the node
# when a player enters the world -- once at the initial login, and AGAIN on the
# transparent migration arrival (node B's loading-screen resume, commit 3b4ece5b).
# Observing it a SECOND time on the SAME, never-reconnected connection is the
# whole point of the gateway: transparent transfer.
SMSG_LOGIN_VERIFY_WORLD = 0x236


# --- AuthCrypt port (classic: key = raw 40-byte K) -------------------------

class HeaderCrypt:
    """Hand-port of src/shared/Auth/AuthCrypt.cpp (classic path).

    Counters start at 0 the moment the crypt is keyed (right after auth).
    Only the first 4 bytes of each server->client header are encrypted, so we
    decrypt exactly 4 here even though the full server header is 4 bytes.
    """

    def __init__(self, key: bytes):
        self.key = key
        self.recv_i = 0
        self.recv_j = 0
        # Send counters are an independent stream (mirror the server's recv
        # counters): the client's EncryptSend is the inverse of the server's
        # DecryptRecv, so the two stay in lockstep packet-for-packet.
        self.send_i = 0
        self.send_j = 0

    def encrypt_send(self, data: bytes) -> bytes:
        # Inverse of the server's DecryptRecv:
        #   server: p = (cipher - prev_j) ^ key[i]; prev_j = cipher
        #   client: cipher = ((p ^ key[i]) + prev_j) & 0xFF; prev_j = cipher
        # This is identical in form to the server's EncryptSend, but the server
        # decrypts the FIRST 6 bytes of each client header (CRYPTED_RECV_LEN),
        # so we encrypt exactly the 6-byte client header here.
        out = bytearray(len(data))
        for t in range(len(data)):
            self.send_i %= len(self.key)
            x = ((data[t] ^ self.key[self.send_i]) + self.send_j) & 0xFF
            self.send_i += 1
            self.send_j = x
            out[t] = x
        return bytes(out)

    def decrypt_recv(self, data: bytes) -> bytes:
        # Inverse of the server's EncryptSend:
        #   server: x = (data[t] ^ key[i]) + j; j = x
        #   client: x = ((cipher - j) & 0xFF) ^ key[i]; j = cipher
        out = bytearray(len(data))
        for t in range(len(data)):
            self.recv_i %= len(self.key)
            cipher = data[t]
            x = ((cipher - self.recv_j) & 0xFF) ^ self.key[self.recv_i]
            self.recv_i += 1
            self.recv_j = cipher
            out[t] = x
        return bytes(out)


# --- Helpers ---------------------------------------------------------------

def seed_session_key():
    """Seed the fixed session key into realmd.account for ADMINISTRATOR."""
    sql = ("UPDATE account SET sessionkey='%s' WHERE username='%s'"
           % (K_HEX.upper(), ACCOUNT))
    try:
        subprocess.run([MYSQL, "-uroot", "-proot", "realmd", "-e", sql],
                       check=True, capture_output=True, text=True)
    except FileNotFoundError:
        # MySQL client not at the expected path; assume the key was seeded
        # manually. Print the SQL so the caller can run it.
        print("NOTE: mysql client not found; run this manually:")
        print("  mysql -uroot -proot realmd -e \"%s\"" % sql)
    except subprocess.CalledProcessError as e:
        raise RuntimeError("seeding session key failed: %s" % e.stderr)


def recv_exact(sock: socket.socket, n: int) -> bytes:
    """Read exactly n bytes or raise."""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise RuntimeError("connection closed (wanted %d, got %d)"
                               % (n, len(buf)))
        buf += chunk
    return buf


def main():
    seed_session_key()

    sock = socket.create_connection((HOST, PORT), timeout=5)
    sock.settimeout(5)

    # --- 1. Read SMSG_AUTH_CHALLENGE (plaintext) ---
    # Server header: size (2 bytes BIG-endian) + opcode (2 bytes LITTLE-endian).
    hdr = recv_exact(sock, 4)
    size = struct.unpack(">H", hdr[0:2])[0]   # big-endian
    opcode = struct.unpack("<H", hdr[2:4])[0]  # little-endian
    if opcode != SMSG_AUTH_CHALLENGE:
        print("AUTH FAIL: expected SMSG_AUTH_CHALLENGE (0x%X), got 0x%X"
              % (SMSG_AUTH_CHALLENGE, opcode))
        return 1
    # size counts the opcode field (2 bytes), so body = size - 2.
    body = recv_exact(sock, size - 2)
    if len(body) < 4:
        print("AUTH FAIL: challenge body too short (%d bytes)" % len(body))
        return 1
    server_seed = struct.unpack("<I", body[0:4])[0]
    print("challenge ok: serverSeed=0x%08X" % server_seed)

    # --- 2. Build CMSG_AUTH_SESSION digest (independent SHA1) ---
    client_seed = struct.unpack("<I", os.urandom(4))[0]
    account_bytes = ACCOUNT.encode("ascii")  # WITHOUT null terminator

    # Field order MUST match src/gateway/GatewayAuth.cpp:
    #   account || 0x00000000 || clientSeed(LE) || serverSeed(LE) || K
    sha = hashlib.sha1()
    sha.update(account_bytes)
    sha.update(b"\x00\x00\x00\x00")
    sha.update(struct.pack("<I", client_seed))
    sha.update(struct.pack("<I", server_seed))
    sha.update(K_BYTES)
    digest = sha.digest()
    assert len(digest) == 20

    # --- 3. Build and send the auth packet (plaintext header) ---
    # Payload: build:uint32, unk:uint32, account cstring (null-term),
    #          clientSeed:uint32, digest:20 bytes.
    payload = b""
    payload += struct.pack("<I", BUILD)
    payload += struct.pack("<I", 0)
    payload += account_bytes + b"\x00"
    payload += struct.pack("<I", client_seed)
    payload += digest

    # Client header (6 bytes, plaintext pre-crypt):
    #   size (2 bytes BIG-endian) = len(4-byte opcode + payload)
    #   opcode (4 bytes LITTLE-endian)
    cmd_field_len = 4 + len(payload)
    client_hdr = struct.pack(">H", cmd_field_len) + struct.pack("<I", CMSG_AUTH_SESSION)
    sock.sendall(client_hdr + payload)

    # --- 4. Read encrypted SMSG_AUTH_RESPONSE ---
    # After we sent auth, the server keyed AuthCrypt with the raw 40-byte K and
    # encrypts the 4-byte server header. Port the cipher and decrypt it.
    crypt = HeaderCrypt(K_BYTES)
    enc_hdr = recv_exact(sock, 4)
    dec_hdr = crypt.decrypt_recv(enc_hdr)
    if os.environ.get("GW_DEBUG"):
        print("DEBUG: authresp hdr enc=%s dec=%s recv_i=%d recv_j=%d"
              % (enc_hdr.hex(), dec_hdr.hex(), crypt.recv_i, crypt.recv_j))
    size = struct.unpack(">H", dec_hdr[0:2])[0]
    opcode = struct.unpack("<H", dec_hdr[2:4])[0]
    if opcode != SMSG_AUTH_RESPONSE:
        print("AUTH FAIL: expected SMSG_AUTH_RESPONSE (0x%X) after decrypt, got 0x%X"
              % (SMSG_AUTH_RESPONSE, opcode))
        return 1
    body = recv_exact(sock, size - 2)
    if len(body) < 1 or body[0] != AUTH_OK:
        got = body[0] if body else None
        print("AUTH FAIL: response body not AUTH_OK (0x%X), got %r"
              % (AUTH_OK, got))
        return 1

    print("AUTH PASS")

    # --- 5. Round-trip a session-handled opcode through the tunnel ---------
    # CMSG_CHAR_ENUM is handled by WorldSession::HandleCharEnumOpcode (an async
    # DB query), so it travels the FULL path: gateway -> node session ->
    # opcode handler -> SMSG_CHAR_ENUM -> gateway -> us. CMSG_PING would not
    # (it's answered socket-side). An empty payload is correct for char-enum.
    #
    # Post-auth, the client header is the 6-byte ClientPktHeader the gateway
    # decrypts (size BE 2 bytes + opcode 4 bytes LE). We encrypt those 6 bytes
    # with the same keyed crypt; send counters start fresh (the auth packet's
    # header went out in plaintext, so this is the first encrypted client
    # header and the server's recv counters are likewise at zero).
    enum_payload = b""  # CMSG_CHAR_ENUM has no body
    enum_cmd_len = 4 + len(enum_payload)  # size counts the 4-byte opcode field
    enum_hdr_plain = (struct.pack(">H", enum_cmd_len)
                      + struct.pack("<I", CMSG_CHAR_ENUM))
    enum_hdr_enc = crypt.encrypt_send(enum_hdr_plain)
    if os.environ.get("GW_DEBUG"):
        print("DEBUG: enum hdr plain=%s enc=%s"
              % (enum_hdr_plain.hex(), enum_hdr_enc.hex()))
    sock.sendall(enum_hdr_enc + enum_payload)

    # Read server packets until SMSG_CHAR_ENUM arrives, skipping the node's
    # world-admission SMSG_AUTH_RESPONSE. When the gateway opens the backend
    # session, World::AddSession sends a SECOND SMSG_AUTH_RESPONSE (the
    # AUTH_OK + billing world-admission packet, body=10 bytes) back through the
    # tunnel ahead of our char-enum reply. That's expected and is consumed
    # here. The node also runs an async character DB query, so the char-enum
    # reply is delayed; loop with a recv timeout to absorb both.
    sock.settimeout(5)
    deadline = __import__("time").time() + 10
    while True:
        if __import__("time").time() > deadline:
            print("ROUNDTRIP FAIL: no SMSG_CHAR_ENUM within deadline")
            return 1
        try:
            enc_reply_hdr = recv_exact(sock, 4)
        except (socket.timeout, RuntimeError) as e:
            print("ROUNDTRIP FAIL: no server reply (%s)" % e)
            return 1

        dec_reply_hdr = crypt.decrypt_recv(enc_reply_hdr)
        rsize = struct.unpack(">H", dec_reply_hdr[0:2])[0]
        ropcode = struct.unpack("<H", dec_reply_hdr[2:4])[0]
        rbody = recv_exact(sock, rsize - 2) if rsize >= 2 else b""
        if os.environ.get("GW_DEBUG"):
            print("DEBUG: reply hdr enc=%s dec=%s opcode=0x%X size=%d"
                  % (enc_reply_hdr.hex(), dec_reply_hdr.hex(), ropcode, rsize))

        if ropcode == SMSG_AUTH_RESPONSE:
            # World-admission packet from the node; expected, keep reading.
            continue
        if ropcode == SMSG_CHAR_ENUM:
            char_count = rbody[0] if rbody else 0
            print("ROUNDTRIP PASS (chars=%d)" % char_count)
            break
        print("ROUNDTRIP FAIL: unexpected opcode 0x%X (size=%d) before "
              "SMSG_CHAR_ENUM" % (ropcode, rsize))
        return 1

    # --- 6. Optional: enter world with CMSG_PLAYER_LOGIN -------------------
    # This is the Phase 2 affinity-routing trigger. The gateway peeks the guid,
    # asks the registry which node owns it (cluster_character_node), and re-homes
    # the player-less session to that node before forwarding the login. The
    # in-world packet flood that follows is not fully processed here -- we just
    # send the login and drain a few replies so the node/gateway logs can show
    # where the character landed.
    guid = _login_guid()
    if guid is None:
        sock.close()
        return 0

    # Payload: a single little-endian uint64 player guid.
    login_payload = struct.pack("<Q", guid)
    login_cmd_len = 4 + len(login_payload)  # size counts the 4-byte opcode field
    login_hdr_plain = (struct.pack(">H", login_cmd_len)
                       + struct.pack("<I", CMSG_PLAYER_LOGIN))
    login_hdr_enc = crypt.encrypt_send(login_hdr_plain)
    if os.environ.get("GW_DEBUG"):
        print("DEBUG: login guid=%d hdr plain=%s enc=%s payload=%s"
              % (guid, login_hdr_plain.hex(), login_hdr_enc.hex(),
                 login_payload.hex()))
    sock.sendall(login_hdr_enc + login_payload)
    print("PLAYER_LOGIN sent (guid=%d)" % guid)

    # --- 7. CONTINUOUS READER (the migration proof loop) -------------------
    # This is the heart of the Phase 3 mechanical proof. After CMSG_PLAYER_LOGIN
    # the node floods us with the in-world login packets (spells, items, auras,
    # initial world state, etc.). The header AuthCrypt is STATEFUL -- its recv_i
    # / recv_j counters advance one step per encrypted header byte -- so we MUST
    # decrypt EVERY server->client 4-byte header IN ORDER, then consume the exact
    # decrypted payload length, or the cipher desyncs and every subsequent header
    # decodes to garbage. We therefore never skip a packet: read header (4 bytes)
    # -> decrypt -> read (size-2) payload -> log -> repeat, forever, with a short
    # socket timeout so the loop keeps spinning (and the connection stays OPEN)
    # while the operator triggers the migration over SOAP on another channel.
    #
    # We do NOT reconnect, re-auth, or re-send char-select. The SAME socket that
    # logged in on node A stays open the whole time; when node B resumes us it
    # sends a SECOND SMSG_LOGIN_VERIFY_WORLD down this very stream. Catching that
    # 0x236 a second time -- crypt still in sync -- is transparent transfer.
    import time as _time

    def _ts():
        return _time.strftime("%H:%M:%S")

    verify_world_count = 0
    pkt_count = 0
    # Run until an overall wall-clock budget elapses (the test harness triggers
    # the migration well within this) or, if RUN_SECONDS is given, that long.
    run_seconds = float(os.environ.get("RUN_SECONDS", "120"))
    started = _time.time()
    sock.settimeout(2)

    print("[%s] CONTINUOUS READER START (staying crypt-synced through the login "
          "flood; connection held open for migration)" % _ts())
    sys.stdout.flush()

    while _time.time() - started < run_seconds:
        try:
            enc_hdr = recv_exact(sock, 4)
        except socket.timeout:
            # No data this interval -- normal between the login flood and the
            # migration. Keep the connection open and keep waiting.
            continue
        except RuntimeError as e:
            print("[%s] READER: connection closed by server (%s) after %d packet(s), "
                  "%d LOGIN_VERIFY_WORLD seen" % (_ts(), e, pkt_count, verify_world_count))
            break

        dh = crypt.decrypt_recv(enc_hdr)
        rsize = struct.unpack(">H", dh[0:2])[0]
        ropcode = struct.unpack("<H", dh[2:4])[0]
        # Read the rest of the payload by the DECRYPTED size (size counts the
        # 2-byte opcode field, so body = size - 2). A wrong size here is the
        # tell-tale of a crypt desync; guard against an absurd length.
        if rsize < 2 or rsize > 0x20000:
            print("[%s] READER DESYNC? implausible size=%d opcode=0x%X (enc_hdr=%s "
                  "dec_hdr=%s recv_i=%d) -- crypt likely lost sync; aborting reader"
                  % (_ts(), rsize, ropcode, enc_hdr.hex(), dh.hex(), crypt.recv_i))
            sock.close()
            return 1
        try:
            _body = recv_exact(sock, rsize - 2) if rsize > 2 else b""
        except (socket.timeout, RuntimeError) as e:
            print("[%s] READER: failed reading body for opcode 0x%X size=%d (%s)"
                  % (_ts(), ropcode, rsize, e))
            break

        pkt_count += 1
        if os.environ.get("GW_DEBUG"):
            print("[%s] pkt opcode=0x%04X size=%d" % (_ts(), ropcode, rsize))
            sys.stdout.flush()

        if ropcode == SMSG_LOGIN_VERIFY_WORLD:
            verify_world_count += 1
            mapid = struct.unpack("<I", _body[0:4])[0] if len(_body) >= 4 else -1
            if verify_world_count == 1:
                print("[%s] *** SMSG_LOGIN_VERIFY_WORLD #1 (INITIAL LOGIN on node A) "
                      "map=%d *** -- in world; now holding the connection open, "
                      "waiting for the migration resume." % (_ts(), mapid))
            else:
                print("[%s] *** SMSG_LOGIN_VERIFY_WORLD #%d (MIGRATION RESUME) map=%d "
                      "*** <<< TRANSPARENT TRANSFER PROVEN: the SAME connection "
                      "received the world-enter again with NO reconnect / re-auth / "
                      "char-select. >>>" % (_ts(), verify_world_count, mapid))
            sys.stdout.flush()

    print("[%s] CONTINUOUS READER END: %d packet(s) read, "
          "%d SMSG_LOGIN_VERIFY_WORLD received." % (_ts(), pkt_count, verify_world_count))
    sys.stdout.flush()
    try:
        sock.close()
    except Exception:
        pass
    # Exit 0 only if we saw the resume (>=2). Exit 2 = logged in but no migration
    # observed within the budget (still useful: distinguishes "never migrated"
    # from a crypt desync, which exits 1 above).
    return 0 if verify_world_count >= 2 else 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print("AUTH FAIL: %s" % e)
        sys.exit(1)
