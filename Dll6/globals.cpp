#include "shared.h"
#include <fstream>
#include <sstream>
volatile ULONGLONG lastSilentAttackAppliedAt = 0;
volatile LONG autoOrbAttackAppliedCount = 0;
uintptr_t clientBase=0; bool menuOpen=false,drawEsp=true,drawBoxes=true,drawHealth=true,drawHealthValues=true,drawNames=true,drawDistance=true,drawSnaplines=false,drawFovCircle=true,drawFarmFovCircle=false,drawBones=false,drawCreepEsp=false,farmAssist=false,autoLastHitOrbs=false,drawOrbEsp=false,glowEnabled=true,aimAssist=true,autoParry=true,imguiInitialized=false,consoleAttached=false; float aimFov=180.0f,farmFov=180.0f,aimSmooth=6.0f,fovCircleAlpha=110.0f,farmFovAlpha=110.0f,snaplineAlpha=180.0f; bool aimVisibilityCheck=true; Vector3 currentLocalPosition{}; bool currentLocalPositionReady=false; Vector3 currentCameraPosition{}; bool currentCameraPositionReady=false; uintptr_t currentLocalPawn=0; uint32_t currentLocalPawnHandle=0xFFFFFFFFu; std::mutex meleeObjectsMutex; std::vector<uintptr_t> meleeObjects; std::mutex silentAnglesMutex; Vector3 pendingSilentAngles{}; bool pendingSilentAnglesReady=false,pendingSilentAttack=false; std::mutex farmTargetsMutex; std::vector<FarmTarget> farmTargets; std::mutex orbTargetsMutex; std::vector<OrbTarget> orbTargets;
ID3D11Texture2D* depthStaging=nullptr; UINT depthWidth=0,depthHeight=0; DXGI_FORMAT depthFormat=DXGI_FORMAT_UNKNOWN; bool depthSnapshotReady=false; int depthDiagnosticState=-1; Matrix4x4 currentViewMatrix{}; bool currentViewMatrixReady=false;
ID3D11Device* pDevice=nullptr; ID3D11DeviceContext* pContext=nullptr; ID3D11RenderTargetView* pRenderTargetView=nullptr; HWND gameWindow=nullptr; WNDPROC oWndProc=nullptr; HMODULE moduleHandle=nullptr; void** presentVTable=nullptr; volatile LONG unloadRequested=0,unloadThreadStarted=0; std::mutex glowMutex,heroPawnsMutex; std::unordered_set<uintptr_t> registeredGlows,queuedGlows; EspStatus espStatus; std::unordered_map<uintptr_t,bool> combatVTables; std::vector<uintptr_t> heroVTables,heroPawns; HANDLE heroDiscoveryThread=nullptr,glowApplyThread=nullptr,farmTargetThread=nullptr,stopHeroDiscoveryEvent=nullptr; PresentFn oPresent=nullptr;
bool aimSilentMode = false;
AimTargetMode aimTargetMode = AimTargetMode::Closest;
int aimAssistKey = VK_RBUTTON;
bool aimKeyCapture = false;
int farmAssistKey = VK_XBUTTON1;
bool farmKeyCapture = false;
bool aimToggleMode = false;
bool aimToggleActive = false;
bool aimToggleLastDown = false;
bool farmToggleMode = false;
bool farmToggleActive = false;
bool farmToggleLastDown = false;
float farmAimSmooth = 6.0f;
bool farmSilentMode = false;
bool autoLastHitOrbsAutoFire = true;
bool autoLastHitOrbsToggleMode = false;
bool autoLastHitOrbsKeyCapture = false;
bool autoLastHitOrbsActive = false;
int autoLastHitOrbsKey = VK_XBUTTON2;
bool orbAimVisibilityCheck = true;
bool orbEntityEventsAvailable = false;

namespace {
std::string ConfigPath() {
    char modulePath[MAX_PATH]{};
    if (!moduleHandle || !GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH))
        return "Dll6.ini";
    std::string path(modulePath);
    const size_t separator = path.find_last_of("\\/");
    return (separator == std::string::npos ? std::string{} : path.substr(0, separator + 1)) + "Dll6.ini";
}
}

void LoadConfig() {
    std::ifstream input(ConfigPath());
    if (!input) return;
    std::string key;
    double number = 0.0;
    while (input >> key >> number) {
        const bool value = number != 0.0;
        if (key == "drawEsp") drawEsp = value;
        else if (key == "drawBoxes") drawBoxes = value;
        else if (key == "drawHealth") drawHealth = value;
        else if (key == "drawHealthValues") drawHealthValues = value;
        else if (key == "drawNames") drawNames = value;
        else if (key == "drawDistance") drawDistance = value;
        else if (key == "drawSnaplines") drawSnaplines = value;
        else if (key == "drawFovCircle") drawFovCircle = value;
        else if (key == "drawFarmFovCircle") drawFarmFovCircle = value;
        else if (key == "drawBones") drawBones = value;
        else if (key == "drawCreepEsp") drawCreepEsp = value;
        else if (key == "drawOrbEsp") drawOrbEsp = value;
        else if (key == "farmAssist") farmAssist = value;
        else if (key == "autoLastHitOrbs") autoLastHitOrbs = value;
        else if (key == "autoLastHitOrbsAutoFire") autoLastHitOrbsAutoFire = value;
        else if (key == "autoLastHitOrbsToggleMode") autoLastHitOrbsToggleMode = value;
        else if (key == "autoLastHitOrbsKey") autoLastHitOrbsKey = static_cast<int>(number);
        else if (key == "orbAimVisibilityCheck") orbAimVisibilityCheck = value;
        else if (key == "glowEnabled") glowEnabled = value;
        else if (key == "aimAssist") aimAssist = value;
        else if (key == "autoParry") autoParry = value;
        else if (key == "aimSilentMode") aimSilentMode = value;
        else if (key == "aimVisibilityCheck") aimVisibilityCheck = value;
        else if (key == "aimToggleMode") aimToggleMode = value;
        else if (key == "farmToggleMode") farmToggleMode = value;
        else if (key == "farmSilentMode") farmSilentMode = value;
        else if (key == "aimAssistKey") aimAssistKey = static_cast<int>(number);
        else if (key == "farmAssistKey") farmAssistKey = static_cast<int>(number);
        else if (key == "aimFov") aimFov = static_cast<float>(number);
        else if (key == "farmFov") farmFov = static_cast<float>(number);
        else if (key == "aimSmooth") aimSmooth = static_cast<float>(number);
        else if (key == "farmAimSmooth") farmAimSmooth = static_cast<float>(number);
        else if (key == "fovCircleAlpha") fovCircleAlpha = static_cast<float>(number);
        else if (key == "farmFovAlpha") farmFovAlpha = static_cast<float>(number);
        else if (key == "snaplineAlpha") snaplineAlpha = static_cast<float>(number);
        else if (key == "aimTargetMode") aimTargetMode = static_cast<AimTargetMode>(static_cast<int>(number));
    }
}

void SaveConfig() {
    std::ofstream output(ConfigPath(), std::ios::trunc);
    if (!output) return;
    output << "drawEsp " << drawEsp << '\n'
           << "drawBoxes " << drawBoxes << '\n'
           << "drawHealth " << drawHealth << '\n'
           << "drawHealthValues " << drawHealthValues << '\n'
           << "drawNames " << drawNames << '\n'
           << "drawDistance " << drawDistance << '\n'
           << "drawSnaplines " << drawSnaplines << '\n'
           << "drawFovCircle " << drawFovCircle << '\n'
           << "drawFarmFovCircle " << drawFarmFovCircle << '\n'
           << "drawBones " << drawBones << '\n'
           << "drawCreepEsp " << drawCreepEsp << '\n'
           << "drawOrbEsp " << drawOrbEsp << '\n'
           << "farmAssist " << farmAssist << '\n'
           << "autoLastHitOrbs " << autoLastHitOrbs << '\n'
           << "autoLastHitOrbsAutoFire " << autoLastHitOrbsAutoFire << '\n'
           << "autoLastHitOrbsToggleMode " << autoLastHitOrbsToggleMode << '\n'
           << "autoLastHitOrbsKey " << autoLastHitOrbsKey << '\n'
           << "orbAimVisibilityCheck " << orbAimVisibilityCheck << '\n'
           << "glowEnabled " << glowEnabled << '\n'
           << "aimAssist " << aimAssist << '\n'
           << "autoParry " << autoParry << '\n'
           << "aimSilentMode " << aimSilentMode << '\n'
           << "aimVisibilityCheck " << aimVisibilityCheck << '\n'
           << "aimToggleMode " << aimToggleMode << '\n'
           << "farmToggleMode " << farmToggleMode << '\n'
           << "farmSilentMode " << farmSilentMode << '\n'
           << "aimAssistKey " << aimAssistKey << '\n'
           << "farmAssistKey " << farmAssistKey << '\n'
           << "aimFov " << aimFov << '\n'
           << "farmFov " << farmFov << '\n'
           << "aimSmooth " << aimSmooth << '\n'
           << "farmAimSmooth " << farmAimSmooth << '\n'
           << "fovCircleAlpha " << fovCircleAlpha << '\n'
           << "farmFovAlpha " << farmFovAlpha << '\n'
           << "snaplineAlpha " << snaplineAlpha << '\n'
           << "aimTargetMode " << static_cast<int>(aimTargetMode) << '\n';
}
