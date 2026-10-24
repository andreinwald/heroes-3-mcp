/*
 * GameActions.h, part of VCMI engine
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

/// Call only on the GUI thread, while holding the interface mutex.
/// z or heroId omitted selects the hero's floor and the hero on the adventure map.
std::string mcpMoveHero(int x, int y, std::optional<int> z, std::optional<int> heroId);

/// Attack with the active stack. A shot if it can shoot, otherwise a melee attack, walking up if needed.
/// Call only on the GUI thread, while holding the interface mutex.
std::string mcpAttack(int stackId);
