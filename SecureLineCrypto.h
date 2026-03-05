#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <winsock2.h>

// SecureLineCrypto
// - Performs a simple line-based ECDH (P-256) handshake over an already-connected TCP socket.
// - After handshake, application lines are sent/received as:
//     SEC1 <base64(nonce12|tag16|cipherBytes)>
//   where the plaintext is the original command line WITHOUT the trailing '\n'.
//
// Handshake messages (plaintext):
//   Client -> Server: HELLO1 <b64(clientPubBlob)> <b64(clientNonce12)>
//   Server -> Client: WELCOME1 <b64(serverPubBlob)> <b64(serverNonce12)>
//
// Session key derivation (32 bytes):
//   SHA256("HK-SESS1" || raw_secret || clientNonce12 || serverNonce12)
//
// This implementation uses a machine-scoped keypair stored at:
//   %ProgramData%\HayateKomorebi\DeviceKeys\server_ecdh_p256.key
// The private key blob is protected via DPAPI (CRYPTPROTECT_LOCAL_MACHINE).

namespace hk_secureline
{
    struct Session
    {
        bool active = false;
        std::array<uint8_t, 32> key{};
    };

    // Perform server-side handshake on an accepted TCP socket.
    // - recvBuffer is used to carry any already-received bytes into the handshake parser.
    // - On success, outSession.active becomes true.
    bool ServerHandshake(SOCKET sock, std::string& recvBuffer, Session& outSession, int timeoutMs);

    // Encrypt a plaintext line (no trailing '\n') into a SEC1 line (with trailing '\n').
    bool EncryptLineToSec1(const Session& session, const std::string& plainLine, std::string& outSecLine);

    // Decrypt a SEC1 line (no trailing '\n') into a plaintext line.
    bool DecryptSec1ToLine(const Session& session, const std::string& secLine, std::string& outPlainLine);

    // Utility: best-effort close of an active session.
    void Clear(Session& session);
}
