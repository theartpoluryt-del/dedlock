#pragma once

#include <cstdint>
#include <vector>

#include "shared.h"

struct CUserCmd;

extern bool vindictaAutoSnipeEnabled;
extern bool hazeSleepDaggerEnabled;
extern bool shivSerratedKnivesEnabled;
extern bool bebopAbility3Enabled;
extern bool bebopAbility2AutoEnabled;
extern bool drifterAbility2Enabled;
extern bool heroScriptsShowFov;
extern bool hazePredictionDot;
extern float vindictaSnipeFov;
extern float vindictaSnipeSmoothX;
extern float vindictaSnipeSmoothY;
extern float hazeDaggerFov;
extern float hazeDaggerSmoothX;
extern float hazeDaggerSmoothY;
extern float shivKnivesFov;
extern float shivKnivesSmoothX;
extern float shivKnivesSmoothY;
extern float bebopAbility3Fov;
extern float bebopAbility3SmoothX;
extern float bebopAbility3SmoothY;
extern float drifterAbility2Fov;
extern float drifterAbility2SmoothX;
extern float drifterAbility2SmoothY;

bool HeroScriptsNeedPlayerBones();
void UpdateHeroScriptTargets(const std::vector<PlayerData>& players);
bool ProcessHeroScriptsUserCmd(CUserCmd* command, bool processInput,
                               uintptr_t input = 0);
void DrawHeroScriptsOverlay();
void ResetHeroScripts();
bool PrimaryWeaponTargetInRange(uintptr_t target);
bool PrimaryWeaponPointInRange(const Vector3& point);
