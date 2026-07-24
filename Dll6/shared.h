#pragma once
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <psapi.h>
#include <algorithm>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <cmath>
#include <cfloat>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "psapi.lib")

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace Offsets {
// qword_18305E800 is the chunk-table root used by Deadlock's own
// CEntityHandle resolver in the current client build.
constexpr uintptr_t GameEntitySystem=0x305E800, ViewMatrix=0x3799830, ControllerPawn=0x6BC, IsLocalPlayerController=0x780, PawnController=0x10B0, GameSceneNode=0x330, GraphControllerManager=0x9A8, MainGraphController=0xA58, AnimSequence=0x15A8, AnimSequenceStartTime=0x15AC, CollisionProperty=0x340, SceneNodeAbsOrigin=0xC8, CollisionMins=0x40, CollisionMaxs=0x4C, Glow=0x7F0, GlowColor=0x08, GlowType=0x30, GlowTeam=0x34, GlowRange=0x38, GlowRangeMin=0x3C, GlowColorOverride=0x40, GlowFlashing=0x44, GlowTime=0x48, GlowStartTime=0x4C, IsEligibleForScreenHighlight=0x50, IsGlowing=0x51, OnGlowTypeChanged=0x162F8A0, MaxHealth=0x350, Health=0x354, LifeState=0x35C, Team=0x3F3, Pos=0x1098, Velocity=0x438, AbilityComponent=0x14C8, AbilityVector=0x68, MeleeAttackState=0x12F0;
constexpr uint32_t HandleIndexMask=0x7FFF, HandleChunkShift=9, HandleChunkMask=0x1FF, MaxEntityIndex=HandleIndexMask+1;
// Current client.dll CEntitySystem uses 0x70-byte entity identities.
constexpr uintptr_t EntityChunks=0x10, EntityChunkStride=sizeof(uintptr_t), EntityStride=0x70;
}
struct Vector3 { float x,y,z; }; struct Vector2 { float x,y; }; struct Matrix4x4 { float m[4][4]; }; struct ColorRGBA { uint8_t r,g,b,a; };
struct PlayerData { Vector3 pos; float boxLeft,boxTop,boxRight,boxBottom; float modelHeight; int health,maxHealth,team; float distance; };
enum class AimTargetMode : int { Head = 0, Body = 1, Closest = 2 };
struct EspStatus { bool entitySystemReady=false, localPawnFound=false, heroPawnsFound=false; };
struct WindowSearchData { DWORD processId; HWND window; };
typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
constexpr UINT ApplyGlowMessage = WM_APP + 0x4D;

extern uintptr_t clientBase; extern bool menuOpen,drawEsp,drawBoxes,drawHealth,aimAssist,autoParry,imguiInitialized,consoleAttached; extern Vector3 currentLocalPosition; extern bool currentLocalPositionReady; extern Vector3 currentCameraPosition; extern bool currentCameraPositionReady; extern uintptr_t currentLocalPawn; extern uint32_t currentLocalPawnHandle; extern std::mutex meleeObjectsMutex; extern std::vector<uintptr_t> meleeObjects; extern std::mutex silentAnglesMutex; extern Vector3 pendingSilentAngles; extern bool pendingSilentAnglesReady;
extern float aimFov,aimSmooth; extern bool aimVisibilityCheck; extern ID3D11Texture2D* depthStaging; extern UINT depthWidth,depthHeight; extern DXGI_FORMAT depthFormat; extern bool depthSnapshotReady; extern int depthDiagnosticState; extern Matrix4x4 currentViewMatrix; extern bool currentViewMatrixReady;
extern ID3D11Device* pDevice; extern ID3D11DeviceContext* pContext; extern ID3D11RenderTargetView* pRenderTargetView; extern HWND gameWindow; extern WNDPROC oWndProc; extern HMODULE moduleHandle; extern void** presentVTable; extern volatile LONG unloadRequested,unloadThreadStarted; extern std::mutex glowMutex,heroPawnsMutex; extern std::unordered_set<uintptr_t> registeredGlows,queuedGlows; extern EspStatus espStatus; extern std::unordered_map<uintptr_t,bool> combatVTables; extern std::vector<uintptr_t> heroVTables,heroPawns; extern HANDLE heroDiscoveryThread,glowApplyThread,stopHeroDiscoveryEvent; extern PresentFn oPresent;
template<typename T> T Read(uintptr_t address) { T value{}; if (!address) return value; __try { value=*reinterpret_cast<T*>(address); } __except(EXCEPTION_EXECUTE_HANDLER) { value=T{}; } return value; }
template<typename T> void Write(uintptr_t address,const T& value) { if (!address) return; __try { *reinterpret_cast<T*>(address)=value; } __except(EXCEPTION_EXECUTE_HANDLER) {} }
bool WorldToScreen(const Vector3&,Vector2&,const Matrix4x4&); bool CaptureDepthSnapshot(); bool ReadDepthAt(float,float,float&); bool IsDepthBufferPopulated(); bool GetAimPointScreen(const PlayerData&,float,Vector2&); bool IsAimPointVisible(const PlayerData&,float,float,float); void AimAtClosestEnemy(const std::vector<PlayerData>&); void AutoParry(const std::vector<PlayerData>&); void ReleaseAimResources();
bool InstallMeleeStateMonitor(); void RemoveMeleeStateMonitor();
BOOL CALLBACK FindGameWindowCallback(HWND,LPARAM); bool HookGameWindow(); uintptr_t ResolveEntity(uint32_t); uint32_t FindEntityHandle(uintptr_t); bool GetEntityPosition(uintptr_t,Vector3&); bool GetEntityScreenBounds(uintptr_t,const Vector3&,const Matrix4x4&,float&,float&,float&,float&); std::string GetEntityClassName(uintptr_t); bool NotifyGlowTypeChanged(uintptr_t); void ApplyHeroGlow(uintptr_t); void DiscoverHeroVTables(); void RefreshHeroPawns(); DWORD WINAPI HeroDiscoveryWorker(LPVOID); DWORD WINAPI GlowApplyWorker(LPVOID); bool IsCombatEntity(uintptr_t);
void DebugEntityHandle(uint32_t);
void SetMenuOpen(bool); std::vector<PlayerData> GetPlayers(); void RenderESP(const std::vector<PlayerData>&); void RenderMenu(size_t); void RestorePresentHook(); void ShutdownOverlay(); DWORD WINAPI UnloadThread(LPVOID); void RequestUnload(); HRESULT __stdcall hkPresent(IDXGISwapChain*,UINT,UINT); LRESULT __stdcall hkWndProc(HWND,UINT,WPARAM,LPARAM); void* DetourFunc(BYTE*,const BYTE*,const int); void SetupHooks(); DWORD WINAPI InitializeThread(LPVOID);
struct UserCmdFunctionAddresses; bool InstallUserCmdHook(); bool InstallCreateMoveHook(const UserCmdFunctionAddresses&); void RemoveUserCmdHook();
extern bool aimSilentMode;  // true = silent (без движения мыши)
extern AimTargetMode aimTargetMode;
