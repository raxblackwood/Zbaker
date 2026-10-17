#include "ZBrushSDK.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const char* BRIDGE_VERSION = "1.0.0";
static HMODULE g_hModule = nullptr;

static void WriteBridgeLog(const char* message) {
    FILE* f = fopen("C:\\Users\\raman\\Desktop\\baker_bridge_log.txt", "a");
    if (f) {
        fprintf(f, "%s\n", message);
        fclose(f);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        WriteBridgeLog("[Bridge DLL] DllMain attached to process.");
    }
    return TRUE;
}

static std::wstring GetModuleDirectory() {
    wchar_t path[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(g_hModule, path, MAX_PATH) > 0) {
        wchar_t* lastSlash = wcsrchr(path, L'\\');
        if (lastSlash) {
            *lastSlash = L'\0';
        }
        return std::wstring(path);
    }
    return L"";
}

static bool FileExists(const std::wstring& path) {
    DWORD attrib = GetFileAttributesW(path.c_str());
    return (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY));
}

static std::wstring FindBakerExecutable() {
    std::wstring modDir = GetModuleDirectory();

    // 1. Same directory as DLL
    std::wstring candidate1 = modDir + L"\\UDIMBakerApp.exe";
    if (FileExists(candidate1)) return candidate1;

    // 2. Subdirectory UDIMBakerData next to DLL
    std::wstring candidate2 = modDir + L"\\UDIMBakerData\\UDIMBakerApp.exe";
    if (FileExists(candidate2)) return candidate2;

    // 3. Parent directory / build directory
    std::wstring candidate3 = modDir + L"\\..\\UDIMBakerApp.exe";
    if (FileExists(candidate3)) return candidate3;

    // 4. Default ZStartup path
    std::wstring candidate4 = L"C:\\Program Files\\Maxon ZBrush 2026\\ZStartup\\ZPlugs64\\UDIMBakerData\\UDIMBakerApp.exe";
    if (FileExists(candidate4)) return candidate4;

    // 5. Development build directories
    std::wstring candidate5 = modDir + L"\\..\\..\\build\\bin\\Release\\UDIMBakerApp.exe";
    if (FileExists(candidate5)) return candidate5;

    return L"C:\\Program Files\\Maxon ZBrush 2026\\ZStartup\\ZPlugs64\\UDIMBakerData\\UDIMBakerApp.exe";
}

// ZBrush entry point: LaunchBakerSession
ZBRUSH_API LaunchBakerSession(
    char* pData,
    double optValue,
    char* pOutputBuffer,
    double* optOutputValue
) {
    WriteBridgeLog("[Bridge] LaunchBakerSession called by ZBrush!");

    std::wstring exePath = FindBakerExecutable();
    std::wstring commandLine = L"\"" + exePath + L"\"";

    if (pData && pData[0] != '\0') {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, pData, -1, nullptr, 0);
        if (wlen > 0) {
            std::vector<wchar_t> wbuf(wlen);
            MultiByteToWideChar(CP_UTF8, 0, pData, -1, wbuf.data(), wlen);
            commandLine += L" \"";
            commandLine += wbuf.data();
            commandLine += L"\"";
        }
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> cmdVec(commandLine.begin(), commandLine.end());
    cmdVec.push_back(L'\0');

    // Working directory set to the folder where UDIMBakerApp.exe resides
    std::wstring workDir = exePath;
    wchar_t* lastSlash = wcsrchr(&workDir[0], L'\\');
    if (lastSlash) {
        *lastSlash = L'\0';
    }

    BOOL success = CreateProcessW(
        nullptr,
        cmdVec.data(),
        nullptr,
        nullptr,
        FALSE,
        0, // Standard GUI process
        nullptr,
        workDir.c_str(),
        &si,
        &pi
    );

    char logBuf[512];
    snprintf(logBuf, sizeof(logBuf), "[Bridge] CreateProcess result: %d (PID: %lu, LastError: %lu)",
             success, pi.dwProcessId, success ? 0 : GetLastError());
    WriteBridgeLog(logBuf);

    if (success) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (pOutputBuffer) {
            snprintf(pOutputBuffer, 256, "UDIM Baker launched (PID: %lu)", pi.dwProcessId);
        }
        if (optOutputValue) {
            *optOutputValue = static_cast<double>(pi.dwProcessId);
        }
        return 1.0f;
    } else {
        DWORD err = GetLastError();
        if (pOutputBuffer) {
            snprintf(pOutputBuffer, 256, "Failed to launch UDIMBakerApp (Error %lu)", err);
        }
        if (optOutputValue) {
            *optOutputValue = -1.0;
        }
        return 0.0f;
    }
}

// ZBrush entry point: GetBridgeVersion
ZBRUSH_API GetBridgeVersion(
    char* pData,
    double optValue,
    char* pOutputBuffer,
    double* optOutputValue
) {
    WriteBridgeLog("[Bridge] GetBridgeVersion called by ZBrush!");
    if (pOutputBuffer) {
        snprintf(pOutputBuffer, 256, "%s", BRIDGE_VERSION);
    }
    if (optOutputValue) {
        *optOutputValue = 1.0;
    }
    return 1.0f;
}
