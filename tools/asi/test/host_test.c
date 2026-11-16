/* host_test.c: offline run of the SHIPPED plugin binary against the real game DLL, no game and no GPU.
 * Maps WHGame.dll as an image without running it (DONT_RESOLVE_DLL_REFERENCES: relocated, imports untouched, no entry
 * point), then loads <dir>\KCDDLSS.asi; its DllMain finds the game DLL by name and hook_whgame() runs its
 * whole install path on it: address lookups, byte-patch guards, vtable hooks. The test reads KCDDLSS.log and checks lines.
 * Build: x86_64-w64-mingw32-gcc -O2 -o host_test.exe host_test.c    Run: wine host_test.exe DIR WHGame.dll CHECKS.txt
 * CHECKS.txt: one check per line: "+text" must be in the log, "-text" must not, "#" comment. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    char p[1024], *log, line[512]; FILE *f; long n; int fails = 0, checks = 0; HMODULE a, w;
    if (argc < 4) { printf("usage: host_test DIR WHGame.dll CHECKS.txt\n"); return 2; }
    snprintf(p, sizeof p, "%s\\KCDDLSS.log", argv[1]); DeleteFileA(p);
    snprintf(p, sizeof p, "%s\\KCDDLSS.asi", argv[1]);
    w = LoadLibraryExA(argv[2], NULL, DONT_RESOLVE_DLL_REFERENCES);   /* first: the plugin's DllMain then finds it by name */
    if (!w) { printf("FAIL  WHGame.dll did not map (error %lu)\n", GetLastError()); return 1; }
    a = LoadLibraryA(p); if (!a) { printf("FAIL  plugin did not load (error %lu)\n", GetLastError()); return 1; }
    Sleep(argc > 4 ? atoi(argv[4]) : 9000);   /* past the plugin's 6 s startup tick: its threads must survive a game that never starts */
    snprintf(p, sizeof p, "%s\\KCDDLSS.log", argv[1]);
    f = fopen(p, "rb"); if (!f) { printf("FAIL  no KCDDLSS.log\n"); return 1; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); log = malloc(n + 1); n = (long)fread(log, 1, n, f); log[n] = 0; fclose(f);
    f = fopen(argv[3], "r"); if (!f) { printf("FAIL  no checks file\n"); return 1; }
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line); int found;
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (l < 2 || line[0] == '#') continue;
        found = strstr(log, line + 1) != NULL; checks++;
        if ((line[0] == '+') != found) { fails++; printf("FAIL  log %s: %s\n", line[0] == '+' ? "lacks" : "has", line + 1); }
    }
    fclose(f);
    printf("%s  host test: %d of %d log checks passed (%ld log bytes)\n", fails ? "FAIL" : "ok  ", checks - fails, checks, n);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), fails ? 1 : 0);
    return 0;
}
