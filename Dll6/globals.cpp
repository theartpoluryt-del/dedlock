#include "shared.h"
uintptr_t clientBase=0; bool menuOpen=false,drawEsp=true,drawBoxes=true,drawHealth=true,drawHealthValues=true,drawNames=true,drawDistance=true,drawSnaplines=false,drawFovCircle=true,glowEnabled=true,aimAssist=true,autoParry=true,imguiInitialized=false,consoleAttached=false; float aimFov=180.0f,aimSmooth=6.0f,fovCircleAlpha=110.0f,snaplineAlpha=180.0f; bool aimVisibilityCheck=true; Vector3 currentLocalPosition{}; bool currentLocalPositionReady=false; Vector3 currentCameraPosition{}; bool currentCameraPositionReady=false; uintptr_t currentLocalPawn=0; uint32_t currentLocalPawnHandle=0xFFFFFFFFu; std::mutex meleeObjectsMutex; std::vector<uintptr_t> meleeObjects; std::mutex silentAnglesMutex; Vector3 pendingSilentAngles{}; bool pendingSilentAnglesReady=false;
ID3D11Texture2D* depthStaging=nullptr; UINT depthWidth=0,depthHeight=0; DXGI_FORMAT depthFormat=DXGI_FORMAT_UNKNOWN; bool depthSnapshotReady=false; int depthDiagnosticState=-1; Matrix4x4 currentViewMatrix{}; bool currentViewMatrixReady=false;
ID3D11Device* pDevice=nullptr; ID3D11DeviceContext* pContext=nullptr; ID3D11RenderTargetView* pRenderTargetView=nullptr; HWND gameWindow=nullptr; WNDPROC oWndProc=nullptr; HMODULE moduleHandle=nullptr; void** presentVTable=nullptr; volatile LONG unloadRequested=0,unloadThreadStarted=0; std::mutex glowMutex,heroPawnsMutex; std::unordered_set<uintptr_t> registeredGlows,queuedGlows; EspStatus espStatus; std::unordered_map<uintptr_t,bool> combatVTables; std::vector<uintptr_t> heroVTables,heroPawns; HANDLE heroDiscoveryThread=nullptr,glowApplyThread=nullptr,stopHeroDiscoveryEvent=nullptr; PresentFn oPresent=nullptr;
bool aimSilentMode = false;
AimTargetMode aimTargetMode = AimTargetMode::Closest;
int aimAssistKey = VK_RBUTTON;
bool aimKeyCapture = false;
bool aimToggleMode = false;
bool aimToggleActive = false;
bool aimToggleLastDown = false;
