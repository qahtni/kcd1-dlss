/* test launcher: starts KingdomCome.exe suspended, loads the Windows version.dll into it first, then lets it run */
#include <windows.h>
#include <stdio.h>
int main(void)
{
    const wchar_t *dir = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\KingdomComeDeliverance\\Bin\\Win64";
    wchar_t cmd[600] = L"\"C:\\Program Files (x86)\\Steam\\steamapps\\common\\KingdomComeDeliverance\\Bin\\Win64\\KingdomCome.exe\" -devmode";
    const wchar_t dll[] = L"C:\\Windows\\System32\\version.dll";
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi; void *mem; HANDLE th; DWORD code = 0;
    FILE *f = fopen("C:\\Users\\user\\Downloads\\KCDDLSS\\prever.log", "w");
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir, &si, &pi)) { fprintf(f, "CreateProcess failed %lu\n", GetLastError()); fclose(f); return 1; }
    mem = VirtualAllocEx(pi.hProcess, NULL, sizeof dll, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WriteProcessMemory(pi.hProcess, mem, dll, sizeof dll, NULL);
    th = CreateRemoteThread(pi.hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"), mem, 0, NULL);
    if (!th) { fprintf(f, "CreateRemoteThread failed %lu\n", GetLastError()); TerminateProcess(pi.hProcess, 1); fclose(f); return 1; }
    WaitForSingleObject(th, 15000); GetExitCodeThread(th, &code);
    fprintf(f, "pid %lu: LoadLibraryW(system version.dll) returned low32 %08lx\n", pi.dwProcessId, code);
    ResumeThread(pi.hThread); fclose(f); return 0;
}
