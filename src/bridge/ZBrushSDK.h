#pragma once

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #define ZBRUSH_API extern "C" __declspec(dllexport) float __cdecl
#else
    #define ZBRUSH_API extern "C" float
#endif

// ZBrush C-ABI Plugin Function Signature:
// float FunctionName(char* pData, double optValue, char* pOutputBuffer, double* optOutputValue);
//
// Arguments:
//   pData:          Null-terminated string passed from ZScript (input).
//   optValue:       Double numerical parameter passed from ZScript (input).
//   pOutputBuffer:  Output buffer provided by ZBrush to return string data (output).
//   optOutputValue: Pointer to double to return numerical status to ZScript (output).
// Return:
//   float: 1.0f on success, 0.0f or negative code on failure.
typedef float(__cdecl* ZBrushPluginFunc)(char* pData, double optValue, char* pOutputBuffer, double* optOutputValue);
