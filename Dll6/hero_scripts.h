#pragma once

#include <cstdint>
#include <vector>

#include "shared.h"

struct CUserCmd;

extern bool vindictaAutoSnipeEnabled;
extern bool hazeSleepDaggerEnabled;
extern bool shivSerratedKnivesEnabled;
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

bool HeroScriptsNeedPlayerBones();
void UpdateHeroScriptTargets(const std::vector<PlayerData>& players);
bool ProcessHeroScriptsUserCmd(CUserCmd* command, bool processInput,
                               uintptr_t input = 0);
void DrawHeroScriptsOverlay();
void ResetHeroScripts();
