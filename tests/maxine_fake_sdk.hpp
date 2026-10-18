#pragma once

#include <windows.h>

// Private fixture protocol, never part of the runtime adapter or release bundle.
struct MaxineFakeSnapshot {
    unsigned int allocated{}, deallocated{}, created{}, destroyed{}, loaded{}, ran{}, transferred{};
    unsigned int inputTransfers{}, outputTransfers{};
    int lastDevice{-1};
    DWORD lastThread{};
    unsigned inputFirstPixel{};
};
using MaxineFakeRead = void(__cdecl*)(MaxineFakeSnapshot*);
using MaxineFakeReset = void(__cdecl*)();
using MaxineFakeFailLoad = void(__cdecl*)(int);
enum class MaxineFakeStage : unsigned { InputTransfer, EffectRun, OutputTransfer };
using MaxineFakeFailStage = void(__cdecl*)(MaxineFakeStage, int);
using MaxineFakeBlockRun = void(__cdecl*)(HANDLE, HANDLE, HANDLE);
using MaxineFakeFillOutput = void(__cdecl*)(bool, unsigned int);

using MaxineFakeEchoInput = void(__cdecl*)(bool);
