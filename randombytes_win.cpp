#include "tweetnacl_sign.h"

#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

extern "C" void randombytes(uint8_t* out, uint64_t outlen)
{
    if (!out || outlen == 0) {
        return;
    }

    // BCryptGenRandom supports at most ULONG length per call.
    uint8_t* p = out;
    uint64_t remaining = outlen;

    while (remaining > 0) {
        ULONG chunk = (remaining > (uint64_t)0x7fffffffULL) ? 0x7fffffffUL : (ULONG)remaining;
        NTSTATUS status = BCryptGenRandom(NULL, p, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
            // Best effort: zero-fill remaining bytes on failure.
            SecureZeroMemory(p, (SIZE_T)chunk);
            return;
        }
        p += chunk;
        remaining -= chunk;
    }
}
