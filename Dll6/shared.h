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
inline uintptr_t GameEntitySystem=0x3083288, ViewMatrix=0x3799830, ViewMatrixView=0x80, ViewMatrixProjection=0xC0, CameraOrigin=0x28, ControllerPawn=0x6BC, IsLocalPlayerController=0x780, PawnController=0x10B0, HeroComponent=0x16B0, HeroSpawnedId=0x20, GameSceneNode=0x330, GraphControllerManager=0x9A8, MainGraphController=0xA58, AnimSequence=0x15A8, AnimSequenceStartTime=0x15AC, CollisionProperty=0x340, SceneNodeAbsOrigin=0xC8, SceneNodeAbsScale=0xE0, SceneNodeDormant=0x103, SceneNodeRenderOrigin=0x128, CollisionMins=0x40, CollisionMaxs=0x4C, OrbCollisionProperty=0x738, OrbCollisionBoundingRadius=0x60, OrbCollisionCapsuleRadius=0xAC, Glow=0x7F0, GlowColor=0x08, GlowType=0x30, GlowTeam=0x34, GlowRange=0x38, GlowRangeMin=0x3C, GlowColorOverride=0x40, GlowFlashing=0x44, GlowTime=0x48, GlowStartTime=0x4C, GlowEligible=0x50, IsGlowing=0x51, OnGlowTypeChanged=0x162F8A0, GlowBackfaceMult=0x848, GlowSetColorOverride=0x1631B30, GlowSetRange=0x1631B50, GlowSetRangeMin=0x1631B70, GlowSetType=0x1631B90, GlowSetTeam=0x1631C60, MaxHealth=0x350, Health=0x354, LifeState=0x35C, Team=0x3F3, Pos=0x1098, Velocity=0x438, AbilityComponent=0x14C8, AbilityVector=0x68, MeleeAttackState=0x12F0, ControllerPlayerData=0x8F0, PlayerDataHealthMax=0x10, NPCState=0xEF0, FadeCorpse=0xEF4, ObserverServices=0xF00, ObserverMode=0x48, ObserverTarget=0x4C, PlayerName=0x6F0;
constexpr uint32_t HandleIndexMask=0x7FFF, HandleChunkShift=9, HandleChunkMask=0x1FF, MaxEntityIndex=HandleIndexMask+1;
// Current client.dll CEntitySystem uses 0x70-byte entity identities.
constexpr uintptr_t EntityChunks=0x10, HighestEntityIndex=0x20A0, EntityChunkStride=sizeof(uintptr_t), EntityStride=0x70;
}
struct Vector3 { float x,y,z; }; struct Vector2 { float x,y; }; struct Matrix4x4 { float m[4][4]; }; struct ColorRGBA { uint8_t r,g,b,a; };
struct BoneSegment { Vector3 start; Vector3 end; };
struct PlayerData { uintptr_t entity{}; Vector3 pos; Vector3 headPos; Vector3 bodyPos; bool hasHeadBone=false; bool hasBodyBone=false; std::vector<BoneSegment> bones; float boxLeft,boxTop,boxRight,boxBottom; float modelMinZ,modelMaxZ,modelHeight; int health,maxHealth,team; float distance; std::string heroName; std::string playerName; };
struct FarmTarget { uintptr_t entity{}; Vector3 pos{}; int health{}; int maxHealth{}; uint8_t team{}; std::string className; };
struct OrbTarget { uintptr_t entity{}; Vector3 pos{}; std::string className; uint32_t handle{}; uint8_t team{}; };
extern bool autoLastHitOrbsAutoFire, autoLastHitOrbsToggleMode, autoLastHitOrbsKeyCapture, autoLastHitOrbsActive, orbAimVisibilityCheck;
extern int autoLastHitOrbsKey;
enum class AimTargetMode : int { Head = 0, Body = 1, Closest = 2 };
struct EspStatus { bool entitySystemReady=false, localPawnFound=false, heroPawnsFound=false, heroScanComplete=false; };
struct WindowSearchData { DWORD processId; HWND window; };
typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
constexpr UINT ApplyGlowMessage = WM_APP + 0x4D;
extern bool freeCam;
extern int freeCamKey;
extern bool freeCamKeyCapture;
extern float freeCamSpeed;

extern uintptr_t clientBase; extern bool menuOpen,drawEsp,drawBoxes,drawHealth,drawHealthValues,drawNames,drawDistance,drawSnaplines,drawFovCircle,drawFarmFovCircle,drawBones,farmAssist,autoLastHitOrbs,drawOrbEsp,drawSpectatorList,collisionDiagnostics,remSizedHull,glowEnabled,aimAssist,autoParry,imguiInitialized,consoleAttached; extern float fovCircleAlpha,farmFov,farmFovAlpha,snaplineAlpha; extern int aimAssistKey,farmAssistKey; extern bool aimKeyCapture,farmKeyCapture; extern Vector3 currentLocalPosition; extern bool currentLocalPositionReady; extern Vector3 currentCameraPosition; extern bool currentCameraPositionReady; extern uintptr_t currentLocalPawn; extern uint32_t currentLocalPawnHandle; extern std::mutex meleeObjectsMutex; extern std::vector<uintptr_t> meleeObjects; extern std::mutex silentAnglesMutex; extern Vector3 pendingSilentAngles; extern bool pendingSilentAnglesReady,pendingSilentAttack; extern std::mutex humanSilentMutex,creepSilentMutex,orbSilentMutex; extern Vector3 pendingHumanAngles,pendingCreepAngles,pendingOrbAngles; extern bool pendingHumanReady,pendingCreepReady,pendingOrbReady,pendingOrbAttack; extern std::mutex farmTargetsMutex; extern std::vector<FarmTarget> farmTargets; extern std::mutex orbTargetsMutex; extern std::vector<OrbTarget> orbTargets;
extern float aimFov,aimSmooth,farmAimSmooth; extern bool aimVisibilityCheck,aimToggleMode,aimToggleActive,aimToggleLastDown,farmSilentMode,humanAimTargetFound; extern ID3D11Texture2D* depthStaging; extern UINT depthWidth,depthHeight; extern DXGI_FORMAT depthFormat; extern bool depthSnapshotReady; extern int depthDiagnosticState; extern Matrix4x4 currentViewMatrix; extern bool currentViewMatrixReady;
extern bool drawCreepEsp;
extern bool drawPlayerNames;
extern bool drawTeammates;
extern bool cornerBoxes;
extern float enemyBoxColor[4], teammateBoxColor[4];
extern float enemyNameColor[4], teammateNameColor[4];
extern float enemyHealthColor[4], teammateHealthColor[4];
extern float boxThickness, cornerBoxLength;
extern volatile ULONGLONG lastSilentAttackAppliedAt;
extern volatile LONG autoOrbAttackAppliedCount;
extern bool farmToggleMode, farmToggleActive, farmToggleLastDown;
bool GetXpOrbPosition(uintptr_t, Vector3&); bool IsXpOrbAttackable(uintptr_t);
bool InitializeRuntimeOffsets();
bool InitializePatternOffsets();
extern bool runtimeOffsetsReady;
extern std::string runtimeBuildKey;
bool InitializeNativeGlow();
bool RegisterNativeGlow(uintptr_t entity);
extern bool nativeGlowReady;
std::string GetEntityDesignerName(uint32_t);
extern bool orbEntityEventsAvailable;
bool NotifyOrbEntityAdded(uint32_t); void NotifyOrbEntityRemoved(uint32_t);
void QueueOrbEntityAdded(uint32_t); void QueueOrbEntityRemoved(uint32_t);
void LoadConfig(); void SaveConfig();
// Call this from the game's StartSound hook. entityIndex is the sound source
// entity index and soundName is the emitted sound name.
void NotifyParrySound(int entityIndex, const char* soundName);
extern ID3D11Device* pDevice; extern ID3D11DeviceContext* pContext; extern ID3D11RenderTargetView* pRenderTargetView; extern HWND gameWindow; extern WNDPROC oWndProc; extern HMODULE moduleHandle; extern void** presentVTable; extern volatile LONG unloadRequested,unloadThreadStarted; extern std::mutex glowMutex,heroPawnsMutex; extern std::unordered_set<uintptr_t> registeredGlows,queuedGlows; extern EspStatus espStatus; extern std::unordered_map<uintptr_t,bool> combatVTables; extern std::vector<uintptr_t> heroVTables,heroPawns; extern HANDLE heroDiscoveryThread,glowApplyThread,farmTargetThread,stopHeroDiscoveryEvent; extern PresentFn oPresent;
template<typename T> T Read(uintptr_t address) { T value{}; if (!address) return value; __try { value=*reinterpret_cast<T*>(address); } __except(EXCEPTION_EXECUTE_HANDLER) { value=T{}; } return value; }
template<typename T> void Write(uintptr_t address,const T& value) { if (!address) return; __try { *reinterpret_cast<T*>(address)=value; } __except(EXCEPTION_EXECUTE_HANDLER) {} }
 bool WorldToScreen(const Vector3&,Vector2&,const Matrix4x4&); bool CaptureDepthSnapshot(); bool ReadDepthAt(float,float,float&); bool IsDepthBufferPopulated(); bool GetEntityBonePosition(uintptr_t,const char*,Vector3&); bool GetEntityBoneSkeleton(uintptr_t,std::vector<BoneSegment>&); bool GetAimPointScreen(const PlayerData&,float,Vector2&); bool GetAimAnglesFromScreen(float,float,Vector3&); bool IsAimPointVisible(const PlayerData&,float,float,float); bool IsWorldAimPointVisible(const Vector3&,uintptr_t=0); void AimAtClosestEnemy(const std::vector<PlayerData>&); void FarmAimAssist(const std::vector<PlayerData>&); void AutoLastHitOrbs(); void AutoParry(const std::vector<PlayerData>&); void ReleaseAimResources();
bool InstallMeleeStateMonitor(); void RemoveMeleeStateMonitor();
bool InstallSoundEventHook(); void RemoveSoundEventHook();
bool InstallModelGlowHook(); void RemoveModelGlowHook();
void UpdateRemSizedHull(); void RestoreRemSizedHull();
BOOL CALLBACK FindGameWindowCallback(HWND,LPARAM); bool HookGameWindow(); uintptr_t ResolveEntity(uint32_t); uintptr_t ResolveEntityIndex(uint32_t); uint32_t FindEntityHandle(uintptr_t); bool GetEntityPosition(uintptr_t,Vector3&); bool GetEntityRenderPosition(uintptr_t,Vector3&); void RefreshFarmTargets(); DWORD WINAPI FarmTargetWorker(LPVOID); bool GetEntityScreenBounds(uintptr_t,const Vector3&,const Matrix4x4&,float&,float&,float&,float&); std::string GetEntityClassName(uintptr_t); bool NotifyGlowTypeChanged(uintptr_t); void ApplyHeroGlow(uintptr_t); void DiscoverHeroVTables(); void RefreshHeroPawns(); DWORD WINAPI HeroDiscoveryWorker(LPVOID); DWORD WINAPI GlowApplyWorker(LPVOID); bool IsCombatEntity(uintptr_t);
void DebugEntityHandle(uint32_t);
void SetMenuOpen(bool); bool InstallInputLockHooks(); void RemoveInputLockHooks(); std::vector<PlayerData> GetPlayers(); void RenderESP(const std::vector<PlayerData>&); void RenderMenu(size_t); void RestorePresentHook(); void ShutdownOverlay(); DWORD WINAPI UnloadThread(LPVOID); void RequestUnload(); HRESULT __stdcall hkPresent(IDXGISwapChain*,UINT,UINT); LRESULT __stdcall hkWndProc(HWND,UINT,WPARAM,LPARAM); void* DetourFunc(BYTE*,const BYTE*,const int); void SetupHooks(); DWORD WINAPI InitializeThread(LPVOID);
bool InstallOrbEntityHooks(); void RemoveOrbEntityHooks();
struct UserCmdFunctionAddresses; bool InstallUserCmdHook(); bool InstallCreateMoveHook(const UserCmdFunctionAddresses&); void RemoveUserCmdHook();
extern bool aimSilentMode;  // true = silent (без движения мыши)
extern AimTargetMode aimTargetMode;
