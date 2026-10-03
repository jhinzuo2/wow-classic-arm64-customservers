/*
 * PlayBeta.exe - native port of Jugar-Beta-Attach.bat (no PowerShell / .NET needed).
 *
 * Put it INSIDE the _classic_beta_ folder (next to WowB.exe) and run it.
 *
 *  1. Makes sure WTF\BetaSuspendedTest.wtf (and WTF\Config.wtf) contain the server portal.
 *  2. Waits for WowB.exe to be started by something else (attach-only, default) or, with --launch,
 *     starts it itself.
 *  3. Scans the client's heap for the 12-key store (anchored on key #1). When found, replaces the
 *     32-byte key of group 8 with the server key. Only writes to MEM_PRIVATE + PAGE_READWRITE.
 *  4. Keeps watching and re-applies if the client recreates the store. Exits when the game closes.
 *
 * Default: starts WowB.exe itself (-config BetaSuspendedTest.wtf) if it is not already running.
 * Everything is logged to log.txt next to this exe. While no key store is found, a background
 * thread also runs a full-memory DIAGNOSTIC pass every 30 s (max 20 passes) and logs the result.
 *
 * Options:
 *   --arm64         launch WowB-ARM64.exe instead of WowB.exe (used automatically if it is the only one;
 *                   either name is recognized when attaching to an already running game)
 *   --attach        do NOT start the game; wait for something else to start WowB.exe / WowB-ARM64.exe
 *   --wait N        minutes to wait for WowB.exe in --attach mode (default 15)
 *   --delay N       seconds to wait after attaching before any scanning starts (default 45)
 *   --maxregion MB  skip single memory regions bigger than this in FULL passes (default 1024)
 *   --fast MB       fast passes skip regions bigger than this (default 128, 0 = every pass is full);
 *                   every 20th pass is a full pass
 *   --selftest      run the emulation self-test (fake game process) and exit
 *   --nodiag        disable the automatic diagnostic passes
 *   --no-default    do not touch WTF\Config.wtf (only BetaSuspendedTest.wtf)
 *   --dryrun        read-only, single pass, writes nothing
 *   --dir "path"    game folder (only needed if auto-detection fails)
 *   --pid N         attach to this PID instead of looking for WowB.exe (testing)
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define PORTAL        "auth.gpon.com.co"
#define CONFIG_NAME   "BetaSuspendedTest.wtf"
#define ENTRY_SIZE    40
#define ENTRY_COUNT   12
#define BLOCK_SIZE    (ENTRY_SIZE * ENTRY_COUNT)
#define TARGET_GROUP  8
#define CHUNK         (4u * 1024u * 1024u)
#define MAX_HITS      256

static const char *KEY_HEX[ENTRY_COUNT] = {
    "9B0671C815DFF513BFD4A2B26AE1F84EC9106841B2FB620DB65F6ADE7C21AD06", /* 1 anchor (never touched) */
    "112451B9843C2799E4750AA3D9B9AFABF536A645B863C4ADB2078B932B354E04",
    "A6B858485748BF38BE193517AB90F8DE169EFF0989EA9360DB346A378B0FFE15",
    "50FFDEB807F8FF73A299A16000AA6575C5945BE875ADFC8795374ED3415249AC",
    "9F842D078755647C008E5FE3E12B839A981DE02A1F520CB2545CC04CD2323E0E",
    "B8CC75864D8EF461D8DD6AA7425E2C63C24D3982066B8D773A1549BFE24E05EE",
    "76BB7BBDD9F34E124A4573C3AA227E3C87CC603506697D054F6FDEC342E56EBF",
    "1FD6DD8FA0EC30D39E3F72E755B8A045BDE0F70449DC71008B767C2EAA89FB9F", /* 8 <- replaced */
    "15D618BD7DB577BD9A8D45769C59E4FC631633BF447398A4B489B4C26FBC03AD", /* 9 flag=0, DO NOT TOUCH */
    "B34EC592D2AC4993F5FFC85B15C3DA9078694051CB224345592AF7136D796C99",
    "9E91586DD4B115AB0568B21959E181F6545910BC5E2F30B8975CA09D7BC3EFCE",
    "769C824A1DADD19AEEFA420414D1732DAF44537F98AA229FC5477BCA55F9D59A"
};
static const char *NEW_KEY_HEX = "02596F0D0C061A8B30745988FD72C59E29EC367FB0F341F28E0F08D037BAFC69";
static const BYTE EXPECTED_FLAGS[ENTRY_COUNT] = { 1,1,1,1,1,1,1,1,0,1,1,1 };
static const BYTE ENTRY_TAIL[3] = { 0x7F, 0x00, 0x00 };  /* only used by the self-test fake game; padding is not validated */

static BYTE g_keys[ENTRY_COUNT][32];
static BYTE g_newKey[32];

static char g_gameDir[MAX_PATH];
static char g_exe[MAX_PATH];
static const char *g_exeName = "WowB.exe";   /* file name that gets launched (WowB.exe or WowB-ARM64.exe) */
static int g_preferArm64 = 0;                /* --arm64: launch WowB-ARM64.exe even if WowB.exe exists */
#define EXE_X64   "WowB.exe"
#define EXE_ARM64     "WowB-ARM64.exe"
#define EXE_ARM64_ALT "WowB_arm64.exe"   /* underscore spelling, also accepted */
static int is_client_name(const char *n) {
    return _stricmp(n, EXE_X64) == 0 || _stricmp(n, EXE_ARM64) == 0 || _stricmp(n, EXE_ARM64_ALT) == 0;
}
static char g_logFile[MAX_PATH];
static int  g_dryRun = 0;
static int  g_launch = 1;      /* default: start the game ourselves */
static int  g_diagEnabled = 1;
static ULONGLONG g_maxAddr = 0x7FFFFFFF0000ULL;   /* replaced by the real limit from GetSystemInfo */
static int  g_selftest = 0;
static char g_childFile[MAX_PATH];
static ULONGLONG g_maxRegion = 1024ULL * 1024ULL * 1024ULL;  /* skip single regions bigger than this (--maxregion MB) */
static ULONGLONG g_fastRegion = 128ULL * 1024ULL * 1024ULL;  /* fast passes skip regions bigger than this (--fast MB, 0 = off) */
static int g_fastEvery = 20;                                 /* every Nth pass is a full pass (uses g_maxRegion) */
static ULONGLONG g_curMax = 1024ULL * 1024ULL * 1024ULL;     /* region limit used by the pass that is running now */
static volatile ULONGLONG g_lastScanMs = 0;                  /* duration of the last completed pass */
static volatile LONG g_scanPasses = 0;                       /* number of completed passes */
static int  g_startDelay = 45;  /* emulated boot is slow: do not scan a half-started game */
static CRITICAL_SECTION g_cs;
static volatile LONG g_storeHeld = 0;
static volatile LONG g_stop = 0;
static int  g_waitMin = 15;
static int  g_patchDefault = 1;
static DWORD g_forcePid = 0;
static char g_lastScan[256];

/* ---------------------------------------------------------------- console / log */
enum { C_GRAY = 7, C_DGRAY = 8, C_GREEN = 10, C_CYAN = 11, C_RED = 12, C_YELLOW = 14, C_WHITE = 15 };

static void logf_(const char *fmt, ...) {
    char msg[1024]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    EnterCriticalSection(&g_cs);
    FILE *f = fopen(g_logFile, "ab");
    if (f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, msg);
        fclose(f);
    }
    LeaveCriticalSection(&g_cs);
}
static void say(int color, const char *fmt, ...) {
    char msg[1024]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
    SYSTEMTIME t; GetLocalTime(&t);
    EnterCriticalSection(&g_cs);
    SetConsoleTextAttribute(o, (WORD)color);
    printf("[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, msg);
    fflush(stdout);
    SetConsoleTextAttribute(o, C_GRAY);
    LeaveCriticalSection(&g_cs);
    logf_("%s", msg);
}
static void finish(int code) {
    if (code != 0) {
        printf("\nPress Enter to close...");
        fflush(stdout);
        getchar();
    }
    exit(code);
}
static void fail(const char *fmt, ...) {
    char msg[1024]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    say(C_RED, "%s", msg);
    finish(1);
}

/* ---------------------------------------------------------------- helpers */
static int nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
static void hex2bin(const char *h, BYTE *out, int n) {
    for (int i = 0; i < n; i++) out[i] = (BYTE)((nib(h[2 * i]) << 4) | nib(h[2 * i + 1]));
}
static int is_heap(const MEMORY_BASIC_INFORMATION *m) {
    return m->State == MEM_COMMIT && m->Type == MEM_PRIVATE && m->Protect == PAGE_READWRITE;
}

static const char *type_name(DWORD t);
static void prot_name(DWORD p, char *o, size_t n);

/* ---------------------------------------------------------------- memory scan */
/* Search for 'needle' (32 bytes) at 4-byte aligned positions in committed private RW memory. */
static int scan_heap(DWORD pid, const BYTE *needle, ULONGLONG *hits, int maxHits) {
    HANDLE h = OpenProcess(0x410, FALSE, pid); /* QUERY_INFORMATION | VM_READ */
    if (!h) return -1;
    BYTE *buf = (BYTE *)malloc(CHUNK);
    if (!buf) { CloseHandle(h); return -1; }
    int nhits = 0, regions = 0, skippedBig = 0;
    ULONGLONG scanned = 0, a = 0, skippedBytes = 0;
    ULONGLONG t0 = GetTickCount64();
    ULONGLONG nextProg = t0 + 10000ULL;   /* progress line every 10 s while one pass is running */
    MEMORY_BASIC_INFORMATION mbi;
    while (a < g_maxAddr) {
        if (!VirtualQueryEx(h, (LPCVOID)(ULONG_PTR)a, &mbi, sizeof mbi)) break;
        ULONGLONG b = (ULONGLONG)(ULONG_PTR)mbi.BaseAddress, s = (ULONGLONG)mbi.RegionSize;
        if (s == 0) break;
        if (is_heap(&mbi) && s > g_curMax) {
            skippedBig++; skippedBytes += s;
        } else if (is_heap(&mbi)) {
            regions++;
            ULONGLONG off = 0;
            while (off < s) {
                SIZE_T take = (s - off > CHUNK) ? CHUNK : (SIZE_T)(s - off);
                SIZE_T got = 0;
                if (ReadProcessMemory(h, (LPCVOID)(ULONG_PTR)(b + off), buf, take, &got) && got > 0) {
                    scanned += got;
                    for (SIZE_T i = 0; i + 32 <= got; i += 4) {
                        if (buf[i] != needle[0] || buf[i + 1] != needle[1] || buf[i + 2] != needle[2] || buf[i + 3] != needle[3]) continue;
                        if (memcmp(buf + i, needle, 32) != 0) continue;
                        ULONGLONG addr = b + off + i;
                        if (nhits > 0 && hits[nhits - 1] == addr) continue; /* chunk-overlap duplicate */
                        if (nhits < maxHits) hits[nhits++] = addr;
                    }
                }
                if (GetTickCount64() >= nextProg) {
                    say(C_DGRAY, "  ...scan in progress: %llu MB read so far, now at address 0x%llX",
                        scanned / (1024ULL * 1024ULL), b + off);
                    nextProg += 10000ULL;
                }
                if (take < CHUNK) break;
                off += CHUNK - 32; /* overlap (multiple of 4) so no match is missed */
            }
        }
        if (GetTickCount64() >= nextProg) {
            say(C_DGRAY, "  ...scan in progress: %llu MB read so far, now at address 0x%llX",
                scanned / (1024ULL * 1024ULL), b + s);
            nextProg += 10000ULL;
        }
        if (b + s <= a) break;
        a = b + s;
    }
    free(buf);
    CloseHandle(h);
    g_lastScanMs = GetTickCount64() - t0;
    InterlockedIncrement(&g_scanPasses);
    snprintf(g_lastScan, sizeof g_lastScan, "search: %llu MB in %d regions, %.1f s",
             scanned / (1024ULL * 1024ULL), regions, (double)(GetTickCount64() - t0) / 1000.0);
    if (skippedBig) {
        char t2[96]; snprintf(t2, sizeof t2, ", SKIPPED %d huge region(s) = %llu MB", skippedBig, skippedBytes / (1024ULL * 1024ULL));
        strncat(g_lastScan, t2, sizeof g_lastScan - strlen(g_lastScan) - 1);
    }
    return nhits;
}

/* Read [a, a+n). With requireHeap, only if it lies entirely inside ONE heap region. */
static int read_block(DWORD pid, ULONGLONG a, BYTE *out, SIZE_T n, int requireHeap) {
    HANDLE h = OpenProcess(0x410, FALSE, pid);
    if (!h) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    int ok = 0;
    if (VirtualQueryEx(h, (LPCVOID)(ULONG_PTR)a, &mbi, sizeof mbi)) {
        ULONGLONG base = (ULONGLONG)(ULONG_PTR)mbi.BaseAddress, end = base + mbi.RegionSize;
        if (!requireHeap || (is_heap(&mbi) && a >= base && a + n <= end)) {
            SIZE_T got = 0;
            if (ReadProcessMemory(h, (LPCVOID)(ULONG_PTR)a, out, n, &got) && got == n) ok = 1;
        }
    }
    CloseHandle(h);
    return ok;
}

enum { ST_INVALID = 0, ST_ORIGINAL = 1, ST_PATCHED = 2 };

static int store_state_ex(DWORD pid, ULONGLONG array, int requireHeap) {
    BYTE raw[BLOCK_SIZE];
    if (!read_block(pid, array, raw, BLOCK_SIZE, requireHeap)) return ST_INVALID;
    for (int e = 0; e < ENTRY_COUNT; e++) {
        const BYTE *p = raw + e * ENTRY_SIZE;
        DWORD id; memcpy(&id, p, 4);
        if (id != (DWORD)(e + 1)) return ST_INVALID;
        int okOrig = memcmp(p + 4, g_keys[e], 32) == 0;
        int ok = okOrig;
        if (e == TARGET_GROUP - 1) ok = okOrig || memcmp(p + 4, g_newKey, 32) == 0;
        if (!ok) return ST_INVALID;
        if (p[36] != EXPECTED_FLAGS[e]) return ST_INVALID;
        /* bytes 37..39 are padding: 7F 00 00 on Windows, 00 00 00 under Wine/FEX, so they are NOT checked */
    }
    const BYTE *k8 = raw + (TARGET_GROUP - 1) * ENTRY_SIZE + 4;
    return memcmp(k8, g_newKey, 32) == 0 ? ST_PATCHED : ST_ORIGINAL;
}
static int store_state(DWORD pid, ULONGLONG array) { return store_state_ex(pid, array, 1); }

/* Explains why a key copy is not a valid store: region info, how many entries match, hex dump around it. */
static void explain_hit(DWORD pid, ULONGLONG keyAddr) {
    HANDLE h = OpenProcess(0x410, FALSE, pid);
    if (!h) return;
    MEMORY_BASIC_INFORMATION mbi;
    char pn[32] = "?"; const char *tn = "?"; ULONGLONG rb = 0, rs = 0;
    if (VirtualQueryEx(h, (LPCVOID)(ULONG_PTR)keyAddr, &mbi, sizeof mbi)) {
        prot_name(mbi.Protect, pn, sizeof pn); tn = type_name(mbi.Type);
        rb = (ULONGLONG)(ULONG_PTR)mbi.BaseAddress; rs = (ULONGLONG)mbi.RegionSize;
    }
    ULONGLONG array = keyAddr - 4;
    BYTE raw[BLOCK_SIZE]; SIZE_T got = 0;
    char why[160];
    if (!ReadProcessMemory(h, (LPCVOID)(ULONG_PTR)array, raw, BLOCK_SIZE, &got) || got != BLOCK_SIZE) {
        snprintf(why, sizeof why, "cannot read the full 480-byte block (read %llu bytes)", (unsigned long long)got);
    } else {
        int e; why[0] = 0;
        for (e = 0; e < ENTRY_COUNT; e++) {
            const BYTE *p = raw + e * ENTRY_SIZE; DWORD id; memcpy(&id, p, 4);
            if (id != (DWORD)(e + 1)) { snprintf(why, sizeof why, "entry %d: id=%lu (expected %d)", e + 1, (unsigned long)id, e + 1); break; }
            int okk = memcmp(p + 4, g_keys[e], 32) == 0 || (e == TARGET_GROUP - 1 && memcmp(p + 4, g_newKey, 32) == 0);
            if (!okk) { snprintf(why, sizeof why, "entry %d: key bytes differ", e + 1); break; }
            if (p[36] != EXPECTED_FLAGS[e]) { snprintf(why, sizeof why, "entry %d: flag=%u (expected %u)", e + 1, p[36], EXPECTED_FLAGS[e]); break; }
        }
        if (e == ENTRY_COUNT) snprintf(why, sizeof why, "all 12 entries match (content OK) - rejected only because of region rules");
        else if (!why[0]) snprintf(why, sizeof why, "unknown");
    }
    logf_("EXPLAIN key copy @0x%llX (mod16=%llu) region 0x%llX size %llu KB type=%s prot=%s -> %s",
          keyAddr, keyAddr % 16, rb, rs / 1024, tn, pn, why);
    BYTE dump[96];
    if (ReadProcessMemory(h, (LPCVOID)(ULONG_PTR)(keyAddr - 8), dump, sizeof dump, &got) && got == sizeof dump) {
        for (int line = 0; line < 3; line++) {
            char hx[80]; int pos = 0;
            for (int i = 0; i < 32; i++) pos += snprintf(hx + pos, sizeof hx - pos, "%02X", dump[line * 32 + i]);
            logf_("EXPLAIN   bytes @0x%llX: %s", keyAddr - 8 + line * 32, hx);
        }
    }
    CloseHandle(h);
}

typedef struct { ULONGLONG array; int state; } Store;

static int find_stores(DWORD pid, Store *out, int maxOut) {
    ULONGLONG hits[MAX_HITS];
    int n = scan_heap(pid, g_keys[0], hits, MAX_HITS);
    if (n < 0) {
        snprintf(g_lastScan, sizeof g_lastScan, "search failed: OpenProcess error %lu", GetLastError());
        return -1;
    }
    int found = 0;
    for (int i = 0; i < n; i++) {
        ULONGLONG array = hits[i] - 4;
        int st = store_state(pid, array);
        if (st != ST_INVALID && found < maxOut) { out[found].array = array; out[found].state = st; found++; }
    }
    if (n > 0 && found == 0) {   /* key copies exist but none validates: log why (only when the picture changes) */
        static ULONGLONG sigA = 0; static int sigN = -1;
        if (sigN != n || sigA != hits[0]) {
            sigN = n; sigA = hits[0];
            for (int i = 0; i < n && i < 6; i++) explain_hit(pid, hits[i]);
        }
    }
    char tmp[320];
    snprintf(tmp, sizeof tmp, "%s, %d key copies, %d valid store(s)", g_lastScan, n, found);
    snprintf(g_lastScan, sizeof g_lastScan, "%s", tmp);
    return found;
}

/* 0 = ok, 1 = not heap, 2 = bytes changed, 3 = OpenProcess failed, 4 = write failed, 5 = verify failed */
static int write32(DWORD pid, ULONGLONG a, const BYTE *before, const BYTE *after, DWORD *err) {
    HANDLE h = OpenProcess(0x438, FALSE, pid); /* + VM_OPERATION | VM_WRITE */
    if (!h) { *err = GetLastError(); return 3; }
    int rc = 0;
    MEMORY_BASIC_INFORMATION mbi;
    BYTE now[32], chk[32];
    SIZE_T n = 0;
    if (!VirtualQueryEx(h, (LPCVOID)(ULONG_PTR)a, &mbi, sizeof mbi)) { *err = GetLastError(); rc = 1; goto done; }
    {
        ULONGLONG end = (ULONGLONG)(ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
        if (!is_heap(&mbi) || a + 32 > end) { rc = 1; goto done; }
    }
    if (!ReadProcessMemory(h, (LPCVOID)(ULONG_PTR)a, now, 32, &n) || n != 32) { *err = GetLastError(); rc = 1; goto done; }
    if (memcmp(now, before, 32) != 0) { rc = 2; goto done; }
    if (!WriteProcessMemory(h, (LPVOID)(ULONG_PTR)a, after, 32, &n) || n != 32) { *err = GetLastError(); rc = 4; goto done; }
    if (!ReadProcessMemory(h, (LPCVOID)(ULONG_PTR)a, chk, 32, &n) || n != 32 || memcmp(chk, after, 32) != 0) { rc = 5; goto done; }
done:
    CloseHandle(h);
    return rc;
}

/* returns 0 on success, otherwise prints the reason and returns nonzero */
static int patch(DWORD pid, ULONGLONG array, int *accessDenied) {
    ULONGLONG keyAddr = array + (TARGET_GROUP - 1) * ENTRY_SIZE + 4;
    int rc = 0; DWORD err = 0;
    *accessDenied = 0;
    for (int tryn = 1; ; tryn++) {
        rc = write32(pid, keyAddr, g_keys[TARGET_GROUP - 1], g_newKey, &err);
        if (rc == 0) break;
        if (rc == 2 && tryn < 20) { Sleep(300); continue; }
        break;
    }
    if (rc != 0) {
        static const char *why[] = { "", "ABORT: the key is not in MEM_PRIVATE+PAGE_READWRITE heap",
            "ABORT: the bytes changed right before writing", "OpenProcess failed", "WriteProcessMemory failed",
            "Readback verification failed" };
        say(C_YELLOW, "Could not patch: %s (Win32 error %lu). Retrying.", why[rc], err);
        if (rc == 3 && err == ERROR_ACCESS_DENIED) *accessDenied = 1;
        return rc;
    }
    if (store_state(pid, array) != ST_PATCHED) {
        say(C_YELLOW, "Could not patch: after writing, the store does not validate as patched. Retrying.");
        return 6;
    }
    logf_("group 8 key written at 0x%llX (store 0x%llX)", keyAddr, array);
    return 0;
}

static void announce_ready(void) {
    printf("\n");
    say(C_GREEN, ">>> READY: key applied. If the attempt to enter the realm failed, log in again WITHOUT closing the game. <<<");
    printf("\n");
    Beep(660, 150); Beep(880, 150); Beep(1320, 250);
}

/* ---------------------------------------------------------------- diagnostics */
static const char *type_name(DWORD t) {
    return t == MEM_PRIVATE ? "PRIVATE" : t == MEM_MAPPED ? "MAPPED" : t == MEM_IMAGE ? "IMAGE" : "?";
}
static void prot_name(DWORD p, char *o, size_t n) {
    const char *b;
    switch (p & 0xFF) {
        case 0x01: b = "NOACCESS"; break; case 0x02: b = "R"; break;   case 0x04: b = "RW"; break;
        case 0x08: b = "WC"; break;       case 0x10: b = "X"; break;   case 0x20: b = "RX"; break;
        case 0x40: b = "RWX"; break;      case 0x80: b = "XWC"; break; default: b = "?"; break;
    }
    snprintf(o, n, "%s%s", b, (p & PAGE_GUARD) ? "+GUARD" : "");
}
static int readable_prot(DWORD p) {
    if (p & (PAGE_GUARD | 0x01)) return 0;
    switch (p & 0xFF) { case 0x02: case 0x04: case 0x08: case 0x20: case 0x40: case 0x80: return 1; }
    return 0;
}

typedef struct { DWORD type, prot; ULONGLONG bytes; int regions; } Cat;
typedef struct { int needle; ULONGLONG addr, rbase, rsize; DWORD type, prot; } DHit;
#define DHITS 400

/* Scans ALL committed readable memory (any type/protection, any byte alignment) for the keys and
   logs what it finds, so we can tell whether the key store exists at all and where it lives. */
static void diag_pass(DWORD pid, int passNo) {
    HANDLE h = OpenProcess(0x410, FALSE, pid);
    if (!h) { logf_("DIAG #%d: OpenProcess failed (error %lu)", passNo, GetLastError()); return; }
    const BYTE *nd[4] = { g_keys[0], g_keys[7], g_keys[8], g_newKey };
    const int grp[4]  = { 0, 7, 8, 7 };
    const char *nn[4] = { "key #1 (anchor)", "key #8 (original)", "key #9", "server key (patched)" };
    BYTE f[4]; for (int k = 0; k < 4; k++) f[k] = nd[k][0];
    Cat cats[64]; int ncat = 0;
    struct { ULONGLONG base, size; DWORD type, prot; } top[5]; int ntop = 0;
    int skippedBig = 0; ULONGLONG skippedBytes = 0;
    DHit *hits = (DHit *)malloc(sizeof(DHit) * DHITS); int nh = 0;
    BYTE *buf = (BYTE *)malloc(CHUNK);
    int counts[4] = { 0, 0, 0, 0 };
    if (!hits || !buf) { free(hits); free(buf); CloseHandle(h); return; }
    ULONGLONG scanned = 0, a = 0, t0 = GetTickCount64();
    int regionsScanned = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (a < g_maxAddr) {
        if (!VirtualQueryEx(h, (LPCVOID)(ULONG_PTR)a, &mbi, sizeof mbi)) break;
        ULONGLONG b = (ULONGLONG)(ULONG_PTR)mbi.BaseAddress, sz = (ULONGLONG)mbi.RegionSize;
        if (sz == 0) break;
        if (mbi.State == MEM_COMMIT) {
            int c;
            for (c = 0; c < ncat; c++) if (cats[c].type == mbi.Type && cats[c].prot == mbi.Protect) break;
            if (c == ncat && ncat < 64) { cats[ncat].type = mbi.Type; cats[ncat].prot = mbi.Protect; cats[ncat].bytes = 0; cats[ncat].regions = 0; ncat++; }
            if (c < ncat) { cats[c].bytes += sz; cats[c].regions++; }
            { /* keep the 5 largest committed regions, sorted descending */
                int pos = -1;
                if (ntop < 5) pos = ntop++;
                else if (sz > top[4].size) pos = 4;
                if (pos >= 0) {
                    top[pos].base = b; top[pos].size = sz; top[pos].type = mbi.Type; top[pos].prot = mbi.Protect;
                    for (int q = pos; q > 0 && top[q].size > top[q - 1].size; q--) {
                        ULONGLONG tb = top[q].base, ts = top[q].size; DWORD tt = top[q].type, tp = top[q].prot;
                        top[q].base = top[q - 1].base; top[q].size = top[q - 1].size; top[q].type = top[q - 1].type; top[q].prot = top[q - 1].prot;
                        top[q - 1].base = tb; top[q - 1].size = ts; top[q - 1].type = tt; top[q - 1].prot = tp;
                    }
                }
            }
            if (readable_prot(mbi.Protect) && mbi.Type != MEM_IMAGE && sz > g_maxRegion) {
                skippedBig++; skippedBytes += sz;
            } else if (readable_prot(mbi.Protect) && mbi.Type != MEM_IMAGE) {
                regionsScanned++;
                const SIZE_T stride = (sz > 32ULL * 1024ULL * 1024ULL) ? 4 : 1;  /* big regions: 4-byte aligned only (speed) */
                ULONGLONG off = 0;
                while (off < sz) {
                    SIZE_T take = (sz - off > CHUNK) ? CHUNK : (SIZE_T)(sz - off);
                    SIZE_T got = 0;
                    if (ReadProcessMemory(h, (LPCVOID)(ULONG_PTR)(b + off), buf, take, &got) && got >= 32) {
                        scanned += got;
                        for (SIZE_T i = (off == 0 || stride == 1) ? 0 : 4; i + 32 <= got; i += stride) {
                            BYTE x = buf[i];
                            if (x != f[0] && x != f[1] && x != f[2] && x != f[3]) continue;
                            for (int k = 0; k < 4; k++) {
                                if (x != f[k] || memcmp(buf + i, nd[k], 32) != 0) continue;
                                counts[k]++;
                                if (nh < DHITS) {
                                    hits[nh].needle = k; hits[nh].addr = b + off + i; hits[nh].rbase = b; hits[nh].rsize = sz;
                                    hits[nh].type = mbi.Type; hits[nh].prot = mbi.Protect; nh++;
                                }
                            }
                        }
                    }
                    if (take < CHUNK) break;
                    off += (stride == 1) ? (CHUNK - 31) : (CHUNK - 32); /* next chunk starts at the first position not yet covered */
                }
            }
        }
        if (b + sz <= a) break;
        a = b + sz;
    }
    logf_("DIAG #%d: scanned %llu MB in %d regions (all types; byte-aligned <=32 MB regions, 4-aligned above), %.1f s; skipped %d huge region(s) = %llu MB",
          passNo, scanned / (1024ULL * 1024ULL), regionsScanned, (double)(GetTickCount64() - t0) / 1000.0, skippedBig, skippedBytes / (1024ULL * 1024ULL));
    for (int q = 0; q < ntop; q++) {
        char pn[32]; prot_name(top[q].prot, pn, sizeof pn);
        logf_("DIAG #%d:   largest region #%d: 0x%llX size %llu MB type=%s prot=%s", passNo, q + 1, top[q].base, top[q].size / (1024ULL * 1024ULL), type_name(top[q].type), pn);
    }
    for (int c = 0; c < ncat; c++) {
        char pn[32]; prot_name(cats[c].prot, pn, sizeof pn);
        logf_("DIAG #%d:   committed %-8s %-9s %7llu MB in %d regions", passNo, type_name(cats[c].type), pn,
              cats[c].bytes / (1024ULL * 1024ULL), cats[c].regions);
    }
    for (int k = 0; k < 4; k++) logf_("DIAG #%d: %s: %d hit(s)", passNo, nn[k], counts[k]);
    int shown[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < nh; i++) {
        int k = hits[i].needle;
        if (shown[k]++ >= 12) continue;
        ULONGLONG array = hits[i].addr - (ULONGLONG)(grp[k] * ENTRY_SIZE + 4);
        int stAny = store_state_ex(pid, array, 0);
        char pn[32]; prot_name(hits[i].prot, pn, sizeof pn);
        logf_("DIAG #%d:   %s @0x%llX (mod16=%llu) region 0x%llX size %llu KB type=%s prot=%s -> valid store at 0x%llX: %s",
              passNo, nn[k], hits[i].addr, hits[i].addr % 16, hits[i].rbase, hits[i].rsize / 1024, type_name(hits[i].type), pn,
              array, stAny == ST_PATCHED ? "YES (patched)" : stAny == ST_ORIGINAL ? "YES (original)" : "no");
    }
    free(hits); free(buf); CloseHandle(h);
}


/* One-time probe (runs in the diagnostic thread): which processes exist, what the target looks like,
   and whether the portal string from Config.wtf is visible in its small memory regions. That tells us
   whether we are attached to the real client and reading real data. */
static void probe_process(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe; pe.dwSize = sizeof pe;
        char line[900]; line[0] = 0;
        if (Process32First(snap, &pe)) {
            do {
                char one[128];
                snprintf(one, sizeof one, "%s(pid %lu, thr %lu) ", pe.szExeFile, (unsigned long)pe.th32ProcessID, (unsigned long)pe.cntThreads);
                if (strlen(line) + strlen(one) < sizeof line - 1) strcat(line, one);
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
        logf_("PROBE processes: %s", line);
    }
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) {
        logf_("PROBE modules of PID %lu: snapshot failed (error %lu)", (unsigned long)pid, GetLastError());
    } else {
        MODULEENTRY32 me; me.dwSize = sizeof me; int n = 0;
        if (Module32First(snap, &me)) {
            do {
                if (n < 40) logf_("PROBE module #%d %s base=0x%llX size=%lu KB", n + 1, me.szModule,
                                  (unsigned long long)(ULONG_PTR)me.modBaseAddr, (unsigned long)(me.modBaseSize / 1024));
                n++;
            } while (Module32Next(snap, &me));
        }
        CloseHandle(snap);
        logf_("PROBE PID %lu has %d module(s)", (unsigned long)pid, n);
    }
    /* portal string search: ASCII and UTF-16, committed readable memory, regions <= 32 MB (fast) */
    HANDLE h = OpenProcess(0x410, FALSE, pid);
    if (!h) return;
    BYTE *buf = (BYTE *)malloc(CHUNK);
    if (!buf) { CloseHandle(h); return; }
    const char *ps = PORTAL; const int pl = (int)strlen(PORTAL);
    BYTE wide[128]; int wl = pl * 2;
    for (int i = 0; i < pl; i++) { wide[2 * i] = (BYTE)ps[i]; wide[2 * i + 1] = 0; }
    int hitsA = 0, hitsW = 0, shown = 0;
    ULONGLONG a = 0, scanned = 0, t0 = GetTickCount64(), lastA = 0, lastW = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (a < g_maxAddr) {
        if (!VirtualQueryEx(h, (LPCVOID)(ULONG_PTR)a, &mbi, sizeof mbi)) break;
        ULONGLONG b = (ULONGLONG)(ULONG_PTR)mbi.BaseAddress, sz = (ULONGLONG)mbi.RegionSize;
        if (sz == 0) break;
        if (mbi.State == MEM_COMMIT && readable_prot(mbi.Protect) && sz <= 32ULL * 1024ULL * 1024ULL) {
            ULONGLONG off = 0;
            while (off < sz) {
                SIZE_T take = (sz - off > CHUNK) ? CHUNK : (SIZE_T)(sz - off), got = 0;
                if (ReadProcessMemory(h, (LPCVOID)(ULONG_PTR)(b + off), buf, take, &got) && got >= (SIZE_T)wl) {
                    scanned += got;
                    for (SIZE_T i = 0; i + wl <= got; i++) {
                        if (buf[i] != (BYTE)ps[0]) continue;
                        ULONGLONG ad = b + off + i;
                        if (i + pl <= got && memcmp(buf + i, ps, pl) == 0 && ad != lastA) {
                            lastA = ad; hitsA++;
                            if (shown++ < 4) logf_("PROBE portal string (ASCII) @0x%llX type=%s", ad, type_name(mbi.Type));
                        } else if (memcmp(buf + i, wide, wl) == 0 && ad != lastW) {
                            lastW = ad; hitsW++;
                            if (shown++ < 4) logf_("PROBE portal string (UTF-16) @0x%llX type=%s", ad, type_name(mbi.Type));
                        }
                    }
                }
                if (take < CHUNK) break;
                off += CHUNK - 64;
            }
        }
        if (b + sz <= a) break;
        a = b + sz;
    }
    free(buf); CloseHandle(h);
    logf_("PROBE portal string \"%s\": %d ASCII hit(s), %d UTF-16 hit(s) (%llu MB scanned in regions <=32 MB, %.1f s)",
          PORTAL, hitsA, hitsW, scanned / (1024ULL * 1024ULL), (double)(GetTickCount64() - t0) / 1000.0);
}

static DWORD WINAPI diag_thread(LPVOID arg) {
    DWORD pid = (DWORD)(ULONG_PTR)arg;
    int pass = 0, steps = 6;
    for (;;) {
        for (int i = 0; i < steps; i++) { if (g_stop) return 0; Sleep(500); }
        steps = 60; /* every 30 s after the first pass */
        if (g_stop) return 0;
        if (g_storeHeld) continue;
        if (g_scanPasses < 1 || g_lastScanMs > 5000) continue;   /* do not add load while scans are slow */
        if (pass >= 20) return 0;
        if (pass == 0) probe_process(pid);
        diag_pass(pid, ++pass);
    }
}

/* ---------------------------------------------------------------- WTF config */
static void ensure_portal(const char *path) {
    /* read file */
    char *data = NULL; long size = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
        data = (char *)malloc(size + 1);
        if (data) { size = (long)fread(data, 1, size, f); data[size] = 0; }
        fclose(f);
    }
    /* split into lines */
    int cap = 64, n = 0;
    char **lines = (char **)malloc(cap * sizeof(char *));
    if (data) {
        char *p = data;
        while (*p) {
            char *e = p; while (*e && *e != '\n') e++;
            size_t len = (size_t)(e - p); if (len && p[len - 1] == '\r') len--;
            if (n + 4 >= cap) { cap *= 2; lines = (char **)realloc(lines, cap * sizeof(char *)); }
            lines[n] = (char *)malloc(len + 1); memcpy(lines[n], p, len); lines[n][len] = 0; n++;
            p = *e ? e + 1 : e;
        }
    }
    char wanted[128]; snprintf(wanted, sizeof wanted, "SET portal \"%s\"", PORTAL);
    int changed = 0, havePortal = 0, haveLocale = 0;
    for (int i = 0; i < n; i++) {
        if (strncmp(lines[i], "SET portal ", 11) == 0) {
            havePortal = 1;
            if (strcmp(lines[i], wanted) != 0) { free(lines[i]); lines[i] = _strdup(wanted); changed = 1; }
        }
        if (strncmp(lines[i], "SET textLocale ", 15) == 0) haveLocale = 1;
    }
    if (!havePortal) { lines[n++] = _strdup(wanted); changed = 1; }
    if (!haveLocale) { lines[n++] = _strdup("SET textLocale \"enUS\""); changed = 1; }
    if (changed) {
        f = fopen(path, "wb");
        if (f) {
            for (int i = 0; i < n; i++) fprintf(f, "%s\r\n", lines[i]);
            fclose(f);
            const char *leaf = strrchr(path, '\\'); leaf = leaf ? leaf + 1 : path;
            say(C_GRAY, "Configuration updated: WTF\\%s (portal %s)", leaf, PORTAL);
        } else {
            say(C_YELLOW, "Could not write %s", path);
        }
    }
    for (int i = 0; i < n; i++) free(lines[i]);
    free(lines); free(data);
}

/* ---------------------------------------------------------------- version / processes */
/* Finds WowB.exe processes by name. Returns the count; PIDs into pids[]. */
static int find_clients(DWORD *pids, int max) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe; pe.dwSize = sizeof pe;
    int n = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (is_client_name(pe.szExeFile) && n < max) pids[n++] = pe.th32ProcessID;
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return n;
}

static int is_alive(HANDLE h, DWORD pid) {
    DWORD code;
    if (h && GetExitCodeProcess(h, &code)) return code == STILL_ACTIVE;
    DWORD pids[16];
    int n = find_clients(pids, 16);
    for (int i = 0; i < n; i++) if (pids[i] == pid) return 1;
    return 0;
}

/* ---------------------------------------------------------------- game folder detection */
static char g_exeDir[MAX_PATH];     /* folder of PlayBeta.exe itself (log.txt goes here) */
static char g_dirArg[MAX_PATH];     /* --dir "path" */
static char g_cand[16][MAX_PATH];
static int  g_ncand = 0;

static void norm_slashes(char *p) { for (; *p; p++) if (*p == '/') *p = '\\'; }
static void strip_last(char *p) { char *s = strrchr(p, '\\'); if (s) *s = 0; }
static void trim_trailing_slash(char *p) { size_t n = strlen(p); while (n > 3 && p[n - 1] == '\\') p[--n] = 0; }
static int dir_has_file(const char *dir, const char *name) {
    char t[MAX_PATH + 32]; snprintf(t, sizeof t, "%s\\%s", dir, name);
    return GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES;
}
/* Name of the ARM64 client present in dir (either spelling), or NULL. */
static const char *arm_name_in(const char *dir) {
    if (dir_has_file(dir, EXE_ARM64)) return EXE_ARM64;
    if (dir_has_file(dir, EXE_ARM64_ALT)) return EXE_ARM64_ALT;
    return NULL;
}
static int dir_has_game(const char *dir) {
    if (!dir[0]) return 0;
    return dir_has_file(dir, EXE_X64) || arm_name_in(dir) != NULL;
}
static void add_cand(const char *dir) {
    if (!dir || !dir[0] || g_ncand >= 15) return;
    for (int i = 0; i < g_ncand; i++) if (!_stricmp(g_cand[i], dir)) return;
    snprintf(g_cand[g_ncand++], MAX_PATH, "%s", dir);
}
static void add_cand_family(const char *dir) {
    char t[MAX_PATH], par[MAX_PATH];
    if (!dir || !dir[0]) return;
    snprintf(t, sizeof t, "%s", dir); norm_slashes(t); trim_trailing_slash(t);
    add_cand(t);
    if (snprintf(par, sizeof par, "%s\\_classic_beta_", t) < (int)sizeof par) add_cand(par);
    snprintf(par, sizeof par, "%s", t); strip_last(par); if (par[0] && strlen(par) > 2) add_cand(par);
}
/* Builds the candidate list and picks the first folder that contains WowB.exe. */
static int resolve_game_dir(const char *argv0) {
    char t[MAX_PATH];
    if (g_dirArg[0]) add_cand_family(g_dirArg);
    add_cand_family(g_exeDir);
    if (GetCurrentDirectoryA(MAX_PATH, t)) add_cand_family(t);
    if (argv0 && (strchr(argv0, '\\') || strchr(argv0, '/'))) {
        snprintf(t, sizeof t, "%s", argv0); norm_slashes(t); strip_last(t); add_cand_family(t);
    }
    { /* the running game, if any: use its own folder */
        DWORD pids[16]; int n = find_clients(pids, 16);
        for (int i = 0; i < n; i++) {
            HANDLE h = OpenProcess(0x1000, FALSE, pids[i]); /* QUERY_LIMITED_INFORMATION */
            if (!h) continue;
            char pp[MAX_PATH]; DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameA(h, 0, pp, &sz)) { norm_slashes(pp); strip_last(pp); add_cand(pp); }
            CloseHandle(h);
        }
    }
    for (int i = 0; i < g_ncand; i++) {
        if (dir_has_game(g_cand[i])) {
            snprintf(g_gameDir, sizeof g_gameDir, "%s", g_cand[i]);
            /* which one to launch: --arm64 forces the ARM64 build; otherwise WowB.exe, or the ARM64 build if it is the only one */
            int hasX64 = dir_has_file(g_gameDir, EXE_X64);
            const char *armName = arm_name_in(g_gameDir);
            g_exeName = ((g_preferArm64 && armName) || !hasX64) ? armName : EXE_X64;
            snprintf(g_exe, sizeof g_exe, "%s\\%s", g_gameDir, g_exeName);
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- self-test (--selftest) */
/* Parent: spawns a copy of this exe as a "fake game" child that builds fake key stores in several kinds of
   memory (process heap, private heap, VirtualAlloc, malloc, misaligned, 128 MB block, partly committed block),
   then exercises EXACTLY the code paths the real patcher uses against it and reports PASS/FAIL. */
static int st_pass = 0, st_fail = 0;
static void st_check(int ok, const char *fmt, ...) {
    char msg[600]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    say(ok ? C_GREEN : C_RED, "[%s] %s", ok ? "PASS" : "FAIL", msg);
    if (ok) st_pass++; else st_fail++;
}
static void st_info(const char *fmt, ...) {
    char msg[600]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    say(C_GRAY, "[INFO] %s", msg);
}

static void build_store(BYTE *arr) {
    memset(arr, 0, BLOCK_SIZE);
    for (int e = 0; e < ENTRY_COUNT; e++) {
        BYTE *p = arr + e * ENTRY_SIZE; DWORD id = (DWORD)(e + 1);
        memcpy(p, &id, 4); memcpy(p + 4, g_keys[e], 32); p[36] = EXPECTED_FLAGS[e]; memcpy(p + 37, ENTRY_TAIL, 3);
    }
}

#define ST_MAX 8
static int selftest_child(const char *outfile) {
    struct { const char *name; BYTE *arr; } st[ST_MAX]; int n = 0;
    BYTE *p;
    p = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, BLOCK_SIZE + 64);
    if (p) { build_store(p); st[n].name = "processheap"; st[n++].arr = p; }
    HANDLE ph = HeapCreate(0, 0, 0);
    p = ph ? (BYTE *)HeapAlloc(ph, HEAP_ZERO_MEMORY, BLOCK_SIZE + 64) : NULL;
    if (p) { build_store(p); st[n].name = "privateheap"; st[n++].arr = p; }
    BYTE *v = (BYTE *)VirtualAlloc(NULL, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (v) {
        build_store(v + 0x100);  st[n].name = "virtualalloc"; st[n++].arr = v + 0x100;
        build_store(v + 0x2004); st[n].name = "valloc_plus4"; st[n++].arr = v + 0x2004;
    }
    p = (BYTE *)malloc(BLOCK_SIZE + 64);
    if (p) { build_store(p); st[n].name = "malloc"; st[n++].arr = p; }
    const SIZE_T bigSize = 128u * 1024u * 1024u;
    BYTE *big = (BYTE *)VirtualAlloc(NULL, bigSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (big) memset(big, 0xAB, bigSize);
    BYTE *edge = (BYTE *)VirtualAlloc(NULL, 3 * 4096, MEM_RESERVE, PAGE_READWRITE);
    if (edge) { VirtualAlloc(edge, 2 * 4096, MEM_COMMIT, PAGE_READWRITE); memset(edge, 0x5A, 2 * 4096); }

    FILE *f = fopen(outfile, "wb");
    if (!f) return 2;
    for (int i = 0; i < n; i++) {
        MEMORY_BASIC_INFORMATION mbi; char pn[32] = "?";
        memset(&mbi, 0, sizeof mbi);
        if (VirtualQuery(st[i].arr, &mbi, sizeof mbi)) prot_name(mbi.Protect, pn, sizeof pn);
        fprintf(f, "STORE %s %llX %s %s %llu\n", st[i].name, (unsigned long long)(ULONG_PTR)st[i].arr,
                type_name(mbi.Type), pn, (unsigned long long)(mbi.RegionSize / 1024));
    }
    if (big) fprintf(f, "BIG %llX %llu\n", (unsigned long long)(ULONG_PTR)big, (unsigned long long)(bigSize / (1024 * 1024)));
    if (edge) fprintf(f, "EDGE %llX\n", (unsigned long long)(ULONG_PTR)edge);
    fprintf(f, "READY\n");
    fclose(f);

    char flag[MAX_PATH + 8], res[MAX_PATH + 8];
    snprintf(flag, sizeof flag, "%s.flag", outfile); snprintf(res, sizeof res, "%s.result", outfile);
    for (int i = 0; i < 600; i++) {   /* wait up to 120 s for the parent's "check now" flag */
        if (GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES) {
            FILE *r = fopen(res, "wb");
            if (r) {
                for (int k = 0; k < n; k++) {
                    const BYTE *a = st[k].arr;
                    int ok = memcmp(a + (TARGET_GROUP - 1) * ENTRY_SIZE + 4, g_newKey, 32) == 0 && memcmp(a + 4, g_keys[0], 32) == 0;
                    fprintf(r, "%s %s\n", st[k].name, ok ? "OK" : "FAIL");
                }
                fclose(r);
            }
            Sleep(1500);
            return 0;
        }
        Sleep(200);
    }
    return 0;
}

static void run_selftest(const char *argv0) {
    st_info("SELF-TEST: checks how Wine + emulation handles the exact operations the patcher needs.");

    /* ---- environment */
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    SYSTEM_INFO si2; GetSystemInfo(&si2);
    st_info("Native CPU arch=%u (9=x64, 12=ARM64), process-view arch=%u, page size=%lu, allocation granularity=%lu",
            si.wProcessorArchitecture, si2.wProcessorArchitecture, si.dwPageSize, si.dwAllocationGranularity);
    st_info("Address space reported: 0x%llX - 0x%llX; PlayBeta scans up to 0x%llX",
            (unsigned long long)(ULONG_PTR)si.lpMinimumApplicationAddress, (unsigned long long)(ULONG_PTR)si.lpMaximumApplicationAddress,
            (unsigned long long)g_maxAddr);
    {
        typedef LONG (WINAPI *rgv_t)(OSVERSIONINFOW *);
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        rgv_t rgv = nt ? (rgv_t)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
        OSVERSIONINFOW vi; memset(&vi, 0, sizeof vi); vi.dwOSVersionInfoSize = sizeof vi;
        if (rgv && rgv(&vi) == 0) st_info("Windows version reported by ntdll: %lu.%lu build %lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    }

    /* ---- game file version, read three ways */
    if (resolve_game_dir(argv0)) {
        st_info("Game folder: %s", g_gameDir);
        DWORD dummy, sz = GetFileVersionInfoSizeA(g_exe, &dummy);
        void *blk = sz ? malloc(sz) : NULL;
        if (blk && GetFileVersionInfoA(g_exe, 0, sz, blk)) {
            VS_FIXEDFILEINFO *ffi = NULL; UINT len = 0;
            if (VerQueryValueA(blk, "\\", (LPVOID *)&ffi, &len) && ffi)
                st_info("WowB.exe numeric version: MS=0x%08lX LS=0x%08lX -> %u.%u.%u.%u", ffi->dwFileVersionMS, ffi->dwFileVersionLS,
                        HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
            WORD *tr = NULL; UINT trLen = 0;
            if (VerQueryValueA(blk, "\\VarFileInfo\\Translation", (LPVOID *)&tr, &trLen) && tr && trLen >= 4) {
                char sub[96]; char *val = NULL; UINT vlen = 0;
                snprintf(sub, sizeof sub, "\\StringFileInfo\\%04x%04x\\FileVersion", tr[0], tr[1]);
                if (VerQueryValueA(blk, sub, (LPVOID *)&val, &vlen) && val) st_info("WowB.exe string FileVersion: \"%s\"", val);
                snprintf(sub, sizeof sub, "\\StringFileInfo\\%04x%04x\\ProductVersion", tr[0], tr[1]);
                if (VerQueryValueA(blk, sub, (LPVOID *)&val, &vlen) && val) st_info("WowB.exe string ProductVersion: \"%s\"", val);
            }
            st_info("Note: a numeric version field is only 16 bits per part (max 65535), so a build number like 70009 cannot be stored there as-is.");
        } else st_info("Could not read the version resource of WowB.exe.");
        free(blk);
    } else st_info("WowB.exe not found near this exe; skipping the game-version check.");

    /* ---- spawn the fake-game child */
    char tmp[MAX_PATH], flag[MAX_PATH + 8], res[MAX_PATH + 8], selfp[MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s\\selftest_child.txt", g_exeDir);
    snprintf(flag, sizeof flag, "%s.flag", tmp); snprintf(res, sizeof res, "%s.result", tmp);
    DeleteFileA(tmp); DeleteFileA(flag); DeleteFileA(res);
    GetModuleFileNameA(NULL, selfp, MAX_PATH);
    char cmd[2 * MAX_PATH + 64]; snprintf(cmd, sizeof cmd, "\"%s\" --selftest-child \"%s\"", selfp, tmp);
    STARTUPINFOA sti; PROCESS_INFORMATION pi; memset(&sti, 0, sizeof sti); sti.cb = sizeof sti; memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(selfp, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, g_exeDir, &sti, &pi)) {
        st_check(0, "start the fake-game child process (error %lu)", GetLastError()); return;
    }
    DWORD cpid = pi.dwProcessId;
    int ready = 0; char content[8192] = "";
    for (int i = 0; i < 150 && !ready; i++) {   /* up to 30 s */
        Sleep(200);
        FILE *f = fopen(tmp, "rb");
        if (f) { size_t n = fread(content, 1, sizeof content - 1, f); content[n] = 0; fclose(f); ready = strstr(content, "READY") != NULL; }
    }
    st_check(ready, "fake-game child started and published its test memory (PID %lu)", cpid);
    if (!ready) { TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return; }

    struct { char name[64]; ULONGLONG addr; char type[16], prot[32]; ULONGLONG kb; } ts[ST_MAX]; int nts = 0;
    ULONGLONG bigAddr = 0, bigMB = 0, edgeAddr = 0;
    for (char *line = strtok(content, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strncmp(line, "STORE ", 6) && nts < ST_MAX) {
            unsigned long long a = 0, kb = 0;
            if (sscanf(line, "STORE %63s %llX %15s %31s %llu", ts[nts].name, &a, ts[nts].type, ts[nts].prot, &kb) == 5) { ts[nts].addr = a; ts[nts].kb = kb; nts++; }
        } else if (!strncmp(line, "BIG ", 4)) { unsigned long long a = 0, m = 0; if (sscanf(line, "BIG %llX %llu", &a, &m) == 2) { bigAddr = a; bigMB = m; } }
        else if (!strncmp(line, "EDGE ", 5)) { unsigned long long a = 0; if (sscanf(line, "EDGE %llX", &a) == 1) edgeAddr = a; }
    }

    HANDLE hq = OpenProcess(0x438, FALSE, cpid);
    st_check(hq != NULL, "OpenProcess with read+write rights on another process (error %lu)", hq ? 0UL : GetLastError());
    if (hq) {
        /* ---- per-memory-kind checks */
        for (int i = 0; i < nts; i++) {
            MEMORY_BASIC_INFORMATION mbi; char pn[32] = "?"; memset(&mbi, 0, sizeof mbi);
            BOOL q = VirtualQueryEx(hq, (LPCVOID)(ULONG_PTR)ts[i].addr, &mbi, sizeof mbi);
            if (q) prot_name(mbi.Protect, pn, sizeof pn);
            st_info("%s @0x%llX: parent sees state=0x%lX type=%s prot=%s region=%llu KB | child's own view: type=%s prot=%s %llu KB",
                    ts[i].name, ts[i].addr, q ? mbi.State : 0, q ? type_name(mbi.Type) : "?", pn, q ? (unsigned long long)(mbi.RegionSize / 1024) : 0ULL,
                    ts[i].type, ts[i].prot, ts[i].kb);
            st_check(q && is_heap(&mbi), "%s: memory counts as 'committed private read/write' (the only kind the scanner reads)", ts[i].name);
            st_check(store_state_ex(cpid, ts[i].addr, 0) == ST_ORIGINAL, "%s: cross-process read + full 12-entry validation (any memory)", ts[i].name);
            st_check(store_state_ex(cpid, ts[i].addr, 1) == ST_ORIGINAL, "%s: same validation with the strict 'inside one heap region' rule", ts[i].name);
        }

        /* ---- the real scanner over the whole child process */
        ULONGLONG *hits = (ULONGLONG *)malloc(sizeof(ULONGLONG) * MAX_HITS);
        int nh = hits ? scan_heap(cpid, g_keys[0], hits, MAX_HITS) : -1;
        st_check(nh >= 0, "full scan of the other process worked: %s, %d key copies", nh >= 0 ? g_lastScan : "failed", nh);
        for (int i = 0; i < nts; i++) {
            int found = 0;
            for (int k = 0; k < nh; k++) if (hits[k] == ts[i].addr + 4) found = 1;
            st_check(found, "%s: located by the heap scan (anchor key address match)", ts[i].name);
        }
        free(hits);

        /* ---- write + readback with the real patch routine */
        for (int i = 0; i < nts; i++) {
            int denied = 0;
            int rc = patch(cpid, ts[i].addr, &denied);
            st_check(rc == 0 && store_state_ex(cpid, ts[i].addr, 0) == ST_PATCHED, "%s: write of the 32-byte key + readback verification (code %d)", ts[i].name, rc);
        }

        /* ---- throughput and boundary behaviour */
        if (bigAddr) {
            BYTE *buf = (BYTE *)malloc(CHUNK); ULONGLONG total = 0, t0 = GetTickCount64(); int okAll = buf != NULL;
            for (ULONGLONG off = 0; okAll && off < bigMB * 1024ULL * 1024ULL; off += CHUNK) {
                SIZE_T got = 0;
                if (!ReadProcessMemory(hq, (LPCVOID)(ULONG_PTR)(bigAddr + off), buf, CHUNK, &got) || got != CHUNK) okAll = 0; else total += got;
            }
            double sec = (double)(GetTickCount64() - t0) / 1000.0; if (sec < 0.001) sec = 0.001;
            st_check(okAll, "read %llu MB of another process in 4 MB chunks: %.0f MB/s (%.2f s)", total / (1024ULL * 1024ULL), (double)(total / (1024ULL * 1024ULL)) / sec, sec);
            free(buf);
        }
        if (edgeAddr) {
            BYTE *eb = (BYTE *)malloc(3 * 4096); SIZE_T got = 0;
            BOOL r = eb ? ReadProcessMemory(hq, (LPCVOID)(ULONG_PTR)edgeAddr, eb, 3 * 4096, &got) : FALSE; DWORD er = GetLastError();
            st_info("Read across the end of committed memory (3 pages asked, 2 committed): returned %s, bytes=%llu, error=%lu (real Windows: FALSE, 8192, 299)",
                    r ? "TRUE" : "FALSE", (unsigned long long)got, er);
            got = 0;
            st_check(eb && ReadProcessMemory(hq, (LPCVOID)(ULONG_PTR)edgeAddr, eb, 2 * 4096, &got) && got == 2 * 4096, "read of exactly the committed part works");
            free(eb);
        }

        /* ---- did the writes really land inside the child? */
        FILE *ff = fopen(flag, "wb"); if (ff) { fputs("go", ff); fclose(ff); }
        char rc2[2048] = ""; int gotRes = 0;
        for (int i = 0; i < 75 && !gotRes; i++) {
            Sleep(200);
            FILE *f = fopen(res, "rb");
            if (f) { size_t n = fread(rc2, 1, sizeof rc2 - 1, f); rc2[n] = 0; fclose(f); gotRes = n > 0; }
        }
        st_check(gotRes, "child answered the 'verify your own memory' request");
        for (int i = 0; i < nts && gotRes; i++) {
            char want[96]; snprintf(want, sizeof want, "%s OK", ts[i].name);
            st_check(strstr(rc2, want) != NULL, "%s: the child itself sees the new key (the write really landed)", ts[i].name);
        }
        CloseHandle(hq);
    }
    TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DeleteFileA(tmp); DeleteFileA(flag); DeleteFileA(res);

    say(st_fail ? C_YELLOW : C_GREEN, "SELF-TEST FINISHED: %d passed, %d failed. Everything is in log.txt next to the exe.", st_pass, st_fail);
    if (st_fail) say(C_YELLOW, "Send me log.txt: the FAIL and INFO lines show exactly which operation behaves differently under this Wine.");
}

/* ---------------------------------------------------------------- main */
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "--selftest-child") && i + 1 < argc) snprintf(g_childFile, sizeof g_childFile, "%s", argv[++i]);
        else if (!_stricmp(argv[i], "--selftest")) g_selftest = 1;
        else if (!_stricmp(argv[i], "--launch")) g_launch = 1;
        else if (!_stricmp(argv[i], "--attach")) g_launch = 0;
        else if (!_stricmp(argv[i], "--arm64")) g_preferArm64 = 1;
        else if (!_stricmp(argv[i], "--nodiag")) g_diagEnabled = 0;
        else if (!_stricmp(argv[i], "--maxregion") && i + 1 < argc) g_maxRegion = (ULONGLONG)atoi(argv[++i]) * 1024ULL * 1024ULL;
        else if (!_stricmp(argv[i], "--fast") && i + 1 < argc) g_fastRegion = (ULONGLONG)atoi(argv[++i]) * 1024ULL * 1024ULL;
        else if (!_stricmp(argv[i], "--delay") && i + 1 < argc) g_startDelay = atoi(argv[++i]);
        else if (!_stricmp(argv[i], "--dryrun")) g_dryRun = 1;
        else if (!_stricmp(argv[i], "--no-default")) g_patchDefault = 0;
        else if (!_stricmp(argv[i], "--wait") && i + 1 < argc) g_waitMin = atoi(argv[++i]);
        else if (!_stricmp(argv[i], "--dir") && i + 1 < argc) snprintf(g_dirArg, sizeof g_dirArg, "%s", argv[++i]);
        else if (!_stricmp(argv[i], "--pid") && i + 1 < argc) g_forcePid = (DWORD)atoi(argv[++i]);
    }
    g_curMax = g_maxRegion;
    SetConsoleTitleA("Play Beta 1.60.1 - do not close this window while playing");

    for (int i = 0; i < ENTRY_COUNT; i++) hex2bin(KEY_HEX[i], g_keys[i], 32);
    hex2bin(NEW_KEY_HEX, g_newKey, 32);
    if (g_childFile[0]) return selftest_child(g_childFile);   /* fake-game child of --selftest */

    GetModuleFileNameA(NULL, g_exeDir, MAX_PATH);
    norm_slashes(g_exeDir); strip_last(g_exeDir);
    if (!g_exeDir[0]) GetCurrentDirectoryA(MAX_PATH, g_exeDir);
    InitializeCriticalSection(&g_cs);
    {   /* use the address-space limit this Wine/emulator really reports */
        SYSTEM_INFO si0; GetSystemInfo(&si0);
        ULONGLONG m = (ULONGLONG)(ULONG_PTR)si0.lpMaximumApplicationAddress;
        if (m > 0x10000000ULL) g_maxAddr = m;
    }
    snprintf(g_logFile, sizeof g_logFile, "%s\\log.txt", g_exeDir);
    {   /* keep log.txt from growing forever */
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(g_logFile, GetFileExInfoStandard, &fa) && fa.nFileSizeHigh == 0 && fa.nFileSizeLow > 2u * 1024u * 1024u)
            DeleteFileA(g_logFile);
    }
    logf_("==================== PlayBeta native started (launch=%d diag=%d dryrun=%d) ====================",
          g_launch, g_diagEnabled, g_dryRun);
    {
        typedef const char *(__cdecl *wgv_t)(void);
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        wgv_t wgv = nt ? (wgv_t)(void *)GetProcAddress(nt, "wine_get_version") : NULL;
        if (wgv) logf_("Running under Wine %s", wgv()); else logf_("Wine version function not found (native Windows?)");
    }

    say(C_CYAN, "PlayBeta (native) started. Exe folder: %s", g_exeDir);
    logf_("Address space scan limit: 0x%llX", g_maxAddr);
    if (g_selftest) {
        run_selftest(argv[0]);
        printf("\nPress Enter to close...");
        fflush(stdout);
        getchar();
        return st_fail ? 1 : 0;
    }

    /* ---- 1. checks: locate the game folder */
    if (!resolve_game_dir(argv[0])) {
        say(C_RED, "I can't find WowB.exe or WowB-ARM64.exe. Folders I looked in:");
        for (int i = 0; i < g_ncand; i++) say(C_RED, "  %s", g_cand[i]);
        fail("Put PlayBeta.exe inside the _classic_beta_ folder, or start it with:  PlayBeta.exe --dir \"D:\\path\\to\\_classic_beta_\"");
    }
    say(C_GRAY, "Game folder: %s", g_gameDir);
    say(C_GRAY, "Client to launch: %s (found: %s%s%s)", g_exeName,
        dir_has_file(g_gameDir, EXE_X64) ? EXE_X64 : "",
        (dir_has_file(g_gameDir, EXE_X64) && arm_name_in(g_gameDir)) ? " + " : "",
        arm_name_in(g_gameDir) ? arm_name_in(g_gameDir) : "");
    /* Version check removed: build is hardcoded as 1.60.1.70009 (FEXCore misreports the file version). */
    say(C_GRAY, "Build: hardcoded 1.60.1.70009 (version check disabled)");
    logf_("----- start (native)");

    /* ---- 2. configuration (portal) */
    if (!g_dryRun) {
        char wtfDir[MAX_PATH], wtf[MAX_PATH], base[MAX_PATH];
        snprintf(wtfDir, sizeof wtfDir, "%s\\WTF", g_gameDir);
        snprintf(wtf, sizeof wtf, "%s\\%s", wtfDir, CONFIG_NAME);
        snprintf(base, sizeof base, "%s\\Config.wtf", wtfDir);
        CreateDirectoryA(wtfDir, NULL);
        if (GetFileAttributesA(wtf) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(base) != INVALID_FILE_ATTRIBUTES) {
            if (CopyFileA(base, wtf, TRUE)) say(C_GRAY, "Created WTF\\%s from your Config.wtf", CONFIG_NAME);
        }
        ensure_portal(wtf);
        if (g_patchDefault) ensure_portal(base);
    }

    /* ---- 3. find / open the client */
    DWORD pid = 0;
    if (g_forcePid) {
        pid = g_forcePid;
        say(C_CYAN, "Using PID %lu (from --pid).", pid);
    } else {
        DWORD pids[16];
        int n = find_clients(pids, 16);
        if (n > 1) fail("Several game instances (WowB.exe / WowB-ARM64.exe) are open: close them all and start over.");
        if (n == 1) {
            pid = pids[0];
            say(C_CYAN, "The game was already open (PID %lu): attaching to it.", pid);
        } else if (g_dryRun) {
            fail("--dryrun: the game is not open.");
        } else if (!g_launch) {
            say(C_CYAN, "Configuration ready. Now open the game (I do not open it)...");
            ULONGLONG deadline = GetTickCount64() + (ULONGLONG)g_waitMin * 60000ULL;
            while (pid == 0 && GetTickCount64() < deadline) {
                Sleep(500);
                n = find_clients(pids, 16);
                if (n > 1) fail("Several game instances (WowB.exe / WowB-ARM64.exe) are open: close them all and start over.");
                if (n == 1) pid = pids[0];
            }
            if (!pid) fail("The game (WowB.exe or WowB-ARM64.exe) did not appear within %d minutes.", g_waitMin);
            say(C_CYAN, "Game detected (PID %lu): attaching.", pid);
            Sleep(2000);
        } else {
            say(C_CYAN, "Opening the game (%s)...", g_exeName);
            char cmd[MAX_PATH + 64];
            snprintf(cmd, sizeof cmd, "\"%s\" -config %s", g_exe, CONFIG_NAME);
            STARTUPINFOA si; PROCESS_INFORMATION pi;
            memset(&si, 0, sizeof si); si.cb = sizeof si; memset(&pi, 0, sizeof pi);
            if (!CreateProcessA(g_exe, cmd, NULL, NULL, FALSE, 0, NULL, g_gameDir, &si, &pi))
                fail("Could not start %s (error %lu).", g_exeName, GetLastError());
            CloseHandle(pi.hThread);
            for (int i = 0; i < 60 && pid == 0; i++) {
                Sleep(1000);
                n = find_clients(pids, 16);
                if (n >= 1) pid = pids[n - 1];
            }
            if (!pid) { DWORD ec; if (GetExitCodeProcess(pi.hProcess, &ec) && ec == STILL_ACTIVE) pid = pi.dwProcessId; }
            CloseHandle(pi.hProcess);
            if (!pid) fail("The game did not start within 60 s.");
        }
    }
    logf_("PID %lu", pid);
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) hProc = OpenProcess(0x1000, FALSE, pid); /* QUERY_LIMITED_INFORMATION */

    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(o, C_WHITE);
    printf("\n  1. Log in and enter the realm. It almost always works on the first try; if it fails (reason 24 / back to login),\n");
    printf("  2. wait for READY to appear here in green (beep) and enter again. It will no longer fail.\n");
    printf("  3. Leave this window open (minimized) while you play: it closes by itself with the game.\n\n");
    SetConsoleTextAttribute(o, C_GRAY);

    /* ---- 3b. give the game time to boot (emulation is slow); no scanning during this time */
    if (g_startDelay > 0 && !g_dryRun) {
        say(C_CYAN, "Waiting %d s for the game to finish booting before scanning (use --delay N to change)...", g_startDelay);
        ULONGLONG t0 = GetTickCount64(), until = t0 + (ULONGLONG)g_startDelay * 1000ULL, nextMsg = t0 + 15000ULL;
        while (GetTickCount64() < until) {
            if (!is_alive(hProc, pid)) {
                say(C_CYAN, "The game closed while waiting. See you next time.");
                logf_("----- end (game closed during start delay)");
                Sleep(3000);
                finish(0);
            }
            Sleep(500);
            if (GetTickCount64() >= nextMsg) {
                { ULONGLONG nowT = GetTickCount64(); say(C_DGRAY, "  ...%llu s left before scanning starts", nowT < until ? (until - nowT) / 1000ULL : 0ULL); }
                nextMsg += 15000ULL;
            }
        }
        say(C_CYAN, "Delay over: scanning now. Try to log in.");
    }

    /* ---- 4. watch and patch */
    ULONGLONG store = 0; int haveStore = 0;
    int everPatched = 0, waitingSaid = 0, passNo = 0;
    ULONGLONG lastBeat = GetTickCount64();
    if (g_diagEnabled && !g_dryRun) {
        HANDLE th = CreateThread(NULL, 0, diag_thread, (LPVOID)(ULONG_PTR)pid, 0, NULL);
        if (th) { CloseHandle(th); logf_("Diagnostic thread started (full-memory scan every 30 s while no store is held)."); }
    }
    say(C_CYAN, "Scanning has started: looking for the certificate store. The first pass can take a while on a slow/emulated system;");
    say(C_CYAN, "you will see a progress line every 10 s. Log in and enter the realm whenever you like.");
    for (;;) {
        if (!is_alive(hProc, pid)) {
            say(C_CYAN, "The game closed. See you next time.");
            logf_("----- end (game closed)");
            Sleep(3000);
            finish(0);
        }
        if (haveStore) {
            int st = store_state(pid, store);
            if (st == ST_PATCHED) { Sleep(3000); continue; }
            if (st == ST_ORIGINAL) {
                int denied;
                say(C_YELLOW, "The client restored the original key: putting mine back.");
                if (patch(pid, store, &denied) == 0) announce_ready();
                continue;
            }
            say(C_DGRAY, "The store was freed; if you connect again, I will search for it again.");
            haveStore = 0; g_storeHeld = 0;
            continue;
        }

        Store found[16];
        passNo++;
        {   /* most passes skip huge regions (fast); every g_fastEvery-th pass is a full pass */
            int fullPass = (g_fastRegion == 0) || (g_fastRegion >= g_maxRegion) || (passNo % g_fastEvery == 0);
            g_curMax = fullPass ? g_maxRegion : g_fastRegion;
        }
        int nf = find_stores(pid, found, 16);
        if (nf < 0) {
            /* could not open the process */
            say(C_RED, "%s", g_lastScan);
            if (GetLastError() == ERROR_ACCESS_DENIED)
                say(C_RED, "Access denied: the game runs elevated. Run PlayBeta.exe as administrator.");
            if (!is_alive(hProc, pid)) continue;
            Sleep(2000);
            continue;
        }
        if (nf == 1) {
            store = found[0].array; haveStore = 1; g_storeHeld = 1;
            logf_("store at 0x%llX (%s); %s", store, found[0].state == ST_PATCHED ? "patched" : "original", g_lastScan);
            if (g_dryRun) {
                say(C_CYAN, "--dryrun: store at 0x%llX, state %s. %s", store,
                    found[0].state == ST_PATCHED ? "patched" : "original", g_lastScan);
                finish(0);
            }
            if (found[0].state == ST_PATCHED) {
                say(C_GREEN, "The key was already in place.");
            } else {
                say(C_GRAY, "Certificate store found: applying the server key...");
                int denied = 0;
                if (patch(pid, store, &denied) != 0) {
                    if (denied) say(C_RED, "Access denied: the game runs elevated. Run PlayBeta.exe as administrator.");
                    haveStore = 0; g_storeHeld = 0; Sleep(2000); continue;
                }
            }
            everPatched = 1;
            announce_ready();
            continue;
        }
        if (g_dryRun) { say(C_CYAN, "--dryrun: %s", g_lastScan); finish(0); }
        if (nf > 1) {
            say(C_YELLOW, "There are %d valid stores at once: not writing until only one is left. %s", nf, g_lastScan);
        } else if (!waitingSaid) {
            say(C_DGRAY, "Waiting for the first attempt to enter the realm... (%s)", g_lastScan);
            waitingSaid = 1; lastBeat = GetTickCount64();
        } else if (GetTickCount64() - lastBeat >= 10000ULL) {
            say(C_DGRAY, "  ...still scanning: pass %d done, no store yet (%s)", passNo, g_lastScan);
            lastBeat = GetTickCount64();
        }
        /* Before the first patch: search almost without pause (the store appears while preparing the
           connection to the realm). While playing: every 15 s. */
        Sleep(everPatched ? 15000 : 100);
    }
}
