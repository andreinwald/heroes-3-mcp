/*
 * GameActions.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "GameActions.h"
#include "McpDialogs.h"

#include "CPlayerInterface.h"
#include "GameEngine.h"
#include "GameInstance.h"
#include "PlayerLocalState.h"
#include "adventureMap/AdventureMapInterface.h"

#include "../lib/ConditionalWait.h"
#include "battle/BattleInterface.h"
#include "battle/BattleStacksController.h"
#include "McpBattleWaiter.h"
#include "gui/WindowHandler.h"

#include "../lib/CStack.h"
#include "../lib/callback/CCallback.h"
#include "../lib/constants/Enumerations.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "../lib/mapObjects/MiscObjects.h"
#include "../lib/pathfinder/CGPathNode.h"
#include "../lib/battle/BattleAction.h"
#include "../lib/battle/CPlayerBattleCallback.h"
#include "../lib/battle/actions/ActionOption.h"
#include "../lib/battle/actions/BattleActionType.h"

namespace
{

std::string quote(const std::string & value)
{
	std::string escaped;
	escaped.reserve(value.size());
	for(char c : value)
	{
		if(c == '\\' || c == '"')
			escaped.push_back('\\');
		if(c == '\n' || c == '\r' || c == '\t')
			escaped.push_back(' ');
		else
			escaped.push_back(c);
	}
	return "\"" + escaped + "\"";
}

std::string field(const std::string & key, const std::string & value)
{
	bool simple = !value.empty();
	for(unsigned char c : value)
	{
		if(!std::isalnum(c) && c != ',' && c != '_' && c != '-' && c != '/' && c != '.')
		{
			simple = false;
			break;
		}
	}

	if(simple)
		return key + "=" + value;
	return key + "=" + quote(value);
}

std::string posText(const int3 & pos)
{
	return std::to_string(pos.x) + "," + std::to_string(pos.y) + "," + std::to_string(pos.z);
}

/// When the battle window is open, only its active stack may act (see BattleInterface::makingTurn).
std::string battleActorReady(const BattleID battleID, const CPlayerBattleCallback & battle, const CStack *& actorStack)
{
	std::shared_ptr<BattleInterface> battleUi = CPlayerInterface::battleInt;
	if(battleUi && battleUi->getBattleID() == battleID)
	{
		if(battleUi->hasAnimations())
			return "error: battle animations are playing";
		if(!battleUi->makingTurn())
			return "error: wait for the current battle action to finish";

		actorStack = battleUi->stacksController->getActiveStack();
		if(!actorStack || !actorStack->alive())
			return "error: no stack is ready to act";
		if(actorStack->unitSide() != battle.battleGetMySide())
			return "error: it is not your stack's turn";
		return {};
	}

	const battle::Unit * actor = battle.battleActiveUnit();
	if(!actor || !actor->alive())
		return "error: no stack is waiting to act";
	if(actor->unitSide() != battle.battleGetMySide())
		return "error: it is not your stack's turn";

	actorStack = dynamic_cast<const CStack *>(actor);
	if(!actorStack)
		return "error: active unit is not a stack";
	return {};
}

std::string creatureName(const battle::Unit & unit)
{
	const CCreature * creature = unit.unitType();
	if(!creature)
		return "stack";
	const std::string name = creature->getNamePluralTranslated();
	if(name.empty())
		return "stack";
	return name;
}

std::string heroName(const CGHeroInstance & hero)
{
	const std::string name = GAME->translator().translate(hero.getNameTextID());
	if(name.empty())
		return "Hero";
	return name;
}

const char * attackActionName(EActionType type)
{
	switch(type)
	{
		case EActionType::SHOOT:
			return "shoot";
		case EActionType::WALK_AND_ATTACK:
		case EActionType::WALK_AND_ATTACK_NO_RETURN:
		case EActionType::LONG_WEAPON_ATTACK:
			return "melee";
		default:
			return nullptr;
	}
}

bool inMap(const int3 & pos, const int3 & mapSize)
{
	return pos.x >= 0 && pos.y >= 0 && pos.z >= 0 && pos.x < mapSize.x && pos.y < mapSize.y && pos.z < mapSize.z;
}

const CGHeroInstance * findHero(CPlayerInterface & interface, std::optional<int> heroId)
{
	const auto heroes = interface.cb->getHeroesInfo();
	if(heroId)
	{
		for(const CGHeroInstance * hero : heroes)
			if(hero && hero->id.getNum() == *heroId)
				return hero;
		return nullptr;
	}

	if(const CGHeroInstance * current = interface.localState->getCurrentHero())
		if(!current->isGarrisoned())
			return current;

	for(const CGHeroInstance * hero : interface.localState->getWanderingHeroes())
		if(hero)
			return hero;

	for(const CGHeroInstance * hero : heroes)
		if(hero && !hero->isGarrisoned())
			return hero;

	return heroes.empty() ? nullptr : heroes.front();
}

}

std::string mcpMoveHero(int x, int y, std::optional<int> z, std::optional<int> heroId)
{
	if(!GAME || !GAME->interface() || !GAME->interface()->cb)
		return "error: no game is in progress";

	CPlayerInterface & interface = *GAME->interface();
	CCallback & cb = *interface.cb;

	std::shared_lock stateLock(CGameState::mutex);

	if(!cb.getActiveBattles().empty())
		return "error: a battle is in progress";
	if(!interface.makingTurn)
		return "error: it is not your turn";
	if(mcpHasBlockingDialog())
		return "error: a dialog is open";
	if(interface.isHeroMoving())
		return "error: a hero is already moving";

	const CGHeroInstance * hero = findHero(interface, heroId);
	if(!hero)
		return heroId ? "error: no hero with id " + std::to_string(*heroId) : "error: you have no hero";
	if(hero->isGarrisoned())
		return "error: " + heroName(*hero) + " is in a garrison";

	const int3 destination(x, y, z.value_or(hero->visitablePos().z));
	const int3 mapSize = cb.getMapSize();
	if(!inMap(destination, mapSize))
		return "error: " + posText(destination) + " is outside the map";
	if(destination == hero->visitablePos())
		return "error: " + heroName(*hero) + " is already at " + posText(destination);
	if(!cb.isVisible(destination))
		return "error: " + posText(destination) + " is not visible";

	EPathfindingLayer layer = EPathfindingLayer::AUTO;
	if(hero->inBoat() && hero->getBoat() && hero->getBoat()->layer == EPathfindingLayer::AVIATE)
		layer = EPathfindingLayer::AVIATE;

	if(!interface.localState->setPath(hero, destination, layer) || !interface.localState->hasPath(hero))
		return "error: no path to " + posText(destination);

	const CGPath & path = interface.localState->getPath(hero);
	if(!path.hasNextNode() || path.nextNode().turns != 0)
		return "error: not enough movement points to step toward " + posText(destination);

	int3 stop = path.currNode().coord;
	EPathNodeAction stopAction = path.currNode().action;
	int steps = 0;
	for(auto it = path.nodes.rbegin(); it != path.nodes.rend(); ++it)
	{
		if(it->turns != 0)
			break;
		stop = it->coord;
		stopAction = it->action;
		if(it->coord != path.currNode().coord)
			++steps;
	}

	if(steps == 0)
		return "error: not enough movement points to step toward " + posText(destination);

	interface.localState->setSelection(hero);
	if(adventureInt)
		adventureInt->onHeroChanged(hero);

	const std::string result = "move " + field("hero", std::to_string(hero->id.getNum()))
		+ " " + field("name", heroName(*hero))
		+ " " + field("toward", posText(destination))
		+ " " + field("steps", std::to_string(steps))
		+ " " + field("stop", posText(stop))
		+ (stopAction == EPathNodeAction::BATTLE ? " battle=yes" : "")
		+ "\n";

	stateLock.unlock();
	interface.moveHero(hero, interface.localState->getPath(hero));

	if(!interface.isHeroMoving())
		return "error: the game did not start the move";
	return result;
}

std::string mcpAttack(int stackId)
{
	if(stackId < 0)
		return "error: stack id must be a positive number";

	if(!GAME || !GAME->interface() || !GAME->interface()->cb)
		return "error: no game is in progress";

	CPlayerInterface & interface = *GAME->interface();
	CCallback & cb = *interface.cb;

	std::shared_lock stateLock(CGameState::mutex);

	if(interface.isAutoFightOn)
		return "error: autofight is on";

	const auto battles = cb.getActiveBattles();
	if(battles.empty() || !battles.begin()->second)
		return "error: no battle is in progress";

	const BattleID battleID = battles.begin()->first;
	const CPlayerBattleCallback & battle = *battles.begin()->second;

	if(battle.battleTacticDist() > 0)
		return "error: the tactics phase is still going";

	const CStack * actorStack = nullptr;
	const std::string readyError = battleActorReady(battleID, battle, actorStack);
	if(!readyError.empty())
		return readyError;
	const battle::Unit * actor = actorStack;

	const battle::Unit * target = battle.battleGetUnitByID(static_cast<uint32_t>(stackId));
	if(!target)
		return "error: no stack with id " + std::to_string(stackId);
	if(!target->alive())
		return "error: that stack is dead";
	if(!battle.battleMatchOwner(actor, target))
		return "error: that stack is not an enemy";

	std::vector<ActionOption> options;
	BattleActionType::collectAllOptions(battle, *actor, options);
	std::sort(options.begin(), options.end(), [&](const ActionOption & left, const ActionOption & right)
	{
		return left.type->getPriority(battle, left, *actor, target) < right.type->getPriority(battle, right, *actor, target);
	});

	const BattleHex hovered = target->getPosition();
	std::optional<BattleAction> action;
	BattleHex attackFrom;

	for(const ActionOption & option : options)
	{
		std::vector<ActionApproach> approaches = option.type->getApproaches(battle, option, *actor, hovered);
		std::sort(approaches.begin(), approaches.end(), [&](const ActionApproach & left, const ActionApproach & right)
		{
			return BattleHex::getDistance(actor->getPosition(), left.from) < BattleHex::getDistance(actor->getPosition(), right.from);
		});

		const std::size_t attempts = approaches.empty() ? 1 : approaches.size();
		for(std::size_t i = 0; i < attempts; ++i)
		{
			ActionContext context;
			context.actor = actor;
			context.hoveredHex = hovered;
			context.side = battle.battleGetMySide();
			if(!approaches.empty())
				context.approach = approaches[i];

			if(!option.type->isLegal(cb, battle, option, context))
				continue;

			std::optional<BattleAction> built = option.type->build(battle, option, context);
			if(!built || !attackActionName(built->actionType))
				continue;

			action = std::move(built);
			attackFrom = context.approach ? context.approach->from : BattleHex();
			break;
		}
		if(action)
			break;
	}

	if(!action)
		return "error: " + creatureName(*actor) + " cannot attack " + creatureName(*target)
			+ " (out of range or no legal move; use defend or wait, or pick another target)";

	std::string result = "attack";
	result += " " + field("action", attackActionName(action->actionType));
	result += " " + field("with", std::to_string(actor->unitId()));
	result += " " + field("creature", creatureName(*actor));
	result += " " + field("stack", std::to_string(target->unitId()));
	result += " " + field("target", creatureName(*target));
	if(attackFrom.isValid())
		result += " " + field("from", std::to_string(attackFrom.toInt()));
	result += "\n";

	std::shared_ptr<BattleInterface> battleUi = CPlayerInterface::battleInt;
	const bool useWindow = battleUi && battleUi->getBattleID() == battleID;

	stateLock.unlock();

	if(useWindow)
	{
		McpBattleWaiter::instance().arm();
		battleUi->sendCommand(*action, actorStack);
	}
	else
		cb.battleMakeUnitAction(battleID, *action);

	return result;
}

std::string mcpBattleWait()
{
	if(!GAME || !GAME->interface() || !GAME->interface()->cb)
		return "error: no game is in progress";

	CPlayerInterface & interface = *GAME->interface();
	CCallback & cb = *interface.cb;

	std::shared_lock stateLock(CGameState::mutex);

	if(interface.isAutoFightOn)
		return "error: autofight is on";

	const auto battles = cb.getActiveBattles();
	if(battles.empty() || !battles.begin()->second)
		return "error: no battle is in progress";

	const BattleID battleID = battles.begin()->first;
	const CPlayerBattleCallback & battle = *battles.begin()->second;

	if(battle.battleTacticDist() > 0)
		return "error: the tactics phase is still going";

	const CStack * actorStack = nullptr;
	const std::string readyError = battleActorReady(battleID, battle, actorStack);
	if(!readyError.empty())
		return readyError;
	if(actorStack->waitedThisRound())
		return "error: " + creatureName(*actorStack) + " already waited this round";

	const BattleAction action = BattleAction::makeWait(actorStack);
	std::string result = "wait stack=" + std::to_string(actorStack->unitId()) + " creature=" + creatureName(*actorStack) + "\n";

	std::shared_ptr<BattleInterface> battleUi = CPlayerInterface::battleInt;
	const bool useWindow = battleUi && battleUi->getBattleID() == battleID;

	stateLock.unlock();

	if(useWindow)
	{
		McpBattleWaiter::instance().arm();
		battleUi->sendCommand(action, actorStack);
	}
	else
		cb.battleMakeUnitAction(battleID, action);

	return result;
}

std::string mcpBattleDefend()
{
	if(!GAME || !GAME->interface() || !GAME->interface()->cb)
		return "error: no game is in progress";

	CPlayerInterface & interface = *GAME->interface();
	CCallback & cb = *interface.cb;

	std::shared_lock stateLock(CGameState::mutex);

	if(interface.isAutoFightOn)
		return "error: autofight is on";

	const auto battles = cb.getActiveBattles();
	if(battles.empty() || !battles.begin()->second)
		return "error: no battle is in progress";

	const BattleID battleID = battles.begin()->first;
	const CPlayerBattleCallback & battle = *battles.begin()->second;

	if(battle.battleTacticDist() > 0)
		return "error: the tactics phase is still going";

	const CStack * actorStack = nullptr;
	const std::string readyError = battleActorReady(battleID, battle, actorStack);
	if(!readyError.empty())
		return readyError;

	const BattleAction action = BattleAction::makeDefend(actorStack);
	std::string result = "defend stack=" + std::to_string(actorStack->unitId()) + " creature=" + creatureName(*actorStack) + "\n";

	std::shared_ptr<BattleInterface> battleUi = CPlayerInterface::battleInt;
	const bool useWindow = battleUi && battleUi->getBattleID() == battleID;

	stateLock.unlock();

	if(useWindow)
	{
		McpBattleWaiter::instance().arm();
		battleUi->sendCommand(action, actorStack);
	}
	else
		cb.battleMakeUnitAction(battleID, action);

	return result;
}

std::string mcpAnswer(int choice, std::optional<int> questionOverride)
{
	return applyMcpDialogAnswer(choice, questionOverride);
}

std::string mcpEndTurn()
{
	if(!GAME || !GAME->interface() || !GAME->interface()->cb)
		return "error: no game is in progress";

	CPlayerInterface & interface = *GAME->interface();
	CCallback & cb = *interface.cb;

	std::shared_lock stateLock(CGameState::mutex);

	if(!interface.makingTurn)
		return "error: it is not your turn";
	if(!cb.getActiveBattles().empty())
		return "error: a battle is in progress";
	if(mcpHasBlockingDialog())
		return "error: a dialog is open";
	if(interface.isHeroMoving())
		return "error: a hero is moving";

	stateLock.unlock();

	if(adventureInt)
		adventureInt->hotkeyEndingTurn();
	else
	{
		interface.makingTurn = false;
		cb.endTurn();
	}

	return "end_turn\n";
}
