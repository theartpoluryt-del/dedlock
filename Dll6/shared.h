#pragma once
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
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
inline uintptr_t GameEntitySystem=0x37D2DC8, ViewMatrix=0x37FDA40, ViewMatrixView=0x00, ViewMatrixProjection=0x40, CameraOrigin=0xC0, ControllerPawn=0x6BC, IsLocalPlayerController=0x780, PawnController=0x10B0, HeroComponent=0x16B8, HeroSpawnedId=0x20, GameSceneNode=0x330, GraphControllerManager=0x9A8, MainGraphController=0xA58, AnimSequence=0x15A8, AnimSequenceStartTime=0x15AC, CollisionProperty=0x340, SceneNodeAbsOrigin=0xC8, SceneNodeAbsScale=0xE0, SceneNodeDormant=0x103, SceneNodeRenderOrigin=0x128, CollisionMins=0x40, CollisionMaxs=0x4C, OrbCollisionProperty=0x738, OrbCollisionBoundingRadius=0x60, OrbCollisionCapsuleRadius=0xAC, Glow=0x7F0, GlowColor=0x08, GlowType=0x30, GlowTeam=0x34, GlowRange=0x38, GlowRangeMin=0x3C, GlowColorOverride=0x40, GlowFlashing=0x44, GlowTime=0x48, GlowStartTime=0x4C, GlowEligible=0x50, IsGlowing=0x51, OnGlowTypeChanged=0x162F8A0, GlowBackfaceMult=0x848, GlowSetColorOverride=0x1631B30, GlowSetRange=0x1631B50, GlowSetRangeMin=0x1631B70, GlowSetType=0x1631B90, GlowSetTeam=0x1631C60, MaxHealth=0x350, Health=0x354, LifeState=0x35C, Team=0x3F3, Pos=0x1098, Velocity=0x438, AbilityComponent=0x14D0, AbilityVector=0x68, AbilityCooldownStart=0x764, AbilityCooldownEnd=0x768, AbilityUpgradeInfo=0x758, AbilitySlot=0x778, MeleeAttackState=0x12F0, ControllerPlayerData=0x8F0, PlayerDataHealthMax=0x10, PlayerDataAbilityUpgradeStates=0x2B0, AbilityUpgradeStateInfo=0x34, NPCState=0xEF0, FadeCorpse=0xEF4, ObserverServices=0xF00, ObserverMode=0x48, ObserverTarget=0x4C, ObserverTargetCameraPos=0x100, PlayerName=0x6F0, TimeRevealedOnMinimapByNPC=0x131C, GameRulesProxyRules=0x5F0, GameRulesGamePaused=0x38, GameRulesServerPaused=0x9F80, GameRulesMinimapMins=0x84, GameRulesMinimapMaxs=0x90, CitadelTeamFOWEntities=0x6C8, TeamFOWEntIndex=0x30, TeamFOWTeam=0x34, TeamFOWClass=0x38, TeamFOWVisibleOnMap=0x41, TeamFOWTickHidden=0x44, TeamFOWEntityName=0x48, TeamFOWHealthPercent=0x50, TeamFOWPositionX=0x51, TeamFOWPositionY=0x52, TeamFOWEntitySize=0x60;
constexpr uint32_t HandleIndexMask=0x7FFF, HandleChunkShift=9, HandleChunkMask=0x1FF, MaxEntityIndex=HandleIndexMask+1;
// Current client.dll CEntitySystem uses 0x70-byte entity identities.
inline uintptr_t EntityChunks=0x10, HighestEntityIndex=0x20A0,
    EntityChunkStride=sizeof(uintptr_t), EntityStride=0x70,
    EntityHandleOffset=0x10;
}
struct Vector3 { float x,y,z; }; struct Vector2 { float x,y; }; struct Matrix4x4 { float m[4][4]; }; struct ColorRGBA { uint8_t r,g,b,a; };
struct BoneSegment { Vector3 start; Vector3 end; };
struct AbilityEspData { int level{}; float cooldown{}; float cooldownDuration{}; bool valid{}; };
struct PlayerData { uintptr_t entity{}; Vector3 pos; Vector3 worldPos; Vector3 visualAnchor; Vector3 headPos; Vector3 neckPos; Vector3 bodyPos; Vector3 leftArmPos; Vector3 rightArmPos; Vector3 leftLegPos; Vector3 rightLegPos; bool hasVisualAnchor=false; bool hasHeadBone=false; bool hasNeckBone=false; bool hasBodyBone=false; bool hasLeftArmBone=false; bool hasRightArmBone=false; bool hasLeftLegBone=false; bool hasRightLegBone=false; std::vector<BoneSegment> bones; float boxLeft,boxTop,boxRight,boxBottom; float modelMinZ,modelMaxZ,modelHeight; int health,maxHealth,team; float distance; std::array<AbilityEspData,4> abilities{}; std::string heroName; std::string playerName; };
struct FarmTarget { uintptr_t entity{}; Vector3 pos{}; int health{}; int maxHealth{}; uint8_t team{}; std::string className; };
struct OrbTarget { uintptr_t entity{}; Vector3 pos{}; std::string className; uint32_t handle{}; uint8_t team{}; };
struct WorldEspTarget { uintptr_t entity{}; Vector3 pos{}; std::string className; std::string designerName; };
struct CampTimerData {
    uint64_t id{};
    Vector3 pos{};
    float respawnAt{};
    float respawnSeconds{};
    uint32_t campClass{};
    uint8_t mapX{};
    uint8_t mapY{};
    uint8_t localTeam{};
    bool occupied{};
};
extern bool autoLastHitOrbsAutoFire, autoLastHitOrbsToggleMode, autoLastHitOrbsKeyCapture, autoLastHitOrbsActive, orbAimVisibilityCheck;
extern int autoLastHitOrbsKey;
constexpr int AimBoneHead = 1 << 0;
constexpr int AimBoneNeck = 1 << 1;
constexpr int AimBoneTorso = 1 << 2;
constexpr int AimBoneArms = 1 << 3;
constexpr int AimBoneLegs = 1 << 4;
constexpr int AimBoneAll = AimBoneHead | AimBoneNeck | AimBoneTorso |
                           AimBoneArms | AimBoneLegs;
enum class AimSelectionMode : int { Crosshair = 0, Distance = 1, Health = 2 };
struct EspStatus { bool entitySystemReady=false, localPawnFound=false, heroPawnsFound=false, heroScanComplete=false; };
struct WindowSearchData { DWORD processId; HWND window; };
typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
constexpr UINT ApplyGlowMessage = WM_APP + 0x4D;
constexpr UINT PanoramaPreviewUiMessage = WM_APP + 0x4E;
constexpr UINT PanoramaPreviewGlowMessage = WM_APP + 0x4F;
extern bool freeCam;
extern bool freeCamActive;
extern int freeCamKey;
extern bool freeCamKeyCapture;
extern float freeCamSpeed;
extern bool movementDiagnostics;
extern std::atomic<unsigned long long> movementProcessCalls;
extern std::atomic<unsigned long long> movementCorrectionCalls;
extern std::atomic<unsigned long long> userCmdNetworkCalls;
extern std::atomic<float> movementDiagCameraYaw;
extern std::atomic<float> movementDiagMovementYaw;
extern std::atomic<float> movementDiagRawForward;
extern std::atomic<float> movementDiagRawLeft;
extern std::atomic<float> movementDiagResultForward;
extern std::atomic<float> movementDiagResultLeft;
extern std::atomic<float> movementDiagAfterForward;
extern std::atomic<float> movementDiagAfterLeft;
extern std::atomic<float> movementDiagAfterYaw;
extern std::atomic<unsigned long long> wishDirectionCalls;
extern std::atomic<unsigned long long> wishDirectionCorrectionCalls;

extern uintptr_t clientBase; extern bool menuOpen,drawEsp,drawBoxes,drawHealth,drawHealthValues,drawNames,drawDistance,drawSnaplines,drawFovCircle,drawFarmFovCircle,drawBones,farmAssist,autoLastHitOrbs,drawOrbEsp,drawSpectatorList,collisionDiagnostics,remSizedHull,glowEnabled,aimAssist,autoParry,imguiInitialized,consoleAttached; extern float fovCircleAlpha,farmFov,farmFovAlpha,snaplineAlpha; extern int aimAssistKey,farmAssistKey; extern bool aimKeyCapture,farmKeyCapture; extern Vector3 currentLocalPosition; extern bool currentLocalPositionReady; extern Vector3 currentCameraPosition; extern bool currentCameraPositionReady; extern uintptr_t currentLocalPawn; extern uint32_t currentLocalPawnHandle; extern std::mutex meleeObjectsMutex; extern std::vector<uintptr_t> meleeObjects; extern std::mutex silentAnglesMutex; extern Vector3 pendingSilentAngles; extern bool pendingSilentAnglesReady,pendingSilentAttack; extern std::mutex humanSilentMutex,creepSilentMutex,orbSilentMutex; extern Vector3 pendingHumanAngles,pendingCreepAngles,pendingOrbAngles; extern bool pendingHumanReady,pendingCreepReady,pendingOrbReady,pendingOrbAttack; extern std::mutex farmTargetsMutex; extern std::vector<FarmTarget> farmTargets; extern std::mutex orbTargetsMutex; extern std::vector<OrbTarget> orbTargets; extern std::mutex worldEspTargetsMutex; extern std::vector<WorldEspTarget> worldEspTargets;
extern std::mutex movementDebugTargetMutex; extern Vector3 movementDebugTarget; extern bool movementDebugTargetReady;
extern std::mutex movementDebugWishMutex; extern Vector3 movementDebugWishDirection; extern bool movementDebugWishReady;
bool GetCurrentCameraForward(Vector3& forward);
extern float aimFov,aimSmooth,farmAimSmooth; extern bool aimVisibilityCheck,aimToggleMode,aimToggleActive,aimToggleLastDown,farmSilentMode,farmMixedMode,humanAimTargetFound; extern ID3D11Texture2D* depthStaging; extern UINT depthWidth,depthHeight; extern DXGI_FORMAT depthFormat; extern bool depthSnapshotReady; extern int depthDiagnosticState; extern Matrix4x4 currentViewMatrix; extern bool currentViewMatrixReady;
extern bool aimMixedMode, aimOnlyYaw, aimLockTarget, aimPrediction;
extern bool antiFrog;
extern float antiFrogHsThreshold;
extern bool aimNormalActive, farmNormalActive;
extern float aimHitchance, aimPitchSmooth, aimYawSmooth;
extern AimSelectionMode aimSelectionMode;
extern bool drawCreepEsp;
extern bool drawPlayerNames;
extern bool drawTeammates;
extern bool cornerBoxes;
extern bool enemyEspEnabled, allyEspEnabled;
extern bool enemyAbilitiesEnabled, allyAbilitiesEnabled, creepAbilitiesEnabled;
extern bool enemyBoxesEnabled, allyBoxesEnabled;
extern bool enemyCornerBoxesEnabled, allyCornerBoxesEnabled;
extern bool enemyHealthEnabled, allyHealthEnabled;
extern bool enemyHealthValuesEnabled, allyHealthValuesEnabled;
extern bool enemyNamesEnabled, allyNamesEnabled;
extern bool enemyPlayerNamesEnabled, allyPlayerNamesEnabled;
extern bool enemyDistanceEnabled, allyDistanceEnabled;
extern bool enemySnaplinesEnabled, allySnaplinesEnabled;
extern bool enemyBonesEnabled, allyBonesEnabled;
extern float enemyBoxColor[4], teammateBoxColor[4];
extern float enemyNameColor[4], teammateNameColor[4];
extern float enemySkeletonColor[4], teammateSkeletonColor[4];
extern float enemyHealthColor[4], teammateHealthColor[4];
extern float enemyPlayerNameColor[4], teammatePlayerNameColor[4];
extern float enemyHealthBarColor[4], teammateHealthBarColor[4];
extern float enemyHealthValueColor[4], teammateHealthValueColor[4];
extern float enemyGlowColor[4], teammateGlowColor[4];
extern bool enemyGlowEnabled, allyGlowEnabled;
extern bool enemyRadarEnabled;
extern int enemyGlowMode, allyGlowMode;
extern bool enemyChamsEnabled, allyChamsEnabled;
extern float enemyChamsColor[4], allyChamsColor[4];
extern bool enemyInvisibleChamsEnabled, allyInvisibleChamsEnabled;
extern float enemyInvisibleChamsColor[4], allyInvisibleChamsColor[4];
extern bool creepEspEnabled, neutralCreepEspEnabled, creepBoxesEnabled, creepCornerBoxesEnabled;
extern bool creepHealthEnabled, creepHealthValuesEnabled, creepDistanceEnabled;
extern float creepBoxColor[4], creepHealthColor[4], creepHealthValueColor[4];
extern bool allyCreepEspEnabled, allyCreepBoxesEnabled, allyCreepCornerBoxesEnabled;
extern bool allyCreepHealthEnabled, allyCreepHealthValuesEnabled, allyCreepDistanceEnabled;
extern float allyCreepBoxColor[4], allyCreepHealthColor[4], allyCreepHealthValueColor[4];
extern float boxThickness, cornerBoxLength;
extern float enemyEspMaxDistance, allyEspMaxDistance, creepEspMaxDistance, orbEspMaxDistance;
extern bool powerupEspEnabled;
extern bool enemyTrooperChams, allyTrooperChams, neutralChams;
extern float enemyTrooperChamsColor[4], allyTrooperChamsColor[4], neutralChamsColor[4];
extern bool fovChangerEnabled, overrideScopeFov;
extern float cameraFov, scopedCameraFov;
extern bool worldModulationEnabled, disableSkybox;
extern float skyboxColor[4], skyboxBrightness, propsColor[4], lightColor[4];
extern float lightBrightness, worldColor[4];
extern bool talonEspEnabled, campTimersEnabled, campTimersOnScreen, campTimersOnMinimap;
extern float talonEspColor[4], campTimerColor[4], campTimerMinimapSize;
extern std::mutex campTimersMutex; extern std::vector<CampTimerData> campTimers;
extern volatile ULONGLONG lastSilentAttackAppliedAt;
extern volatile LONG autoOrbAttackAppliedCount;
extern bool farmToggleMode, farmToggleActive, farmToggleLastDown;
bool GetXpOrbPosition(uintptr_t, Vector3&); bool IsXpOrbAttackable(uintptr_t, uint32_t = 0);
bool IsXpOrbAlive(uintptr_t, uint32_t);
bool InitializeRuntimeOffsets();
bool InitializePatternOffsets();
uintptr_t FindUniqueClientPattern(const char* pattern);
uintptr_t ResolveRuntimeSchemaOffset(const char* className,
                                     const char* fieldName);
bool WriteResolvedOffsetSnapshot();
extern bool runtimeOffsetsReady;
extern std::string runtimeBuildKey;
bool InitializeNativeGlow();
bool RegisterNativeGlow(uintptr_t entity);
bool RegisterNativeTrooperGlow(uintptr_t entity);
bool RegisterNativePreviewGlow(uintptr_t entity);
extern bool nativeGlowReady;
std::string GetEntityDesignerName(uint32_t);
extern bool orbEntityEventsAvailable;
bool NotifyOrbEntityAdded(uint32_t); void NotifyOrbEntityRemoved(uint32_t);
void QueueOrbEntityAdded(uint32_t); void QueueOrbEntityRemoved(uint32_t);
void LoadConfig(); void SaveConfig();
// Call this from the game's StartSound hook. entityIndex is the sound source
// entity index and soundName is the emitted sound name.
void NotifyParrySound(int entityIndex, const char* soundName);
void ApplyCurrentCameraAim(const Vector3& worldTarget);
void ApplyHeroScriptCameraAim(const Vector3& worldTarget,
                              float pitchSmooth, float yawSmooth);
void QueueHeroSilentAngles(const Vector3& angles, bool attack);
void ClearHeroSilentAngles();
void FlushCurrentCameraAim();
void UpdateVisibleAimCamera();
extern ID3D11Device* pDevice; extern ID3D11DeviceContext* pContext; extern ID3D11RenderTargetView* pRenderTargetView; extern HWND gameWindow; extern WNDPROC oWndProc; extern HMODULE moduleHandle; extern void** presentVTable; extern volatile LONG unloadRequested,unloadThreadStarted; extern std::mutex glowMutex,heroPawnsMutex; extern std::unordered_set<uintptr_t> registeredGlows,queuedGlows; extern EspStatus espStatus; extern std::unordered_map<uintptr_t,bool> combatVTables; extern std::vector<uintptr_t> heroVTables,heroPawns; extern HANDLE heroDiscoveryThread,glowApplyThread,farmTargetThread,stopHeroDiscoveryEvent; extern PresentFn oPresent;
template<typename T> T Read(uintptr_t address) { T value{}; if (!address) return value; __try { value=*reinterpret_cast<T*>(address); } __except(EXCEPTION_EXECUTE_HANDLER) { value=T{}; } return value; }
template<typename T> void Write(uintptr_t address,const T& value) { if (!address) return; __try { *reinterpret_cast<T*>(address)=value; } __except(EXCEPTION_EXECUTE_HANDLER) {} }
 bool WorldToScreen(const Vector3&,Vector2&,const Matrix4x4&); void ArmGameDepthCapture(); void TrackGameDepthStencil(ID3D11DepthStencilView*); bool CaptureDepthSnapshot(); bool ReadDepthAt(float,float,float&); bool IsDepthBufferPopulated(); bool GetEntityBonePosition(uintptr_t,const char*,Vector3&); bool GetEntityBoneSkeleton(uintptr_t,std::vector<BoneSegment>&); bool GetEntityPreviewSkeleton(uintptr_t,std::array<Vector3,18>&,std::array<bool,18>&); bool GetAimPointScreen(const PlayerData&,float,Vector2&); bool GetAimAnglesFromScreen(float,float,Vector3&); bool IsAimPointVisible(const PlayerData&,float,float,float); bool IsWorldAimPointVisible(const Vector3&,uintptr_t=0); void ProcessAimVisibilityTraces(); void AimAtClosestEnemy(const std::vector<PlayerData>&); void FarmAimAssist(const std::vector<PlayerData>&); void AutoLastHitOrbs(); void AutoParry(const std::vector<PlayerData>&); void ReleaseAimResources();
Vector3 PredictPlayerAimPoint(uintptr_t target, const Vector3& point,
                              const Vector3& targetOrigin);
void NotifyAntiFrogDamage(int attackerEntityIndex, int victimEntityIndex,
                          int hitgroupId);
void ResetAntiFrogStats();
float GetAntiFrogHeadshotPercent();
const char* GetAntiFrogSlotLabel();
bool InstallMeleeStateMonitor(); void RemoveMeleeStateMonitor();
bool InstallSoundEventHook(); void RemoveSoundEventHook();
bool InstallModelGlowHook(); void RemoveModelGlowHook();
void UpdateRemSizedHull(); void RestoreRemSizedHull();
BOOL CALLBACK FindGameWindowCallback(HWND,LPARAM); bool HookGameWindow(); uintptr_t ResolveEntity(uint32_t); uintptr_t ResolveEntityIndex(uint32_t); uint32_t FindEntityHandle(uintptr_t); bool GetEntityPosition(uintptr_t,Vector3&); bool GetEntityRenderPosition(uintptr_t,Vector3&); bool GetEntityRenderTransformPosition(uintptr_t,Vector3&); void RefreshFarmTargets(); DWORD WINAPI FarmTargetWorker(LPVOID); bool GetEntityScreenBounds(uintptr_t,const Vector3&,const Matrix4x4&,float&,float&,float&,float&); std::string GetEntityClassName(uintptr_t); bool NotifyGlowTypeChanged(uintptr_t); void ApplyHeroGlow(uintptr_t); void ApplyTrooperGlow(uintptr_t); void DiscoverHeroVTables(); void RefreshHeroPawns(); DWORD WINAPI HeroDiscoveryWorker(LPVOID); DWORD WINAPI GlowApplyWorker(LPVOID); bool IsCombatEntity(uintptr_t);
void ApplyEnemyRadar(bool enabled);
void UpdateWorldVisuals();
void RestoreWorldVisuals();
void RestoreWorldRenderState();
void DebugEntityHandle(uint32_t);
float GetClientGameTime();
float GetCampGameTime();
void SetMenuOpen(bool); bool InstallInputLockHooks(); void RemoveInputLockHooks(); std::vector<PlayerData> GetPlayers(); void RenderESP(const std::vector<PlayerData>&); void RenderMenu(size_t); void RestorePresentHook(); void ShutdownOverlay(); DWORD WINAPI UnloadThread(LPVOID); void RequestUnload(); HRESULT __stdcall hkPresent(IDXGISwapChain*,UINT,UINT); LRESULT __stdcall hkWndProc(HWND,UINT,WPARAM,LPARAM); void* DetourFunc(BYTE*,const BYTE*,const int); void SetupHooks(); DWORD WINAPI InitializeThread(LPVOID);
bool InstallOrbEntityHooks(); void RemoveOrbEntityHooks();
struct UserCmdFunctionAddresses; bool InstallUserCmdHook(); bool InstallCreateMoveHook(const UserCmdFunctionAddresses&); void RemoveUserCmdHook();
extern bool aimSilentMode;  // true = silent (без движения мыши)
extern int aimBonesMask;
extern bool aimSilentActive;
