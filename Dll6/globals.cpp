#include "shared.h"
bool freeCam=false;
bool freeCamActive=false;
bool movementDiagnostics=false;
std::atomic<unsigned long long> movementProcessCalls{0};
std::atomic<unsigned long long> movementCorrectionCalls{0};
std::atomic<unsigned long long> userCmdNetworkCalls{0};
std::atomic<float> movementDiagCameraYaw{0.0f};
std::atomic<float> movementDiagMovementYaw{0.0f};
std::atomic<float> movementDiagRawForward{0.0f};
std::atomic<float> movementDiagRawLeft{0.0f};
std::atomic<float> movementDiagResultForward{0.0f};
std::atomic<float> movementDiagResultLeft{0.0f};
std::atomic<float> movementDiagAfterForward{0.0f};
std::atomic<float> movementDiagAfterLeft{0.0f};
std::atomic<float> movementDiagAfterYaw{0.0f};
std::atomic<unsigned long long> wishDirectionCalls{0};
std::atomic<unsigned long long> wishDirectionCorrectionCalls{0};
std::mutex movementDebugTargetMutex; Vector3 movementDebugTarget{}; bool movementDebugTargetReady=false;
std::mutex movementDebugWishMutex; Vector3 movementDebugWishDirection{}; bool movementDebugWishReady=false;
#include <fstream>
#include <sstream>
volatile ULONGLONG lastSilentAttackAppliedAt = 0;
volatile LONG autoOrbAttackAppliedCount = 0;
uintptr_t clientBase=0; bool menuOpen=false,drawEsp=true,drawBoxes=true,drawHealth=true,drawHealthValues=true,drawNames=true,drawDistance=true,drawSnaplines=false,drawFovCircle=true,drawFarmFovCircle=false,drawBones=false,drawCreepEsp=false,farmAssist=false,autoLastHitOrbs=false,drawOrbEsp=false,drawSpectatorList=false,collisionDiagnostics=false,remSizedHull=false,glowEnabled=true,aimAssist=true,autoParry=true,imguiInitialized=false,consoleAttached=false; float aimFov=180.0f,farmFov=180.0f,aimSmooth=6.0f,fovCircleAlpha=110.0f,farmFovAlpha=110.0f,snaplineAlpha=180.0f; bool aimVisibilityCheck=true; Vector3 currentLocalPosition{}; bool currentLocalPositionReady=false; Vector3 currentCameraPosition{}; bool currentCameraPositionReady=false; uintptr_t currentLocalPawn=0; uint32_t currentLocalPawnHandle=0xFFFFFFFFu; std::mutex meleeObjectsMutex; std::vector<uintptr_t> meleeObjects; std::mutex silentAnglesMutex; Vector3 pendingSilentAngles{}; bool pendingSilentAnglesReady=false,pendingSilentAttack=false; std::mutex humanSilentMutex,creepSilentMutex,orbSilentMutex; Vector3 pendingHumanAngles{},pendingCreepAngles{},pendingOrbAngles{}; bool pendingHumanReady=false,pendingCreepReady=false,pendingOrbReady=false,pendingOrbAttack=false; std::mutex farmTargetsMutex; std::vector<FarmTarget> farmTargets; std::mutex orbTargetsMutex; std::vector<OrbTarget> orbTargets; std::mutex worldEspTargetsMutex; std::vector<WorldEspTarget> worldEspTargets;
ID3D11Texture2D* depthStaging=nullptr; UINT depthWidth=0,depthHeight=0; DXGI_FORMAT depthFormat=DXGI_FORMAT_UNKNOWN; bool depthSnapshotReady=false; int depthDiagnosticState=-1; Matrix4x4 currentViewMatrix{}; bool currentViewMatrixReady=false;
ID3D11Device* pDevice=nullptr; ID3D11DeviceContext* pContext=nullptr; ID3D11RenderTargetView* pRenderTargetView=nullptr; HWND gameWindow=nullptr; WNDPROC oWndProc=nullptr; HMODULE moduleHandle=nullptr; void** presentVTable=nullptr; volatile LONG unloadRequested=0,unloadThreadStarted=0; std::mutex glowMutex,heroPawnsMutex; std::unordered_set<uintptr_t> registeredGlows,queuedGlows; EspStatus espStatus; std::unordered_map<uintptr_t,bool> combatVTables; std::vector<uintptr_t> heroVTables,heroPawns; HANDLE heroDiscoveryThread=nullptr,glowApplyThread=nullptr,farmTargetThread=nullptr,stopHeroDiscoveryEvent=nullptr; PresentFn oPresent=nullptr;
bool humanAimTargetFound = false;
bool aimSilentMode = false;
bool aimMixedMode = false;
bool aimNormalActive = false;
bool farmNormalActive = false;
bool aimSilentActive = false;
bool aimOnlyYaw = false;
bool aimBacktrack = false;
bool aimLockTarget = false;
float aimHitchance = 100.0f;
float aimBacktrackMs = 100.0f;
float aimPitchSmooth = 6.0f;
float aimYawSmooth = 6.0f;
AimSelectionMode aimSelectionMode = AimSelectionMode::Crosshair;
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
bool farmMixedMode = false;
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
bool enemyEspEnabled = true, allyEspEnabled = false;
bool enemyAbilitiesEnabled = true, allyAbilitiesEnabled = true, creepAbilitiesEnabled = false;
bool enemyBoxesEnabled = true, allyBoxesEnabled = true;
bool enemyCornerBoxesEnabled = true, allyCornerBoxesEnabled = true;
bool enemyHealthEnabled = true, allyHealthEnabled = true;
bool enemyHealthValuesEnabled = true, allyHealthValuesEnabled = true;
bool enemyNamesEnabled = true, allyNamesEnabled = true;
bool enemyPlayerNamesEnabled = true, allyPlayerNamesEnabled = true;
bool enemyDistanceEnabled = true, allyDistanceEnabled = true;
bool enemySnaplinesEnabled = true, allySnaplinesEnabled = true;
bool enemyBonesEnabled = true, allyBonesEnabled = true;
float enemyBoxColor[4] = {0.20f, 1.00f, 0.10f, 1.00f};
float teammateBoxColor[4] = {0.20f, 0.60f, 1.00f, 1.00f};
float enemyPlayerNameColor[4] = {0.25f, 0.85f, 1.00f, 1.00f};
float teammatePlayerNameColor[4] = {0.25f, 0.65f, 1.00f, 1.00f};
float enemyHealthBarColor[4] = {0.20f, 1.00f, 0.25f, 1.00f};
float teammateHealthBarColor[4] = {0.25f, 0.65f, 1.00f, 1.00f};
float enemyHealthValueColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
float teammateHealthValueColor[4] = {0.70f, 0.85f, 1.00f, 1.00f};
float enemyGlowColor[4] = {0.10f, 1.00f, 0.18f, 1.00f};
float teammateGlowColor[4] = {0.25f, 0.65f, 1.00f, 1.00f};
bool enemyGlowEnabled = true, allyGlowEnabled = false;
int enemyGlowMode = 0, allyGlowMode = 0;
float enemyNameColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
float teammateNameColor[4] = {0.35f, 0.75f, 1.00f, 1.00f};
float enemySkeletonColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
float teammateSkeletonColor[4] = {0.35f, 0.75f, 1.00f, 1.00f};
float enemyHealthColor[4] = {0.20f, 1.00f, 0.25f, 1.00f};
float teammateHealthColor[4] = {0.25f, 0.65f, 1.00f, 1.00f};
float boxThickness = 1.20f;
float cornerBoxLength = 0.24f;
bool creepEspEnabled = false, neutralCreepEspEnabled = false,
     creepBoxesEnabled = true, creepCornerBoxesEnabled = false;
bool creepHealthEnabled = true, creepHealthValuesEnabled = true, creepDistanceEnabled = true;
float creepBoxColor[4] = {1.00f, 0.67f, 0.05f, 1.00f};
float creepHealthColor[4] = {0.25f, 0.90f, 0.35f, 1.00f};
float creepHealthValueColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
bool allyCreepEspEnabled = false, allyCreepBoxesEnabled = true, allyCreepCornerBoxesEnabled = false;
bool allyCreepHealthEnabled = true, allyCreepHealthValuesEnabled = true, allyCreepDistanceEnabled = true;
float allyCreepBoxColor[4] = {0.25f, 0.65f, 1.00f, 1.00f};
float allyCreepHealthColor[4] = {0.25f, 0.65f, 1.00f, 1.00f};
float allyCreepHealthValueColor[4] = {0.70f, 0.85f, 1.00f, 1.00f};
float enemyEspMaxDistance = 150.0f, allyEspMaxDistance = 150.0f,
      creepEspMaxDistance = 80.0f;
bool powerupEspEnabled = false;
bool enemyTrooperChams = false, allyTrooperChams = false,
     neutralChams = false;
float enemyTrooperChamsColor[4] = {1.00f, 0.22f, 0.12f, 1.00f};
float allyTrooperChamsColor[4] = {0.20f, 0.60f, 1.00f, 1.00f};
float neutralChamsColor[4] = {1.00f, 0.72f, 0.12f, 1.00f};
bool fovChangerEnabled = false, overrideScopeFov = false;
float cameraFov = 90.0f, scopedCameraFov = 90.0f;
bool worldModulationEnabled = false, disableSkybox = false;
float skyboxColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
float skyboxBrightness = 1.0f;
float propsColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
float lightColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
float lightBrightness = 1.0f;
float worldColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
bool talonEspEnabled = false, campTimersEnabled = false,
     campTimersOnScreen = true, campTimersOnMinimap = false;
float talonEspColor[4] = {0.95f, 0.35f, 0.12f, 1.00f};
float campTimerColor[4] = {1.00f, 0.82f, 0.20f, 1.00f};
namespace {
std::string ConfigPath() {
    // Manual-map injectors do not register the image in the loader list, so
    // GetModuleFileName(moduleHandle) can fail even though DllMain received a
    // valid image base. The old relative fallback then read/wrote Dll6.ini in
    // Deadlock's process working directory instead of next to the built DLL.
    constexpr const char* StableConfigPath =
        "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\Dll6.ini";
    char modulePath[MAX_PATH]{};
    if (!moduleHandle || !GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH))
        return StableConfigPath;
    std::string path(modulePath);
    const size_t separator = path.find_last_of("\\/");
    if (separator == std::string::npos) return StableConfigPath;
    return path.substr(0, separator + 1) + "Dll6.ini";
}
}

void LoadConfig() {
    const std::string path = ConfigPath();
    std::ifstream input(path);
    if (!input) {
        // Materialize the defaults once so every later injection has a stable,
        // editable config file instead of silently loading nothing.
        SaveConfig();
        return;
    }
    std::string key;
    double number = 0.0;
    while (input >> key >> number) {
        const bool value = number != 0.0;
        if (key == "drawEsp") drawEsp = value;
        if (key == "enemyEspEnabled") enemyEspEnabled = value;
        else if (key == "allyEspEnabled") allyEspEnabled = value;
        else if (key == "enemyAbilitiesEnabled") enemyAbilitiesEnabled = value;
        else if (key == "allyAbilitiesEnabled") allyAbilitiesEnabled = value;
        else if (key == "creepAbilitiesEnabled") creepAbilitiesEnabled = value;
        else if (key == "enemyBoxesEnabled") enemyBoxesEnabled = value;
        else if (key == "allyBoxesEnabled") allyBoxesEnabled = value;
        else if (key == "enemyCornerBoxesEnabled") enemyCornerBoxesEnabled = value;
        else if (key == "allyCornerBoxesEnabled") allyCornerBoxesEnabled = value;
        if (key == "enemyHealthEnabled") enemyHealthEnabled = value;
        else if (key == "allyHealthEnabled") allyHealthEnabled = value;
        else if (key == "enemyHealthValuesEnabled") enemyHealthValuesEnabled = value;
        else if (key == "allyHealthValuesEnabled") allyHealthValuesEnabled = value;
        if (key == "enemyNamesEnabled") enemyNamesEnabled = value;
        else if (key == "allyNamesEnabled") allyNamesEnabled = value;
        else if (key == "enemyPlayerNamesEnabled") enemyPlayerNamesEnabled = value;
        else if (key == "allyPlayerNamesEnabled") allyPlayerNamesEnabled = value;
        if (key == "enemyDistanceEnabled") enemyDistanceEnabled = value;
        else if (key == "allyDistanceEnabled") allyDistanceEnabled = value;
        else if (key == "enemySnaplinesEnabled") enemySnaplinesEnabled = value;
        else if (key == "allySnaplinesEnabled") allySnaplinesEnabled = value;
        else if (key == "enemyBonesEnabled") enemyBonesEnabled = value;
        else if (key == "allyBonesEnabled") allyBonesEnabled = value;
        if (key == "drawBoxes") drawBoxes = value;
        else if (key == "drawHealth") drawHealth = value;
        else if (key == "drawHealthValues") drawHealthValues = value;
        else if (key == "drawNames") drawNames = value;
        else if (key == "drawDistance") drawDistance = value;
        else if (key == "drawSnaplines") drawSnaplines = value;
        else if (key == "drawFovCircle") drawFovCircle = value;
        else if (key == "drawFarmFovCircle") drawFarmFovCircle = value;
        else if (key == "drawBones") drawBones = value;
        else if (key == "drawCreepEsp") drawCreepEsp = value;
        else if (key == "creepEspEnabled") creepEspEnabled = value;
        else if (key == "neutralCreepEspEnabled") neutralCreepEspEnabled = value;
        else if (key == "creepBoxesEnabled") creepBoxesEnabled = value;
        else if (key == "creepCornerBoxesEnabled") creepCornerBoxesEnabled = value;
        else if (key == "creepHealthEnabled") creepHealthEnabled = value;
        else if (key == "creepHealthValuesEnabled") creepHealthValuesEnabled = value;
        else if (key == "creepDistanceEnabled") creepDistanceEnabled = value;
        else if (key == "allyCreepEspEnabled") allyCreepEspEnabled = value;
        else if (key == "allyCreepBoxesEnabled") allyCreepBoxesEnabled = value;
        else if (key == "allyCreepCornerBoxesEnabled") allyCreepCornerBoxesEnabled = value;
        else if (key == "allyCreepHealthEnabled") allyCreepHealthEnabled = value;
        else if (key == "allyCreepHealthValuesEnabled") allyCreepHealthValuesEnabled = value;
        else if (key == "allyCreepDistanceEnabled") allyCreepDistanceEnabled = value;
        else if (key == "enemyEspMaxDistance") enemyEspMaxDistance = static_cast<float>(number);
        else if (key == "allyEspMaxDistance") allyEspMaxDistance = static_cast<float>(number);
        else if (key == "creepEspMaxDistance") creepEspMaxDistance = static_cast<float>(number);
        else if (key == "powerupEspEnabled") powerupEspEnabled = value;
        // Migrate old Chams presets to the ordinary Creep ESP switches.
        else if (key == "enemyTrooperChams") creepEspEnabled = value;
        else if (key == "allyTrooperChams") allyCreepEspEnabled = value;
        else if (key == "neutralChams") neutralCreepEspEnabled = value;
        else if (key == "fovChangerEnabled") fovChangerEnabled = value;
        else if (key == "overrideScopeFov") overrideScopeFov = value;
        else if (key == "cameraFov") cameraFov = static_cast<float>(number);
        else if (key == "scopedCameraFov") scopedCameraFov = static_cast<float>(number);
        else if (key == "worldModulationEnabled") worldModulationEnabled = value;
        else if (key == "disableSkybox") disableSkybox = value;
        else if (key == "skyboxBrightness") skyboxBrightness = static_cast<float>(number);
        else if (key == "lightBrightness") lightBrightness = static_cast<float>(number);
        else if (key == "talonEspEnabled") talonEspEnabled = value;
        else if (key == "campTimersEnabled") campTimersEnabled = value;
        else if (key == "campTimersOnScreen") campTimersOnScreen = value;
        else if (key == "campTimersOnMinimap") campTimersOnMinimap = value;
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
        if (key == "enemySkeletonR") enemySkeletonColor[0] = static_cast<float>(number);
        else if (key == "enemySkeletonG") enemySkeletonColor[1] = static_cast<float>(number);
        else if (key == "enemySkeletonB") enemySkeletonColor[2] = static_cast<float>(number);
        else if (key == "enemySkeletonA") enemySkeletonColor[3] = static_cast<float>(number);
        else if (key == "teammateSkeletonR") teammateSkeletonColor[0] = static_cast<float>(number);
        else if (key == "teammateSkeletonG") teammateSkeletonColor[1] = static_cast<float>(number);
        else if (key == "teammateSkeletonB") teammateSkeletonColor[2] = static_cast<float>(number);
        else if (key == "teammateSkeletonA") teammateSkeletonColor[3] = static_cast<float>(number);
        if (key == "enemyPlayerNameR") enemyPlayerNameColor[0] = static_cast<float>(number);
        else if (key == "enemyPlayerNameG") enemyPlayerNameColor[1] = static_cast<float>(number);
        else if (key == "enemyPlayerNameB") enemyPlayerNameColor[2] = static_cast<float>(number);
        else if (key == "enemyPlayerNameA") enemyPlayerNameColor[3] = static_cast<float>(number);
        else if (key == "teammatePlayerNameR") teammatePlayerNameColor[0] = static_cast<float>(number);
        else if (key == "teammatePlayerNameG") teammatePlayerNameColor[1] = static_cast<float>(number);
        else if (key == "teammatePlayerNameB") teammatePlayerNameColor[2] = static_cast<float>(number);
        else if (key == "teammatePlayerNameA") teammatePlayerNameColor[3] = static_cast<float>(number);
        if (key == "enemyHealthBarR") enemyHealthBarColor[0] = static_cast<float>(number);
        else if (key == "enemyHealthBarG") enemyHealthBarColor[1] = static_cast<float>(number);
        else if (key == "enemyHealthBarB") enemyHealthBarColor[2] = static_cast<float>(number);
        else if (key == "enemyHealthBarA") enemyHealthBarColor[3] = static_cast<float>(number);
        else if (key == "teammateHealthBarR") teammateHealthBarColor[0] = static_cast<float>(number);
        else if (key == "teammateHealthBarG") teammateHealthBarColor[1] = static_cast<float>(number);
        else if (key == "teammateHealthBarB") teammateHealthBarColor[2] = static_cast<float>(number);
        else if (key == "teammateHealthBarA") teammateHealthBarColor[3] = static_cast<float>(number);
        else if (key == "enemyHealthValueR") enemyHealthValueColor[0] = static_cast<float>(number);
        else if (key == "enemyHealthValueG") enemyHealthValueColor[1] = static_cast<float>(number);
        else if (key == "enemyHealthValueB") enemyHealthValueColor[2] = static_cast<float>(number);
        else if (key == "enemyHealthValueA") enemyHealthValueColor[3] = static_cast<float>(number);
        else if (key == "teammateHealthValueR") teammateHealthValueColor[0] = static_cast<float>(number);
        else if (key == "teammateHealthValueG") teammateHealthValueColor[1] = static_cast<float>(number);
        else if (key == "teammateHealthValueB") teammateHealthValueColor[2] = static_cast<float>(number);
        else if (key == "teammateHealthValueA") teammateHealthValueColor[3] = static_cast<float>(number);
        if (key == "enemyHealthR") enemyHealthColor[0] = static_cast<float>(number);
        else if (key == "enemyHealthG") enemyHealthColor[1] = static_cast<float>(number);
        else if (key == "enemyHealthB") enemyHealthColor[2] = static_cast<float>(number);
        else if (key == "enemyHealthA") enemyHealthColor[3] = static_cast<float>(number);
        else if (key == "teammateHealthR") teammateHealthColor[0] = static_cast<float>(number);
        else if (key == "teammateHealthG") teammateHealthColor[1] = static_cast<float>(number);
        else if (key == "teammateHealthB") teammateHealthColor[2] = static_cast<float>(number);
        else if (key == "teammateHealthA") teammateHealthColor[3] = static_cast<float>(number);
        if (key == "enemyGlowR") enemyGlowColor[0] = static_cast<float>(number);
        else if (key == "enemyGlowG") enemyGlowColor[1] = static_cast<float>(number);
        else if (key == "enemyGlowB") enemyGlowColor[2] = static_cast<float>(number);
        else if (key == "enemyGlowA") enemyGlowColor[3] = static_cast<float>(number);
        else if (key == "teammateGlowR") teammateGlowColor[0] = static_cast<float>(number);
        else if (key == "teammateGlowG") teammateGlowColor[1] = static_cast<float>(number);
        else if (key == "teammateGlowB") teammateGlowColor[2] = static_cast<float>(number);
        else if (key == "teammateGlowA") teammateGlowColor[3] = static_cast<float>(number);
        else if (key == "enemyGlowEnabled") enemyGlowEnabled = value;
        else if (key == "allyGlowEnabled") allyGlowEnabled = value;
        if (key == "creepBoxR") creepBoxColor[0] = static_cast<float>(number);
        else if (key == "creepBoxG") creepBoxColor[1] = static_cast<float>(number);
        else if (key == "creepBoxB") creepBoxColor[2] = static_cast<float>(number);
        else if (key == "creepBoxA") creepBoxColor[3] = static_cast<float>(number);
        else if (key == "creepHealthR") creepHealthColor[0] = static_cast<float>(number);
        else if (key == "creepHealthG") creepHealthColor[1] = static_cast<float>(number);
        else if (key == "creepHealthB") creepHealthColor[2] = static_cast<float>(number);
        else if (key == "creepHealthA") creepHealthColor[3] = static_cast<float>(number);
        else if (key == "creepHealthValueR") creepHealthValueColor[0] = static_cast<float>(number);
        else if (key == "creepHealthValueG") creepHealthValueColor[1] = static_cast<float>(number);
        else if (key == "creepHealthValueB") creepHealthValueColor[2] = static_cast<float>(number);
        else if (key == "creepHealthValueA") creepHealthValueColor[3] = static_cast<float>(number);
        else if (key == "allyCreepBoxR") allyCreepBoxColor[0] = static_cast<float>(number);
        else if (key == "allyCreepBoxG") allyCreepBoxColor[1] = static_cast<float>(number);
        else if (key == "allyCreepBoxB") allyCreepBoxColor[2] = static_cast<float>(number);
        else if (key == "allyCreepBoxA") allyCreepBoxColor[3] = static_cast<float>(number);
        else if (key == "allyCreepHealthR") allyCreepHealthColor[0] = static_cast<float>(number);
        else if (key == "allyCreepHealthG") allyCreepHealthColor[1] = static_cast<float>(number);
        else if (key == "allyCreepHealthB") allyCreepHealthColor[2] = static_cast<float>(number);
        else if (key == "allyCreepHealthA") allyCreepHealthColor[3] = static_cast<float>(number);
        else if (key == "allyCreepHealthValueR") allyCreepHealthValueColor[0] = static_cast<float>(number);
        else if (key == "allyCreepHealthValueG") allyCreepHealthValueColor[1] = static_cast<float>(number);
        else if (key == "allyCreepHealthValueB") allyCreepHealthValueColor[2] = static_cast<float>(number);
        else if (key == "allyCreepHealthValueA") allyCreepHealthValueColor[3] = static_cast<float>(number);
        else if (key == "enemyTrooperChamsR") enemyTrooperChamsColor[0] = static_cast<float>(number);
        else if (key == "enemyTrooperChamsG") enemyTrooperChamsColor[1] = static_cast<float>(number);
        else if (key == "enemyTrooperChamsB") enemyTrooperChamsColor[2] = static_cast<float>(number);
        else if (key == "enemyTrooperChamsA") enemyTrooperChamsColor[3] = static_cast<float>(number);
        else if (key == "allyTrooperChamsR") allyTrooperChamsColor[0] = static_cast<float>(number);
        else if (key == "allyTrooperChamsG") allyTrooperChamsColor[1] = static_cast<float>(number);
        else if (key == "allyTrooperChamsB") allyTrooperChamsColor[2] = static_cast<float>(number);
        else if (key == "allyTrooperChamsA") allyTrooperChamsColor[3] = static_cast<float>(number);
        else if (key == "neutralChamsR") neutralChamsColor[0] = static_cast<float>(number);
        else if (key == "neutralChamsG") neutralChamsColor[1] = static_cast<float>(number);
        else if (key == "neutralChamsB") neutralChamsColor[2] = static_cast<float>(number);
        else if (key == "neutralChamsA") neutralChamsColor[3] = static_cast<float>(number);
        else if (key == "skyboxColorR") skyboxColor[0] = static_cast<float>(number);
        else if (key == "skyboxColorG") skyboxColor[1] = static_cast<float>(number);
        else if (key == "skyboxColorB") skyboxColor[2] = static_cast<float>(number);
        else if (key == "skyboxColorA") skyboxColor[3] = static_cast<float>(number);
        else if (key == "propsColorR") propsColor[0] = static_cast<float>(number);
        else if (key == "propsColorG") propsColor[1] = static_cast<float>(number);
        else if (key == "propsColorB") propsColor[2] = static_cast<float>(number);
        else if (key == "propsColorA") propsColor[3] = static_cast<float>(number);
        else if (key == "lightColorR") lightColor[0] = static_cast<float>(number);
        else if (key == "lightColorG") lightColor[1] = static_cast<float>(number);
        else if (key == "lightColorB") lightColor[2] = static_cast<float>(number);
        else if (key == "lightColorA") lightColor[3] = static_cast<float>(number);
        else if (key == "worldColorR") worldColor[0] = static_cast<float>(number);
        else if (key == "worldColorG") worldColor[1] = static_cast<float>(number);
        else if (key == "worldColorB") worldColor[2] = static_cast<float>(number);
        else if (key == "worldColorA") worldColor[3] = static_cast<float>(number);
        else if (key == "talonEspColorR") talonEspColor[0] = static_cast<float>(number);
        else if (key == "talonEspColorG") talonEspColor[1] = static_cast<float>(number);
        else if (key == "talonEspColorB") talonEspColor[2] = static_cast<float>(number);
        else if (key == "talonEspColorA") talonEspColor[3] = static_cast<float>(number);
        else if (key == "campTimerColorR") campTimerColor[0] = static_cast<float>(number);
        else if (key == "campTimerColorG") campTimerColor[1] = static_cast<float>(number);
        else if (key == "campTimerColorB") campTimerColor[2] = static_cast<float>(number);
        else if (key == "campTimerColorA") campTimerColor[3] = static_cast<float>(number);
        else if (key == "enemyGlowMode") enemyGlowMode = static_cast<int>(number);
        else if (key == "allyGlowMode") allyGlowMode = static_cast<int>(number);
        // Compatibility with older configs that had one shared mode.
        else if (key == "glowMode") {
            enemyGlowMode = static_cast<int>(number);
            allyGlowMode = static_cast<int>(number);
        }
        if (key == "drawOrbEsp") drawOrbEsp = value;
        else if (key == "drawSpectatorList") drawSpectatorList = value;
        if (key == "freeCam") freeCam = value;
        else if (key == "freeCamKey") freeCamKey = static_cast<int>(number);
        else if (key == "freeCamSpeed") freeCamSpeed = static_cast<float>(number);
        if (key == "farmAssist") farmAssist = value;
        if (key == "autoLastHitOrbs") autoLastHitOrbs = value;
        else if (key == "autoLastHitOrbsAutoFire") autoLastHitOrbsAutoFire = value;
        else if (key == "autoLastHitOrbsToggleMode") autoLastHitOrbsToggleMode = value;
        else if (key == "autoLastHitOrbsKey") autoLastHitOrbsKey = static_cast<int>(number);
        else if (key == "orbAimVisibilityCheck") orbAimVisibilityCheck = value;
        if (key == "glowEnabled") glowEnabled = value;
        if (key == "aimAssist") aimAssist = value;
        else if (key == "autoParry") autoParry = value;
        else if (key == "aimSilentMode") aimSilentMode = value;
        else if (key == "aimMixedMode") aimMixedMode = value;
        else if (key == "aimOnlyYaw") aimOnlyYaw = value;
        else if (key == "aimBacktrack") aimBacktrack = value;
        else if (key == "aimLockTarget") aimLockTarget = value;
        else if (key == "aimVisibilityCheck") aimVisibilityCheck = value;
        else if (key == "aimToggleMode") aimToggleMode = value;
        if (key == "farmToggleMode") farmToggleMode = value;
        else if (key == "farmSilentMode") farmSilentMode = value;
        else if (key == "farmMixedMode") farmMixedMode = value;
        if (key == "aimAssistKey") aimAssistKey = static_cast<int>(number);
        else if (key == "farmAssistKey") farmAssistKey = static_cast<int>(number);
        if (key == "aimFov") aimFov = static_cast<float>(number);
        else if (key == "farmFov") farmFov = static_cast<float>(number);
        else if (key == "aimSmooth") aimSmooth = static_cast<float>(number);
        else if (key == "aimHitchance") aimHitchance = static_cast<float>(number);
        else if (key == "aimBacktrackMs") aimBacktrackMs = static_cast<float>(number);
        else if (key == "aimPitchSmooth") aimPitchSmooth = static_cast<float>(number);
        else if (key == "aimYawSmooth") aimYawSmooth = static_cast<float>(number);
        else if (key == "farmAimSmooth") farmAimSmooth = static_cast<float>(number);
        else if (key == "fovCircleAlpha") fovCircleAlpha = static_cast<float>(number);
        else if (key == "farmFovAlpha") farmFovAlpha = static_cast<float>(number);
        else if (key == "snaplineAlpha") snaplineAlpha = static_cast<float>(number);
        else if (key == "aimTargetMode") aimTargetMode = static_cast<AimTargetMode>(static_cast<int>(number));
        else if (key == "aimSelectionMode") aimSelectionMode = static_cast<AimSelectionMode>(static_cast<int>(number));
    }
}

void SaveConfig() {
    std::ofstream output(ConfigPath(), std::ios::trunc);
    if (!output) return;
    output << "drawEsp " << drawEsp << '\n'
           << "enemyEspEnabled " << enemyEspEnabled << '\n'
           << "allyEspEnabled " << allyEspEnabled << '\n'
           << "enemyAbilitiesEnabled " << enemyAbilitiesEnabled << '\n'
           << "allyAbilitiesEnabled " << allyAbilitiesEnabled << '\n'
           << "creepAbilitiesEnabled " << creepAbilitiesEnabled << '\n'
           << "enemyBoxesEnabled " << enemyBoxesEnabled << '\n'
           << "allyBoxesEnabled " << allyBoxesEnabled << '\n'
           << "enemyCornerBoxesEnabled " << enemyCornerBoxesEnabled << '\n'
           << "allyCornerBoxesEnabled " << allyCornerBoxesEnabled << '\n'
           << "enemyHealthEnabled " << enemyHealthEnabled << '\n'
           << "allyHealthEnabled " << allyHealthEnabled << '\n'
           << "enemyHealthValuesEnabled " << enemyHealthValuesEnabled << '\n'
           << "allyHealthValuesEnabled " << allyHealthValuesEnabled << '\n'
           << "enemyNamesEnabled " << enemyNamesEnabled << '\n'
           << "allyNamesEnabled " << allyNamesEnabled << '\n'
           << "enemyPlayerNamesEnabled " << enemyPlayerNamesEnabled << '\n'
           << "allyPlayerNamesEnabled " << allyPlayerNamesEnabled << '\n'
           << "enemyDistanceEnabled " << enemyDistanceEnabled << '\n'
           << "allyDistanceEnabled " << allyDistanceEnabled << '\n'
           << "enemySnaplinesEnabled " << enemySnaplinesEnabled << '\n'
           << "allySnaplinesEnabled " << allySnaplinesEnabled << '\n'
           << "enemyBonesEnabled " << enemyBonesEnabled << '\n'
           << "allyBonesEnabled " << allyBonesEnabled << '\n'
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
           << "creepEspEnabled " << creepEspEnabled << '\n'
           << "neutralCreepEspEnabled " << neutralCreepEspEnabled << '\n'
           << "creepBoxesEnabled " << creepBoxesEnabled << '\n'
           << "creepCornerBoxesEnabled " << creepCornerBoxesEnabled << '\n'
           << "creepHealthEnabled " << creepHealthEnabled << '\n'
           << "creepHealthValuesEnabled " << creepHealthValuesEnabled << '\n'
           << "creepDistanceEnabled " << creepDistanceEnabled << '\n'
           << "allyCreepEspEnabled " << allyCreepEspEnabled << '\n'
           << "allyCreepBoxesEnabled " << allyCreepBoxesEnabled << '\n'
           << "allyCreepCornerBoxesEnabled " << allyCreepCornerBoxesEnabled << '\n'
           << "allyCreepHealthEnabled " << allyCreepHealthEnabled << '\n'
           << "allyCreepHealthValuesEnabled " << allyCreepHealthValuesEnabled << '\n'
           << "allyCreepDistanceEnabled " << allyCreepDistanceEnabled << '\n'
           << "enemyEspMaxDistance " << enemyEspMaxDistance << '\n'
           << "allyEspMaxDistance " << allyEspMaxDistance << '\n'
           << "creepEspMaxDistance " << creepEspMaxDistance << '\n'
           << "powerupEspEnabled " << powerupEspEnabled << '\n'
           << "enemyTrooperChams " << enemyTrooperChams << '\n'
           << "allyTrooperChams " << allyTrooperChams << '\n'
           << "neutralChams " << neutralChams << '\n'
           << "fovChangerEnabled " << fovChangerEnabled << '\n'
           << "overrideScopeFov " << overrideScopeFov << '\n'
           << "cameraFov " << cameraFov << '\n'
           << "scopedCameraFov " << scopedCameraFov << '\n'
           << "worldModulationEnabled " << worldModulationEnabled << '\n'
           << "disableSkybox " << disableSkybox << '\n'
           << "skyboxBrightness " << skyboxBrightness << '\n'
           << "lightBrightness " << lightBrightness << '\n'
           << "talonEspEnabled " << talonEspEnabled << '\n'
           << "campTimersEnabled " << campTimersEnabled << '\n'
           << "campTimersOnScreen " << campTimersOnScreen << '\n'
           << "campTimersOnMinimap " << campTimersOnMinimap << '\n'
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
            << "enemySkeletonR " << enemySkeletonColor[0] << '\n'
            << "enemySkeletonG " << enemySkeletonColor[1] << '\n'
            << "enemySkeletonB " << enemySkeletonColor[2] << '\n'
            << "enemySkeletonA " << enemySkeletonColor[3] << '\n'
            << "teammateSkeletonR " << teammateSkeletonColor[0] << '\n'
            << "teammateSkeletonG " << teammateSkeletonColor[1] << '\n'
            << "teammateSkeletonB " << teammateSkeletonColor[2] << '\n'
            << "teammateSkeletonA " << teammateSkeletonColor[3] << '\n'
           << "enemyPlayerNameR " << enemyPlayerNameColor[0] << '\n'
           << "enemyPlayerNameG " << enemyPlayerNameColor[1] << '\n'
           << "enemyPlayerNameB " << enemyPlayerNameColor[2] << '\n'
           << "enemyPlayerNameA " << enemyPlayerNameColor[3] << '\n'
           << "teammatePlayerNameR " << teammatePlayerNameColor[0] << '\n'
           << "teammatePlayerNameG " << teammatePlayerNameColor[1] << '\n'
           << "teammatePlayerNameB " << teammatePlayerNameColor[2] << '\n'
           << "teammatePlayerNameA " << teammatePlayerNameColor[3] << '\n'
           << "enemyHealthR " << enemyHealthColor[0] << '\n'
           << "enemyHealthG " << enemyHealthColor[1] << '\n'
           << "enemyHealthB " << enemyHealthColor[2] << '\n'
           << "enemyHealthA " << enemyHealthColor[3] << '\n'
           << "teammateHealthR " << teammateHealthColor[0] << '\n'
           << "teammateHealthG " << teammateHealthColor[1] << '\n'
           << "teammateHealthB " << teammateHealthColor[2] << '\n'
           << "teammateHealthA " << teammateHealthColor[3] << '\n'
           << "enemyGlowR " << enemyGlowColor[0] << '\n'
           << "enemyGlowG " << enemyGlowColor[1] << '\n'
           << "enemyGlowB " << enemyGlowColor[2] << '\n'
           << "enemyGlowA " << enemyGlowColor[3] << '\n'
           << "teammateGlowR " << teammateGlowColor[0] << '\n'
           << "teammateGlowG " << teammateGlowColor[1] << '\n'
           << "teammateGlowB " << teammateGlowColor[2] << '\n'
           << "teammateGlowA " << teammateGlowColor[3] << '\n'
           << "enemyGlowEnabled " << enemyGlowEnabled << '\n'
           << "allyGlowEnabled " << allyGlowEnabled << '\n'
           << "enemyHealthBarR " << enemyHealthBarColor[0] << '\n'
           << "enemyHealthBarG " << enemyHealthBarColor[1] << '\n'
           << "enemyHealthBarB " << enemyHealthBarColor[2] << '\n'
           << "enemyHealthBarA " << enemyHealthBarColor[3] << '\n'
           << "teammateHealthBarR " << teammateHealthBarColor[0] << '\n'
           << "teammateHealthBarG " << teammateHealthBarColor[1] << '\n'
           << "teammateHealthBarB " << teammateHealthBarColor[2] << '\n'
           << "teammateHealthBarA " << teammateHealthBarColor[3] << '\n'
           << "enemyHealthValueR " << enemyHealthValueColor[0] << '\n'
           << "enemyHealthValueG " << enemyHealthValueColor[1] << '\n'
           << "enemyHealthValueB " << enemyHealthValueColor[2] << '\n'
           << "enemyHealthValueA " << enemyHealthValueColor[3] << '\n'
           << "teammateHealthValueR " << teammateHealthValueColor[0] << '\n'
           << "teammateHealthValueG " << teammateHealthValueColor[1] << '\n'
           << "teammateHealthValueB " << teammateHealthValueColor[2] << '\n'
           << "teammateHealthValueA " << teammateHealthValueColor[3] << '\n'
           << "creepBoxR " << creepBoxColor[0] << '\n'
           << "creepBoxG " << creepBoxColor[1] << '\n'
           << "creepBoxB " << creepBoxColor[2] << '\n'
           << "creepBoxA " << creepBoxColor[3] << '\n'
           << "creepHealthR " << creepHealthColor[0] << '\n'
           << "creepHealthG " << creepHealthColor[1] << '\n'
            << "creepHealthB " << creepHealthColor[2] << '\n'
            << "creepHealthA " << creepHealthColor[3] << '\n'
            << "creepHealthValueR " << creepHealthValueColor[0] << '\n'
            << "creepHealthValueG " << creepHealthValueColor[1] << '\n'
            << "creepHealthValueB " << creepHealthValueColor[2] << '\n'
            << "creepHealthValueA " << creepHealthValueColor[3] << '\n'
           << "allyCreepBoxR " << allyCreepBoxColor[0] << '\n'
           << "allyCreepBoxG " << allyCreepBoxColor[1] << '\n'
           << "allyCreepBoxB " << allyCreepBoxColor[2] << '\n'
           << "allyCreepBoxA " << allyCreepBoxColor[3] << '\n'
           << "allyCreepHealthR " << allyCreepHealthColor[0] << '\n'
           << "allyCreepHealthG " << allyCreepHealthColor[1] << '\n'
            << "allyCreepHealthB " << allyCreepHealthColor[2] << '\n'
            << "allyCreepHealthA " << allyCreepHealthColor[3] << '\n'
            << "allyCreepHealthValueR " << allyCreepHealthValueColor[0] << '\n'
            << "allyCreepHealthValueG " << allyCreepHealthValueColor[1] << '\n'
            << "allyCreepHealthValueB " << allyCreepHealthValueColor[2] << '\n'
             << "allyCreepHealthValueA " << allyCreepHealthValueColor[3] << '\n'
             << "enemyTrooperChamsR " << enemyTrooperChamsColor[0] << '\n'
             << "enemyTrooperChamsG " << enemyTrooperChamsColor[1] << '\n'
             << "enemyTrooperChamsB " << enemyTrooperChamsColor[2] << '\n'
             << "enemyTrooperChamsA " << enemyTrooperChamsColor[3] << '\n'
             << "allyTrooperChamsR " << allyTrooperChamsColor[0] << '\n'
             << "allyTrooperChamsG " << allyTrooperChamsColor[1] << '\n'
             << "allyTrooperChamsB " << allyTrooperChamsColor[2] << '\n'
             << "allyTrooperChamsA " << allyTrooperChamsColor[3] << '\n'
             << "neutralChamsR " << neutralChamsColor[0] << '\n'
             << "neutralChamsG " << neutralChamsColor[1] << '\n'
             << "neutralChamsB " << neutralChamsColor[2] << '\n'
             << "neutralChamsA " << neutralChamsColor[3] << '\n'
             << "skyboxColorR " << skyboxColor[0] << '\n'
             << "skyboxColorG " << skyboxColor[1] << '\n'
             << "skyboxColorB " << skyboxColor[2] << '\n'
             << "skyboxColorA " << skyboxColor[3] << '\n'
             << "propsColorR " << propsColor[0] << '\n'
             << "propsColorG " << propsColor[1] << '\n'
             << "propsColorB " << propsColor[2] << '\n'
             << "propsColorA " << propsColor[3] << '\n'
             << "lightColorR " << lightColor[0] << '\n'
             << "lightColorG " << lightColor[1] << '\n'
             << "lightColorB " << lightColor[2] << '\n'
             << "lightColorA " << lightColor[3] << '\n'
             << "worldColorR " << worldColor[0] << '\n'
             << "worldColorG " << worldColor[1] << '\n'
             << "worldColorB " << worldColor[2] << '\n'
             << "worldColorA " << worldColor[3] << '\n'
             << "talonEspColorR " << talonEspColor[0] << '\n'
             << "talonEspColorG " << talonEspColor[1] << '\n'
             << "talonEspColorB " << talonEspColor[2] << '\n'
             << "talonEspColorA " << talonEspColor[3] << '\n'
             << "campTimerColorR " << campTimerColor[0] << '\n'
             << "campTimerColorG " << campTimerColor[1] << '\n'
             << "campTimerColorB " << campTimerColor[2] << '\n'
             << "campTimerColorA " << campTimerColor[3] << '\n'
           << "enemyGlowMode " << enemyGlowMode << '\n'
           << "allyGlowMode " << allyGlowMode << '\n'
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
           << "aimMixedMode " << aimMixedMode << '\n'
           << "aimOnlyYaw " << aimOnlyYaw << '\n'
           << "aimBacktrack " << aimBacktrack << '\n'
           << "aimLockTarget " << aimLockTarget << '\n'
           << "aimVisibilityCheck " << aimVisibilityCheck << '\n'
           << "aimToggleMode " << aimToggleMode << '\n'
           << "farmToggleMode " << farmToggleMode << '\n'
           << "farmSilentMode " << farmSilentMode << '\n'
           << "farmMixedMode " << farmMixedMode << '\n'
           << "aimAssistKey " << aimAssistKey << '\n'
           << "farmAssistKey " << farmAssistKey << '\n'
           << "aimFov " << aimFov << '\n'
           << "farmFov " << farmFov << '\n'
           << "aimSmooth " << aimSmooth << '\n'
           << "aimHitchance " << aimHitchance << '\n'
           << "aimBacktrackMs " << aimBacktrackMs << '\n'
           << "aimPitchSmooth " << aimPitchSmooth << '\n'
           << "aimYawSmooth " << aimYawSmooth << '\n'
           << "farmAimSmooth " << farmAimSmooth << '\n'
           << "fovCircleAlpha " << fovCircleAlpha << '\n'
           << "farmFovAlpha " << farmFovAlpha << '\n'
           << "snaplineAlpha " << snaplineAlpha << '\n'
           << "aimTargetMode " << static_cast<int>(aimTargetMode) << '\n'
           << "aimSelectionMode " << static_cast<int>(aimSelectionMode) << '\n';
}
