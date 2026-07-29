#include "shared.h"
bool freeCam=false;
#include <fstream>
#include <sstream>
volatile ULONGLONG lastSilentAttackAppliedAt = 0;
volatile LONG autoOrbAttackAppliedCount = 0;
uintptr_t clientBase=0; bool menuOpen=false,drawEsp=true,drawBoxes=true,drawHealth=true,drawHealthValues=true,drawNames=true,drawDistance=true,drawSnaplines=false,drawFovCircle=true,drawFarmFovCircle=false,drawBones=false,drawCreepEsp=false,farmAssist=false,autoLastHitOrbs=false,drawOrbEsp=false,drawSpectatorList=false,collisionDiagnostics=false,remSizedHull=false,glowEnabled=true,aimAssist=true,autoParry=true,imguiInitialized=false,consoleAttached=false; float aimFov=180.0f,farmFov=180.0f,aimSmooth=6.0f,fovCircleAlpha=110.0f,farmFovAlpha=110.0f,snaplineAlpha=180.0f; bool aimVisibilityCheck=true; Vector3 currentLocalPosition{}; bool currentLocalPositionReady=false; Vector3 currentCameraPosition{}; bool currentCameraPositionReady=false; uintptr_t currentLocalPawn=0; uint32_t currentLocalPawnHandle=0xFFFFFFFFu; std::mutex meleeObjectsMutex; std::vector<uintptr_t> meleeObjects; std::mutex silentAnglesMutex; Vector3 pendingSilentAngles{}; bool pendingSilentAnglesReady=false,pendingSilentAttack=false; std::mutex humanSilentMutex,creepSilentMutex,orbSilentMutex; Vector3 pendingHumanAngles{},pendingCreepAngles{},pendingOrbAngles{}; bool pendingHumanReady=false,pendingCreepReady=false,pendingOrbReady=false,pendingOrbAttack=false; std::mutex farmTargetsMutex; std::vector<FarmTarget> farmTargets; std::mutex orbTargetsMutex; std::vector<OrbTarget> orbTargets;
ID3D11Texture2D* depthStaging=nullptr; UINT depthWidth=0,depthHeight=0; DXGI_FORMAT depthFormat=DXGI_FORMAT_UNKNOWN; bool depthSnapshotReady=false; int depthDiagnosticState=-1; Matrix4x4 currentViewMatrix{}; bool currentViewMatrixReady=false;
ID3D11Device* pDevice=nullptr; ID3D11DeviceContext* pContext=nullptr; ID3D11RenderTargetView* pRenderTargetView=nullptr; HWND gameWindow=nullptr; WNDPROC oWndProc=nullptr; HMODULE moduleHandle=nullptr; void** presentVTable=nullptr; volatile LONG unloadRequested=0,unloadThreadStarted=0; std::mutex glowMutex,heroPawnsMutex; std::unordered_set<uintptr_t> registeredGlows,queuedGlows; EspStatus espStatus; std::unordered_map<uintptr_t,bool> combatVTables; std::vector<uintptr_t> heroVTables,heroPawns; HANDLE heroDiscoveryThread=nullptr,glowApplyThread=nullptr,farmTargetThread=nullptr,stopHeroDiscoveryEvent=nullptr; PresentFn oPresent=nullptr;
bool humanAimTargetFound = false;
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
int freeCamKey = VK_F6;
float freeCamSpeed = 450.0f;
bool freeCamKeyCapture = false;
bool orbEntityEventsAvailable = false;
bool drawTeammates = false;
bool drawPlayerNames = false;
bool cornerBoxes = true;
float enemyBoxColor[4] = {0.20f, 1.00f, 0.10f, 1.00f};
float teammateBoxColor[4] = {0.20f, 0.60f, 1.00f, 1.00f};
float enemyNameColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
float teammateNameColor[4] = {0.35f, 0.75f, 1.00f, 1.00f};
float enemyHealthColor[4] = {0.20f, 1.00f, 0.25f, 1.00f};
float teammateHealthColor[4] = {0.25f, 0.65f, 1.00f, 1.00f};
float boxThickness = 1.20f;
float cornerBoxLength = 0.24f;

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
        else if (key == "drawTeammates") drawTeammates = value;
        else if (key == "drawPlayerNames") drawPlayerNames = value;
        else if (key == "cornerBoxes") cornerBoxes = value;
        else if (key == "boxThickness") boxThickness = static_cast<float>(number);
        else if (key == "cornerBoxLength") cornerBoxLength = static_cast<float>(number);
        else if (key == "enemyBoxR") enemyBoxColor[0] = static_cast<float>(number);
        else if (key == "enemyBoxG") enemyBoxColor[1] = static_cast<float>(number);
        else if (key == "enemyBoxB") enemyBoxColor[2] = static_cast<float>(number);
        else if (key == "enemyBoxA") enemyBoxColor[3] = static_cast<float>(number);
        else if (key == "teammateBoxR") teammateBoxColor[0] = static_cast<float>(number);
        else if (key == "teammateBoxG") teammateBoxColor[1] = static_cast<float>(number);
        else if (key == "teammateBoxB") teammateBoxColor[2] = static_cast<float>(number);
        else if (key == "teammateBoxA") teammateBoxColor[3] = static_cast<float>(number);
        else if (key == "enemyNameR") enemyNameColor[0] = static_cast<float>(number);
        else if (key == "enemyNameG") enemyNameColor[1] = static_cast<float>(number);
        else if (key == "enemyNameB") enemyNameColor[2] = static_cast<float>(number);
        else if (key == "enemyNameA") enemyNameColor[3] = static_cast<float>(number);
        else if (key == "teammateNameR") teammateNameColor[0] = static_cast<float>(number);
        else if (key == "teammateNameG") teammateNameColor[1] = static_cast<float>(number);
        else if (key == "teammateNameB") teammateNameColor[2] = static_cast<float>(number);
        else if (key == "teammateNameA") teammateNameColor[3] = static_cast<float>(number);
        else if (key == "enemyHealthR") enemyHealthColor[0] = static_cast<float>(number);
        else if (key == "enemyHealthG") enemyHealthColor[1] = static_cast<float>(number);
        else if (key == "enemyHealthB") enemyHealthColor[2] = static_cast<float>(number);
        else if (key == "enemyHealthA") enemyHealthColor[3] = static_cast<float>(number);
        else if (key == "teammateHealthR") teammateHealthColor[0] = static_cast<float>(number);
        else if (key == "teammateHealthG") teammateHealthColor[1] = static_cast<float>(number);
        else if (key == "teammateHealthB") teammateHealthColor[2] = static_cast<float>(number);
        else if (key == "teammateHealthA") teammateHealthColor[3] = static_cast<float>(number);
        else if (key == "drawOrbEsp") drawOrbEsp = value;
        else if (key == "drawSpectatorList") drawSpectatorList = value;
        else if (key == "freeCam") freeCam = value;
        else if (key == "freeCamKey") freeCamKey = static_cast<int>(number);
        else if (key == "freeCamSpeed") freeCamSpeed = static_cast<float>(number);
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
           << "drawTeammates " << drawTeammates << '\n'
           << "drawPlayerNames " << drawPlayerNames << '\n'
           << "cornerBoxes " << cornerBoxes << '\n'
           << "boxThickness " << boxThickness << '\n'
           << "cornerBoxLength " << cornerBoxLength << '\n'
           << "enemyBoxR " << enemyBoxColor[0] << '\n'
           << "enemyBoxG " << enemyBoxColor[1] << '\n'
           << "enemyBoxB " << enemyBoxColor[2] << '\n'
           << "enemyBoxA " << enemyBoxColor[3] << '\n'
           << "teammateBoxR " << teammateBoxColor[0] << '\n'
           << "teammateBoxG " << teammateBoxColor[1] << '\n'
           << "teammateBoxB " << teammateBoxColor[2] << '\n'
           << "teammateBoxA " << teammateBoxColor[3] << '\n'
           << "enemyNameR " << enemyNameColor[0] << '\n'
           << "enemyNameG " << enemyNameColor[1] << '\n'
           << "enemyNameB " << enemyNameColor[2] << '\n'
           << "enemyNameA " << enemyNameColor[3] << '\n'
           << "teammateNameR " << teammateNameColor[0] << '\n'
           << "teammateNameG " << teammateNameColor[1] << '\n'
           << "teammateNameB " << teammateNameColor[2] << '\n'
           << "teammateNameA " << teammateNameColor[3] << '\n'
           << "enemyHealthR " << enemyHealthColor[0] << '\n'
           << "enemyHealthG " << enemyHealthColor[1] << '\n'
           << "enemyHealthB " << enemyHealthColor[2] << '\n'
           << "enemyHealthA " << enemyHealthColor[3] << '\n'
           << "teammateHealthR " << teammateHealthColor[0] << '\n'
           << "teammateHealthG " << teammateHealthColor[1] << '\n'
           << "teammateHealthB " << teammateHealthColor[2] << '\n'
           << "teammateHealthA " << teammateHealthColor[3] << '\n'
           << "drawOrbEsp " << drawOrbEsp << '\n'
           << "drawSpectatorList " << drawSpectatorList << '\n'
           << "freeCam " << freeCam << '\n'
           << "freeCamKey " << freeCamKey << '\n'
           << "freeCamSpeed " << freeCamSpeed << '\n'
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
