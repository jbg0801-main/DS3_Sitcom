#pragma once

#include <windows.h>
#include <unknwn.h>

using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

bool LoadRealDinput8(HMODULE* out_module, DirectInput8Create_t* out_create);
void UnloadRealDinput8(HMODULE module);
