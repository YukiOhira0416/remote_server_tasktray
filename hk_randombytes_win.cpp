#include "hk_tweetnacl_sign.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

extern "C" void randombytes(uint8_t* out, uint64_t outlen)
{
    if (!out || outlen == 0) {
        return;
    }

    uint8_t* p = out;
    uint64_t remaining = outlen;

    while (remaining > 0) {
        ULONG chunk = (remaining > 0x7fffffffULL) ? 0x7fffffffUL : (ULONG)remaining;
        NTSTATUS st = BCryptGenRandom(nullptr, p, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (st < 0) {
            SecureZeroMemory(p, (SIZE_T)chunk);
            return;
        }
        p += chunk;
        remaining -= chunk;
    }
}
