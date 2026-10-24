/*
 * GameStateSnapshot.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "GameStateSnapshot.h"

#include "CPlayerInterface.h"
#include "GameEngine.h"
#include "GameInstance.h"
#include "gui/WindowHandler.h"
#include "widgets/CComponent.h"
#include "widgets/TextControls.h"
#include "windows/CExchangeWindow.h"
#include "windows/GUIClasses.h"
#include "windows/InfoWindows.h"

#include "../lib/CSkillHandler.h"
#include "../lib/ConditionalWait.h"
#include "../lib/GameLibrary.h"
#include "../lib/TerrainHandler.h"
#include "../lib/entities/hero/CHeroClass.h"
#include "../lib/gameState/InfoAboutArmy.h"
#include "../lib/battle/BattleSide.h"
#include "../lib/battle/CPlayerBattleCallback.h"
#include "../lib/callback/CCallback.h"
#include "../lib/constants/Enumerations.h"
#include "../lib/entities/ResourceTypeHandler.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "../lib/mapObjects/CGResource.h"
#include "../lib/mapObjects/CGTownInstance.h"
#include "../lib/mapObjects/army/CCreatureSet.h"
#include "../lib/CStack.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/texts/MetaString.h"

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

std::string ownerText(const PlayerColor & color)
{
	if(!color.isValidPlayer())
		return "neutral";
	return color.toString();
}

const char * skillLevelName(ui8 level)
{
	switch(level)
	{
		case 1:
			return "basic";
		case 2:
			return "advanced";
		case 3:
			return "expert";
		default:
			return nullptr;
	}
}

const char * fortName(CGTownInstance::EFortLevel level)
{
	switch(level)
	{
		case CGTownInstance::NONE:
			return "none";
		case CGTownInstance::FORT:
			return "fort";
		case CGTownInstance::CITADEL:
			return "citadel";
		case CGTownInstance::CASTLE:
			return "castle";
	}
	return "unknown";
}

const char * sideName(BattleSide side)
{
	switch(side)
	{
		case BattleSide::ATTACKER:
			return "attacker";
		case BattleSide::DEFENDER:
			return "defender";
		default:
			return "none";
	}
}

bool seesExactArmy(const CCallback & cb, const CGObjectInstance * obj)
{
	if(!obj || !cb.getPlayerID())
		return false;
	return cb.getPlayerRelations(obj->tempOwner, *cb.getPlayerID()) != PlayerRelations::ENEMIES;
}

void appendExactArmy(std::string & out, const CCreatureSet & army)
{
	for(const auto & slot : army.Slots())
	{
		if(!slot.second || slot.second->getCount() <= 0)
			continue;

		const CCreature * creature = slot.second->getCreature();
		if(!creature)
			continue;

		out += "  army:";
		out += " " + field("slot", std::to_string(slot.first.getNum()));
		out += " " + field("creature", creature->getNamePluralTranslated());
		out += " " + field("count", std::to_string(slot.second->getCount()));
		out += "\n";
	}
}

void appendRoughArmy(std::string & out, const CCreatureSet & army, const ITranslator & translator)
{
	if(army.stacksCount() <= 0)
		return;

	const std::string rough = army.getArmyDescription().toString(&translator);
	if(rough.empty())
		return;

	out += "  army: " + quote(rough) + "\n";
}

std::string objectLabel(const CGObjectInstance & obj, const PlayerColor & player, const ITranslator & translator)
{
	const std::string hover = obj.getHoverText(player).toString(&translator);
	if(!hover.empty())
		return hover;
	return obj.getObjectName().toString(&translator);
}

void appendHero(std::string & out, const CGHeroInstance & hero, const ITranslator & translator)
{
	const std::string name = translator.translate(hero.getNameTextID());
	const CHeroClass * heroClass = hero.getHeroClass();

	out += "hero";
	out += " " + field("id", std::to_string(hero.id.getNum()));
	out += " " + field("name", name.empty() ? std::string("Hero") : name);
	if(heroClass)
		out += " " + field("class", heroClass->getNameTranslated());
	out += " " + field("pos", posText(hero.visitablePos()));
	out += " " + field("level", std::to_string(hero.level));
	out += " " + field("move", std::to_string(hero.movementPointsRemaining()) + "/" + std::to_string(hero.movementPointsLimit()));
	out += " " + field("mana", std::to_string(hero.mana) + "/" + std::to_string(hero.manaLimit()));
	out += " " + field("attack", std::to_string(hero.getPrimSkillLevel(PrimarySkill::ATTACK)));
	out += " " + field("defense", std::to_string(hero.getPrimSkillLevel(PrimarySkill::DEFENSE)));
	out += " " + field("power", std::to_string(hero.getPrimSkillLevel(PrimarySkill::SPELL_POWER)));
	out += " " + field("knowledge", std::to_string(hero.getPrimSkillLevel(PrimarySkill::KNOWLEDGE)));
	out += " " + field("garrison", hero.isGarrisoned() ? "yes" : "no");
	out += " " + field("boat", hero.inBoat() ? "yes" : "no");
	out += "\n";

	appendExactArmy(out, hero);

	for(const auto & [skill, level] : hero.secSkills)
	{
		if(skill.getNum() < 0 || skill.getNum() >= GameConstants::SKILL_QUANTITY || level == 0)
			continue;

		const auto * skillType = LIBRARY->skillh->getById(skill);
		if(!skillType)
			continue;

		const char * levelName = skillLevelName(level);
		out += "  skill:";
		out += " " + field("level", levelName ? levelName : std::to_string(level));
		out += " " + field("name", skillType->getNameTranslated());
		out += "\n";
	}
}

void appendTown(std::string & out, const CCallback & cb, const CGTownInstance & town, const ITranslator & translator)
{
	const CFaction * faction = town.getFaction();

	out += "town";
	out += " " + field("id", std::to_string(town.id.getNum()));
	out += " " + field("name", translator.translate(town.getNameTextID()));
	if(faction)
		out += " " + field("faction", faction->getNameTranslated());
	out += " " + field("pos", posText(town.visitablePos()));
	out += " " + field("owner", ownerText(town.tempOwner));
	out += " " + field("fort", fortName(town.fortLevel()));
	out += "\n";

	if(seesExactArmy(cb, &town))
		appendExactArmy(out, town);
	else
		appendRoughArmy(out, town, translator);

	if(const CGHeroInstance * visiting = town.getVisitingHero())
		out += "  hero: " + field("role", "visiting") + " " + field("id", std::to_string(visiting->id.getNum())) + "\n";
	if(const CGHeroInstance * garrisoned = town.getGarrisonHero())
		out += "  hero: " + field("role", "garrison") + " " + field("id", std::to_string(garrisoned->id.getNum())) + "\n";
}

void appendObject(std::string & out, const CCallback & cb, const CGObjectInstance & obj, const ITranslator & translator)
{
	out += "object";
	out += " " + field("id", std::to_string(obj.id.getNum()));
	out += " " + field("typeId", std::to_string(obj.ID.getNum()));
	out += " " + field("pos", posText(obj.visitablePos()));
	out += " " + field("owner", ownerText(obj.tempOwner));
	out += " " + field("name", objectLabel(obj, cb.getPlayerID().value_or(PlayerColor::NEUTRAL), translator));

	if(const auto * pile = dynamic_cast<const CGResource *>(&obj))
		out += " " + field("amount", std::to_string(pile->getAmount()));

	out += "\n";

	if(const auto * armed = dynamic_cast<const CArmedInstance *>(&obj))
	{
		if(seesExactArmy(cb, &obj))
			appendExactArmy(out, *armed);
		else
			appendRoughArmy(out, *armed, translator);
	}
}

void appendBattleHero(std::string & out, const CPlayerBattleCallback & battle, BattleSide side, const ITranslator & translator)
{
	if(!battle.battleHasHero(side))
		return;

	const InfoAboutHero info = battle.battleGetHeroInfo(side);
	out += "battle_hero";
	out += " " + field("side", sideName(side));
	out += " " + field("name", info.name.toString(&translator));
	if(info.hclass)
		out += " " + field("class", info.hclass->getNameTranslated());
	if(info.details)
	{
		out += " " + field("mana", std::to_string(info.details->mana) + "/" + std::to_string(info.details->manaLimit));
		if(info.details->primskills.size() >= 4)
		{
			out += " " + field("attack", std::to_string(info.details->primskills[0]));
			out += " " + field("defense", std::to_string(info.details->primskills[1]));
			out += " " + field("power", std::to_string(info.details->primskills[2]));
			out += " " + field("knowledge", std::to_string(info.details->primskills[3]));
		}
	}
	out += "\n";
}

void appendBattle(std::string & out, CCallback & cb, const ITranslator & translator)
{
	const auto battles = cb.getActiveBattles();
	if(battles.empty())
	{
		out += "battle: none\n";
		return;
	}

	for(const auto & [battleID, battle] : battles)
	{
		if(!battle)
			continue;

		out += "battle";
		out += " " + field("id", std::to_string(battleID.getNum()));
		out += " " + field("round", std::to_string(battle->battleGetRound()));
		out += " " + field("side", sideName(battle->battleGetMySide()));

		const TerrainId terrain = battle->battleTerrainType();
		if(const auto * terrainType = LIBRARY->terrainTypeHandler->getById(terrain))
			out += " " + field("terrain", terrainType->getNameTranslated());

		const int tactics = battle->battleTacticDist();
		if(tactics > 0)
			out += " " + field("tactics", std::to_string(tactics));

		if(const battle::Unit * active = battle->battleActiveUnit())
		{
			out += " " + field("active", std::to_string(active->unitId()));
			out += " " + field("active_turn", active->unitSide() == battle->battleGetMySide() ? "yours" : "enemy");
		}
		else
		{
			out += " " + field("active", "none");
			out += " " + field("active_turn", "none");
		}

		if(const CGTownInstance * town = battle->battleGetDefendedTown())
			out += " " + field("siege", translator.translate(town->getNameTextID()));

		out += "\n";

		appendBattleHero(out, *battle, BattleSide::ATTACKER, translator);
		appendBattleHero(out, *battle, BattleSide::DEFENDER, translator);

		auto stacks = battle->battleGetStacks(CBattleInfoEssentials::MINE_AND_ENEMY, true);
		std::sort(stacks.begin(), stacks.end(), [](const CStack * left, const CStack * right)
		{
			if(!left || !right)
				return left != nullptr;
			return left->getPosition().toInt() < right->getPosition().toInt();
		});

		for(const CStack * stack : stacks)
		{
			if(!stack)
				continue;

			const CCreature * creature = stack->unitType();
			const int maxHealth = creature ? static_cast<int>(creature->getMaxHealth()) : 0;

			out += "stack";
			out += " " + field("id", std::to_string(stack->unitId()));
			out += " " + field("side", sideName(stack->unitSide()));
			out += " " + field("owner", ownerText(stack->unitOwner()));
			if(creature)
				out += " " + field("creature", creature->getNamePluralTranslated());
			out += " " + field("count", std::to_string(stack->getCount()));
			out += " " + field("hp", std::to_string(stack->getFirstHPleft()) + "/" + std::to_string(maxHealth));
			out += " " + field("hex", std::to_string(stack->getPosition().toInt()));
			if(stack->isShooter())
				out += " " + field("shots", std::to_string(stack->shots.available()));
			if(stack->waited())
				out += " waited=yes";
			if(stack->defended())
				out += " defended=yes";
			if(stack->doubleWide())
				out += " wide=yes";
			out += "\n";
		}
	}
}

void appendCaught(std::string & out, const std::string & kind, int id, const std::exception & error)
{
	out += kind;
	out += " " + field("id", std::to_string(id));
	out += " " + field("error", error.what());
	out += "\n";
}

const char * componentTypeName(ComponentType type)
{
	switch(type)
	{
		case ComponentType::RESOURCE:
			return "resource";
		case ComponentType::EXPERIENCE:
			return "experience";
		case ComponentType::ARTIFACT:
			return "artifact";
		case ComponentType::CREATURE:
			return "creature";
		case ComponentType::SPELL:
			return "spell";
		case ComponentType::SEC_SKILL:
			return "skill";
		case ComponentType::PRIM_SKILL:
			return "primary";
		case ComponentType::MANA:
			return "mana";
		case ComponentType::MORALE:
			return "morale";
		case ComponentType::LUCK:
			return "luck";
		default:
			return "item";
	}
}

void appendComponentOption(std::string & out, int choice, const CComponent & component)
{
	out += "  option";
	out += " " + field("choice", std::to_string(choice));
	out += " " + field("type", componentTypeName(component.data.type));
	if(component.data.type == ComponentType::RESOURCE)
	{
		const GameResID resourceId = component.data.subType.as<GameResID>();
		if(resourceId.hasValue())
		{
			if(const auto * resource = LIBRARY->resourceTypeHandler->getById(resourceId))
				out += " " + field("name", resource->getNameTranslated());
		}
	}
	const std::string subtitle = component.getSubtitle();
	if(!subtitle.empty())
		out += " " + field("label", subtitle);
	out += "\n";
}

void appendInfoWindow(std::string & out, const CInfoWindow & window)
{
	const bool choose = dynamic_cast<const CSelWindow *>(&window) != nullptr;
	const bool yesNo = !choose && window.buttons.size() >= 2;
	const bool hasQuestion = window.ID.getNum() >= 0;

	out += "dialog";
	out += " blocking=yes";
	if(hasQuestion)
		out += " " + field("question", std::to_string(window.ID.getNum()));
	out += " " + field("type", choose ? "choose" : (yesNo ? "yes_no" : "info"));
	out += "\n";

	if(window.text && window.text->label)
	{
		const std::string text = window.text->label->getText();
		if(!text.empty())
			out += "  text: " + quote(text) + "\n";
	}

	if(choose && window.components)
	{
		int choice = 1;
		for(const auto & component : window.components->getComponents())
		{
			if(component)
				appendComponentOption(out, choice++, *component);
		}
		if(window.buttons.size() > 1)
			out += "  option " + field("choice", "0") + " name=cancel\n";
	}
	else if(yesNo)
	{
		out += "  option " + field("choice", "1") + " name=yes\n";
		out += "  option " + field("choice", "0") + " name=no\n";
	}
	else
	{
		out += "  option " + field("choice", "1") + " name=ok\n";
	}
}

void appendDialogs(std::string & out, CPlayerInterface & interface)
{
	bool wrote = false;

	if(ENGINE)
	{
		for(const auto & window : ENGINE->windows().findWindows<CInfoWindow>())
		{
			if(!window)
				continue;
			appendInfoWindow(out, *window);
			wrote = true;
		}

		for(const auto & window : ENGINE->windows().findWindows<CLevelWindow>())
		{
			if(!window)
				continue;
			out += "dialog blocking=yes type=level_up\n";
			out += "  text: \"Choose a skill for the level-up.\"\n";
			wrote = true;
		}

		for(const auto & window : ENGINE->windows().findWindows<CExchangeWindow>())
		{
			if(!window)
				continue;
			out += "dialog blocking=yes type=exchange";
			if(window->getQuestionID().getNum() >= 0)
				out += " " + field("question", std::to_string(window->getQuestionID().getNum()));
			out += "\n";
			out += "  option " + field("choice", "0") + " name=leave\n";
			wrote = true;
		}
	}

	if(!wrote && interface.showingDialog && interface.showingDialog->isBusy())
		out += "dialog blocking=yes type=unknown\n";
}

}

std::string buildMcpGameState()
{
	if(!GAME || !GAME->interface() || !GAME->interface()->cb)
		return "No game is in progress.\n";

	std::shared_lock stateLock(CGameState::mutex);

	CPlayerInterface & interface = *GAME->interface();
	CCallback & cb = *interface.cb;
	const ITranslator & translator = GAME->translator();

	std::string out;
	const Calendar calendar = cb.getCalendar();
	const int3 mapSize = cb.getMapSize();

	out += "player " + field("color", ownerText(interface.playerID)) + "\n";
	out += "date";
	out += " " + field("day", std::to_string(calendar.getCurrentDay()));
	out += " " + field("weekday", std::to_string(calendar.getDayOfWeek()));
	out += " " + field("week", std::to_string(calendar.getWeek()));
	out += " " + field("month", std::to_string(calendar.getMonth()));
	out += "\n";
	out += "map";
	out += " " + field("width", std::to_string(mapSize.x));
	out += " " + field("height", std::to_string(mapSize.y));
	out += " " + field("levels", std::to_string(mapSize.z));
	out += "\n";

	std::string resources;
	for(const GameResID resourceId : LIBRARY->resourceTypeHandler->getAllObjects())
	{
		const auto * resource = LIBRARY->resourceTypeHandler->getById(resourceId);
		if(!resource)
			continue;
		if(!resources.empty())
			resources += " ";
		resources += field(resource->getJsonKey(), std::to_string(cb.getResourceAmount(resourceId)));
	}
	out += "resources " + resources + "\n";

	const bool inBattle = !cb.getActiveBattles().empty();
	const char * activity = inBattle ? "battle" : (interface.makingTurn ? "your_turn" : "waiting");
	out += "activity " + field("state", activity);
	out += " " + field("moving", interface.isHeroMoving() ? "yes" : "no");
	out += " " + field("dialog", interface.showingDialog && interface.showingDialog->isBusy() ? "yes" : "no");
	if(interface.isAutoFightOn)
		out += " autofight=yes";
	out += "\n";

	appendDialogs(out, interface);

	auto heroes = cb.getHeroesInfo();
	std::sort(heroes.begin(), heroes.end(), [](const CGHeroInstance * left, const CGHeroInstance * right)
	{
		if(!left || !right)
			return left != nullptr;
		return left->id.getNum() < right->id.getNum();
	});

	std::set<int> ownHeroIds;
	if(heroes.empty())
		out += "heroes: none\n";
	for(const CGHeroInstance * hero : heroes)
	{
		if(!hero)
			continue;
		ownHeroIds.insert(hero->id.getNum());
		try
		{
			appendHero(out, *hero, translator);
		}
		catch(const std::exception & error)
		{
			appendCaught(out, "hero", hero->id.getNum(), error);
		}
	}

	auto towns = cb.getTownsInfo(false);
	std::sort(towns.begin(), towns.end(), [](const CGTownInstance * left, const CGTownInstance * right)
	{
		if(!left || !right)
			return left != nullptr;
		return left->id.getNum() < right->id.getNum();
	});

	if(towns.empty())
		out += "towns: none\n";
	for(const CGTownInstance * town : towns)
	{
		if(!town)
			continue;
		try
		{
			appendTown(out, cb, *town, translator);
		}
		catch(const std::exception & error)
		{
			appendCaught(out, "town", town->id.getNum(), error);
		}
	}

	auto objects = cb.getAllVisitableObjs();
	std::sort(objects.begin(), objects.end(), [](const CGObjectInstance * left, const CGObjectInstance * right)
	{
		if(!left || !right)
			return left != nullptr;
		const int3 leftPos = left->visitablePos();
		const int3 rightPos = right->visitablePos();
		if(leftPos.z != rightPos.z)
			return leftPos.z < rightPos.z;
		if(leftPos.y != rightPos.y)
			return leftPos.y < rightPos.y;
		if(leftPos.x != rightPos.x)
			return leftPos.x < rightPos.x;
		return left->id.getNum() < right->id.getNum();
	});

	bool wroteObject = false;
	for(const CGObjectInstance * obj : objects)
	{
		if(!obj)
			continue;
		if(obj->ID == Obj::TOWN || obj->ID == Obj::RANDOM_TOWN)
			continue;
		if(obj->ID == Obj::HERO && ownHeroIds.count(obj->id.getNum()))
			continue;

		wroteObject = true;
		try
		{
			appendObject(out, cb, *obj, translator);
		}
		catch(const std::exception & error)
		{
			appendCaught(out, "object", obj->id.getNum(), error);
		}
	}
	if(!wroteObject)
		out += "objects: none\n";

	try
	{
		appendBattle(out, cb, translator);
	}
	catch(const std::exception & error)
	{
		out += "battle " + field("error", error.what()) + "\n";
	}

	return out;
}
