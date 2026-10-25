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
#include <vector>

class CCallback;
class CPlayerBattleCallback;

namespace battle
{
class Unit;
}

/// Call only on the GUI thread, while holding the interface mutex.
/// z or heroId omitted selects the hero's floor and the hero on the adventure map.
std::string mcpMoveHero(int x, int y, std::optional<int> z, std::optional<int> heroId);

/// True when the actor has a legal shot or melee against the target. GUI thread, state mutex held.
bool mcpCanAttackTarget(CCallback & cb, const CPlayerBattleCallback & battle, const battle::Unit & actor, const battle::Unit & target);

/// Enemy stack ids the actor can legally shoot or melee. GUI thread, state mutex held.
std::vector<int> mcpLegalAttackIds(CCallback & cb, const CPlayerBattleCallback & battle, const battle::Unit & actor);

/// "attack=3,4 wait=yes defend=yes" for the active stack. GUI thread, state mutex held.
std::string mcpBattleCanFields(CCallback & cb, const CPlayerBattleCallback & battle, const battle::Unit & actor);

/// Attack with the active stack. A shot if it can shoot, otherwise a melee attack, walking up if needed.
/// Call only on the GUI thread, while holding the interface mutex.
std::string mcpAttack(int stackId);

/// Wait with the active stack so another friendly unit can act first this round.
/// Call only on the GUI thread, while holding the interface mutex.
std::string mcpBattleWait();

/// Defend with the active stack, ending its turn for this round.
/// Call only on the GUI thread, while holding the interface mutex.
std::string mcpBattleDefend();

/// Answer the open blocking dialog. choice is the option number from get_state.
/// Call only on the GUI thread, while holding the interface mutex.
std::string mcpAnswer(int choice, std::optional<int> questionOverride = std::nullopt);

/// End the current adventure turn and advance to the next day.
/// Call only on the GUI thread, while holding the interface mutex.
std::string mcpEndTurn();
