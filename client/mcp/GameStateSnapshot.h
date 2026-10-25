/*
 * GameStateSnapshot.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include <optional>
#include <string>

/// Text description of what the current human player can see.
/// Call only on the GUI thread, while holding the interface mutex.
std::string buildMcpGameState();

/// Adventure-map camera FOV: terrain, roads, rivers, visitable objects, and tiles the current hero can reach this turn. GUI thread only.
std::string buildMcpMapState();

/// Current battle: round, active stack, heroes, stacks, legal attack ids, and per-enemy target=yes|no. GUI thread only. "battle: none" when not fighting.
std::string buildMcpBattleState();

/// One owned hero: army, primary skills, secondary skills, artifacts, spellbook. Omit id for the selected hero.
std::string buildMcpHeroState(std::optional<int> heroId);

/// One visible town: army, visiting/garrison heroes, buildings, recruitable creatures.
std::string buildMcpTownState(int townId);

/// True while a battle window is playing animations or is not yet accepting a player decision.
/// False on the adventure map, and while the enemy is acting. GUI thread only.
bool mcpBattleIsBusy();
