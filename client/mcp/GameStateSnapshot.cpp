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
#include "GameActions.h"
#include "McpDialogs.h"

#include "CPlayerInterface.h"
#include "PlayerLocalState.h"
#include "adventureMap/AdventureMapInterface.h"
#include "battle/BattleInterface.h"
#include "GameEngine.h"
#include "GameInstance.h"
#include "gui/WindowHandler.h"
#include "mapView/MapView.h"
#include "widgets/CComponent.h"
#include "widgets/TextControls.h"

#include <cstring>

#include "../lib/CSkillHandler.h"
#include "../lib/ConditionalWait.h"
#include "../lib/CCreatureHandler.h"
#include "../lib/GameLibrary.h"
#include "../lib/Rect.h"
#include "../lib/RiverHandler.h"
#include "../lib/RoadHandler.h"
#include "../lib/TerrainHandler.h"
#include "../lib/mapping/TerrainTile.h"
#include "../lib/entities/artifact/CArtifact.h"
#include "../lib/entities/artifact/CArtifactInstance.h"
#include "../lib/entities/artifact/CArtifactSet.h"
#include "../lib/entities/building/CBuilding.h"
#include "../lib/entities/faction/CTown.h"
#include "../lib/entities/hero/CHeroClass.h"
#include "../lib/gameState/InfoAboutArmy.h"
#include "../lib/battle/BattleSide.h"
#include "../lib/battle/CPlayerBattleCallback.h"
#include "../lib/callback/CCallback.h"
#include "../lib/constants/EntityIdentifiers.h"
#include "../lib/constants/Enumerations.h"
#include "../lib/entities/ResourceTypeHandler.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "../lib/mapObjects/CGResource.h"
#include "../lib/mapObjects/CGTownInstance.h"
#include "../lib/mapObjects/army/CArmedInstance.h"
#include "../lib/mapObjects/army/CCreatureSet.h"
#include "../lib/CStack.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/pathfinder/CGPathNode.h"
#include "../lib/spells/CSpellHandler.h"
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

const char * relationName(PlayerRelations relation)
{
	switch(relation)
	{
		case PlayerRelations::ENEMIES:
			return "enemy";
		case PlayerRelations::ALLIES:
			return "ally";
		default:
			return "neutral";
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

void appendHeroIndex(std::string & out, const CGHeroInstance & hero, const ITranslator & translator)
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
	out += " " + field("garrison", hero.isGarrisoned() ? "yes" : "no");
	out += " " + field("boat", hero.inBoat() ? "yes" : "no");
	out += "\n";
}

void appendHeroDetail(std::string & out, const CGHeroInstance & hero, const ITranslator & translator)
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

	for(const auto & [slot, info] : hero.artifactsWorn)
	{
		if(slot == ArtifactPosition::SPELLBOOK || !info.getArt() || info.locked)
			continue;
		const CArtifact * type = info.getArt()->getType();
		if(!type)
			continue;
		out += "  artifact: " + field("slot", "worn") + " " + field("name", type->getNameTranslated()) + "\n";
	}
	for(const ArtSlotInfo & info : hero.artifactsInBackpack)
	{
		if(!info.getArt() || info.locked)
			continue;
		const CArtifact * type = info.getArt()->getType();
		if(!type)
			continue;
		out += "  artifact: " + field("slot", "backpack") + " " + field("name", type->getNameTranslated()) + "\n";
	}

	for(const SpellID spellId : hero.getSpellsInSpellbook())
	{
		const auto * spell = LIBRARY->spellh->getById(spellId);
		if(!spell)
			continue;
		out += "  spell: " + field("name", spell->getNameTranslated()) + "\n";
	}
}

void appendTownIndex(std::string & out, const CCallback & cb, const CGTownInstance & town, const ITranslator & translator)
{
	const CFaction * faction = town.getFaction();

	out += "town";
	out += " " + field("id", std::to_string(town.id.getNum()));
	out += " " + field("name", translator.translate(town.getNameTextID()));
	if(faction)
		out += " " + field("faction", faction->getNameTranslated());
	out += " " + field("pos", posText(town.visitablePos()));
	out += " " + field("owner", ownerText(town.tempOwner));
	if(const auto player = cb.getPlayerID())
		out += " " + field("relation", relationName(cb.getPlayerRelations(*player, town.tempOwner)));
	out += " " + field("fort", fortName(town.fortLevel()));
	out += "\n";
}

void appendTownDetail(std::string & out, const CCallback & cb, const CGTownInstance & town, const ITranslator & translator)
{
	appendTownIndex(out, cb, town, translator);

	if(seesExactArmy(cb, &town))
		appendExactArmy(out, town);
	else
		appendRoughArmy(out, town, translator);

	if(const CGHeroInstance * visiting = town.getVisitingHero())
		out += "  hero: " + field("role", "visiting") + " " + field("id", std::to_string(visiting->id.getNum())) + "\n";
	if(const CGHeroInstance * garrisoned = town.getGarrisonHero())
		out += "  hero: " + field("role", "garrison") + " " + field("id", std::to_string(garrisoned->id.getNum())) + "\n";

	if(!seesExactArmy(cb, &town) || !town.getTown())
		return;

	for(const BuildingID buildingId : town.getBuildings())
	{
		const auto found = town.getTown()->buildings.find(buildingId);
		if(found == town.getTown()->buildings.end() || !found->second)
			continue;
		out += "  building: " + field("name", found->second->getNameTranslated()) + "\n";
	}

	for(const auto & [amount, ids] : town.creatures)
	{
		if(ids.empty() || amount <= 0)
			continue;
		const auto * creature = LIBRARY->creh->getById(ids.back());
		if(!creature)
			continue;
		out += "  recruit:";
		out += " " + field("creature", creature->getNamePluralTranslated());
		out += " " + field("count", std::to_string(amount));
		out += "\n";
	}
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

		const battle::Unit * activeUnit = battle->battleActiveUnit();
		const int activeStackId = activeUnit ? activeUnit->unitId() : -1;

		auto stacks = battle->battleGetStacks(CBattleInfoEssentials::MINE_AND_ENEMY, true);
		std::sort(stacks.begin(), stacks.end(), [](const CStack * left, const CStack * right)
		{
			if(!left || !right)
				return left != nullptr;
			return left->getPosition().toInt() < right->getPosition().toInt();
		});

		const CStack * actor = nullptr;
		for(const CStack * stack : stacks)
		{
			if(stack && stack->unitId() == static_cast<uint32_t>(activeStackId))
			{
				actor = stack;
				break;
			}
		}

		bool ourTurn = false;
		const char * turn = "none";
		if(activeUnit)
		{
			const BattleSide mySide = battle->battleGetMySide();
			turn = activeUnit->unitSide() == mySide ? "yours" : "enemy";
			if(activeUnit->unitSide() == mySide)
			{
				if(const std::shared_ptr<BattleInterface> battleUi = CPlayerInterface::battleInt;
					battleUi && battleUi->getBattleID() == battleID && (battleUi->hasAnimations() || !battleUi->makingTurn()))
					turn = "busy";
				else
					ourTurn = actor != nullptr;
			}
		}

		std::set<int> legalTargets;
		std::string canFields;
		if(ourTurn)
		{
			for(int id : mcpLegalAttackIds(cb, *battle, *actor))
				legalTargets.insert(id);
			std::string attackIds;
			for(int id : legalTargets)
			{
				if(!attackIds.empty())
					attackIds += ",";
				attackIds += std::to_string(id);
			}
			canFields = "attack=" + (attackIds.empty() ? std::string("none") : attackIds)
				+ " wait=" + (actor->waitedThisRound() ? "no" : "yes")
				+ " defend=yes";
		}

		if(activeUnit)
			out += " " + field("active", std::to_string(activeStackId));
		else
			out += " " + field("active", "none");
		out += " " + field("active_turn", turn);
		if(!canFields.empty())
			out += " " + canFields;

		if(const CGTownInstance * town = battle->battleGetDefendedTown())
			out += " " + field("siege", translator.translate(town->getNameTextID()));

		out += "\n";

		appendBattleHero(out, *battle, BattleSide::ATTACKER, translator);
		appendBattleHero(out, *battle, BattleSide::DEFENDER, translator);

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
			if(stack->unitId() == activeStackId)
				out += " active=yes";
			if(stack->isShooter())
				out += " " + field("shots", std::to_string(stack->shots.available()));
			if(stack->waited())
				out += " waited=yes";
			if(stack->defended())
				out += " defended=yes";
			if(stack->doubleWide())
				out += " wide=yes";
			if(ourTurn && stack->unitSide() != battle->battleGetMySide())
				out += stack->alive() && legalTargets.count(static_cast<int>(stack->unitId())) ? " target=yes" : " target=no";
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

std::string entityShortName(const std::string & jsonKey, const char * stripSuffix)
{
	std::string key = jsonKey;
	const auto colon = key.rfind(':');
	if(colon != std::string::npos)
		key = key.substr(colon + 1);
	if(stripSuffix)
	{
		const size_t suffixLen = std::strlen(stripSuffix);
		if(key.size() > suffixLen && key.compare(key.size() - suffixLen, suffixLen, stripSuffix) == 0)
			key.resize(key.size() - suffixLen);
	}
	return key;
}

std::string xyRange(int startX, int endX, int y)
{
	std::string text = std::to_string(startX) + "," + std::to_string(y);
	if(endX != startX)
		text += "-" + std::to_string(endX) + "," + std::to_string(y);
	return text;
}

const char * mapObjectKind(MapObjectID id)
{
	switch(id.getNum())
	{
		case Obj::HERO:
		case Obj::RANDOM_HERO:
		case Obj::HERO_PLACEHOLDER:
			return "hero";
		case Obj::TOWN:
		case Obj::RANDOM_TOWN:
			return "town";
		case Obj::MONSTER:
		case Obj::RANDOM_MONSTER:
		case Obj::RANDOM_MONSTER_L1:
		case Obj::RANDOM_MONSTER_L2:
		case Obj::RANDOM_MONSTER_L3:
		case Obj::RANDOM_MONSTER_L4:
		case Obj::RANDOM_MONSTER_L5:
		case Obj::RANDOM_MONSTER_L6:
		case Obj::RANDOM_MONSTER_L7:
			return "monster";
		case Obj::ARTIFACT:
		case Obj::RANDOM_ART:
		case Obj::RANDOM_TREASURE_ART:
		case Obj::RANDOM_MINOR_ART:
		case Obj::RANDOM_MAJOR_ART:
		case Obj::RANDOM_RELIC_ART:
		case Obj::SPELL_SCROLL:
			return "artifact";
		case Obj::RESOURCE:
		case Obj::RANDOM_RESOURCE:
		case Obj::CAMPFIRE:
		case Obj::TREASURE_CHEST:
		case Obj::SEA_CHEST:
		case Obj::FLOTSAM:
		case Obj::SHIPWRECK_SURVIVOR:
		case Obj::PANDORAS_BOX:
			return "loot";
		case Obj::MINE:
		case Obj::ABANDONED_MINE:
		case Obj::LIGHTHOUSE:
		case Obj::SHIPYARD:
			return "flaggable";
		case Obj::CREATURE_GENERATOR1:
		case Obj::CREATURE_GENERATOR2:
		case Obj::CREATURE_GENERATOR3:
		case Obj::CREATURE_GENERATOR4:
		case Obj::RANDOM_DWELLING:
		case Obj::RANDOM_DWELLING_LVL:
		case Obj::RANDOM_DWELLING_FACTION:
		case Obj::REFUGEE_CAMP:
		case Obj::WAR_MACHINE_FACTORY:
			return "dwelling";
		case Obj::SUBTERRANEAN_GATE:
			return "subterranean_gate";
		case Obj::MONOLITH_ONE_WAY_ENTRANCE:
			return "monolith_entrance";
		case Obj::MONOLITH_ONE_WAY_EXIT:
			return "monolith_exit";
		case Obj::MONOLITH_TWO_WAY:
			return "monolith";
		case Obj::WHIRLPOOL:
			return "whirlpool";
		case Obj::BOAT:
			return "boat";
		case Obj::BORDERGUARD:
		case Obj::BORDER_GATE:
		case Obj::QUEST_GUARD:
		case Obj::GARRISON:
		case Obj::GARRISON2:
			return "gate";
		case Obj::PRISON:
			return "prison";
		case Obj::SEER_HUT:
			return "quest";
		case Obj::SIGN:
		case Obj::OCEAN_BOTTLE:
			return "sign";
		default:
			return "visit";
	}
}

struct MapViewport
{
	int minX = 0;
	int minY = 0;
	int maxX = -1;
	int maxY = -1;
	int z = 0;
};

MapViewport cameraViewport(CCallback & cb, CPlayerInterface & interface)
{
	const int3 mapSize = cb.getMapSize();
	MapViewport view;
	view.z = 0;
	view.minX = 0;
	view.minY = 0;
	view.maxX = mapSize.x - 1;
	view.maxY = mapSize.y - 1;

	if(adventureInt && adventureInt->getMapView())
	{
		const Rect tiles = adventureInt->getMapView()->getVisibleTilesRect();
		view.z = adventureInt->getMapViewCenter().z;
		view.minX = std::max(0, tiles.x);
		view.minY = std::max(0, tiles.y);
		view.maxX = std::min(mapSize.x - 1, tiles.x + tiles.w - 1);
		view.maxY = std::min(mapSize.y - 1, tiles.y + tiles.h - 1);
		return view;
	}

	const CGHeroInstance * hero = interface.localState->getCurrentHero();
	if(!hero)
	{
		const auto heroes = cb.getHeroesInfo();
		if(!heroes.empty())
			hero = heroes.front();
	}
	if(!hero)
		return view;

	const int3 center = hero->visitablePos();
	view.z = center.z;
	constexpr int halfWidth = 10;
	constexpr int halfHeight = 8;
	view.minX = std::max(0, center.x - halfWidth);
	view.minY = std::max(0, center.y - halfHeight);
	view.maxX = std::min(mapSize.x - 1, center.x + halfWidth);
	view.maxY = std::min(mapSize.y - 1, center.y + halfHeight);
	return view;
}

bool inViewport(const MapViewport & view, const int3 & pos)
{
	return pos.z == view.z && pos.x >= view.minX && pos.x <= view.maxX && pos.y >= view.minY && pos.y <= view.maxY;
}

void appendViewportTerrain(std::string & out, CCallback & cb, const MapViewport & view)
{
	struct Group
	{
		std::string terrain;
		std::string road;
		std::string river;
		std::string walk;
		std::vector<int3> tiles;
	};

	std::map<std::string, Group> groups;
	int visible = 0;
	int hidden = 0;

	for(int y = view.minY; y <= view.maxY; ++y)
	{
		for(int x = view.minX; x <= view.maxX; ++x)
		{
			const int3 pos(x, y, view.z);
			const TerrainTile * tile = cb.tryGetTile(pos);
			if(!tile)
			{
				++hidden;
				continue;
			}
			++visible;

			const TerrainType * terrain = tile->getTerrain();
			std::string walk = "land";
			if(terrain && terrain->isWater())
				walk = "water";
			else if(terrain && !terrain->isPassable())
				walk = "rock";
			else if(tile->blocked() && !tile->visitable())
				walk = "blocked";

			std::string terrainName = terrain ? entityShortName(terrain->getJsonKey(), nullptr) : "unknown";
			std::string roadName;
			if(tile->hasRoad() && tile->getRoad())
				roadName = entityShortName(tile->getRoad()->getJsonKey(), "Road");
			std::string riverName;
			if(tile->hasRiver() && tile->getRiver())
				riverName = entityShortName(tile->getRiver()->getJsonKey(), "River");

			const std::string key = terrainName + "|" + walk + "|" + roadName + "|" + riverName;
			Group & group = groups[key];
			if(group.tiles.empty())
			{
				group.terrain = std::move(terrainName);
				group.walk = std::move(walk);
				group.road = std::move(roadName);
				group.river = std::move(riverName);
			}
			group.tiles.push_back(pos);
		}
	}

	std::vector<const Group *> ordered;
	ordered.reserve(groups.size());
	for(const auto & entry : groups)
		ordered.push_back(&entry.second);
	std::sort(ordered.begin(), ordered.end(), [](const Group * left, const Group * right)
	{
		if(left->terrain != right->terrain)
			return left->terrain < right->terrain;
		if(left->walk != right->walk)
			return left->walk < right->walk;
		if(left->road != right->road)
			return left->road < right->road;
		return left->river < right->river;
	});

	for(const Group * group : ordered)
	{
		out += "ground";
		out += " " + field("terrain", group->terrain);
		out += " " + field("walk", group->walk);
		if(!group->road.empty())
			out += " " + field("road", group->road);
		if(!group->river.empty())
			out += " " + field("river", group->river);
		std::string coords;
		int runY = -1;
		int runStartX = -1;
		int runEndX = -1;
		const auto flushRun = [&]()
		{
			if(runStartX < 0)
				return;
			if(!coords.empty())
				coords += " ";
			coords += xyRange(runStartX, runEndX, runY);
			runStartX = -1;
			runEndX = -1;
			runY = -1;
		};
		for(const int3 & pos : group->tiles)
		{
			if(runStartX >= 0 && pos.y == runY && pos.x == runEndX + 1)
			{
				runEndX = pos.x;
				continue;
			}
			flushRun();
			runY = pos.y;
			runStartX = pos.x;
			runEndX = pos.x;
		}
		flushRun();
		out += " " + field("tiles", coords) + "\n";
	}

	if(hidden > 0)
		out += "fog " + field("tiles", std::to_string(hidden)) + " " + field("visible", std::to_string(visible)) + "\n";
}

void appendViewportObjects(std::string & out, CCallback & cb, const MapViewport & view, const ITranslator & translator)
{
	struct MapObject
	{
		int3 pos;
		std::string kind;
		std::string name;
		std::string owner;
		std::string army;
		int id = 0;
	};

	std::vector<MapObject> listed;
	std::set<int> seen;
	const PlayerColor viewer = cb.getPlayerID().value_or(PlayerColor::NEUTRAL);

	const auto addObject = [&](const CGObjectInstance * obj)
	{
		if(!obj || obj->ID == Obj::EVENT)
			return;
		if(!obj->isVisitable())
			return;
		if(!inViewport(view, obj->visitablePos()))
			return;
		if(!seen.insert(obj->id.getNum()).second)
			return;

		MapObject item;
		item.id = obj->id.getNum();
		item.pos = obj->visitablePos();
		item.kind = mapObjectKind(obj->ID);
		item.name = objectLabel(*obj, viewer, translator);
		item.owner = ownerText(obj->tempOwner);
		if(const auto * armed = dynamic_cast<const CArmedInstance *>(obj))
		{
			if(armed->stacksCount() > 0)
				item.army = armed->getArmyDescription().toString(&translator);
		}
		listed.push_back(std::move(item));
	};

	for(int y = view.minY; y <= view.maxY; ++y)
	{
		for(int x = view.minX; x <= view.maxX; ++x)
		{
			const int3 pos(x, y, view.z);
			const TerrainTile * tile = cb.tryGetTile(pos);
			if(!tile)
				continue;
			for(const ObjectInstanceID objId : tile->visitableObjects)
				addObject(cb.tryGetObj(objId));
		}
	}

	std::sort(listed.begin(), listed.end(), [](const MapObject & left, const MapObject & right)
	{
		if(left.pos.y != right.pos.y)
			return left.pos.y < right.pos.y;
		if(left.pos.x != right.pos.x)
			return left.pos.x < right.pos.x;
		return left.id < right.id;
	});

	if(listed.empty())
	{
		out += "objects: none\n";
		return;
	}

	for(const MapObject & item : listed)
	{
		out += "object";
		out += " " + field("kind", item.kind);
		out += " " + field("id", std::to_string(item.id));
		out += " " + field("pos", posText(item.pos));
		out += " " + field("owner", item.owner);
		out += " " + field("name", item.name);
		if(!item.army.empty())
			out += " " + field("army", item.army);
		out += "\n";
	}
}

const char * reachKind(EPathNodeAction action)
{
	switch(action)
	{
		case EPathNodeAction::BATTLE:
		case EPathNodeAction::TELEPORT_BATTLE:
			return "battle";
		case EPathNodeAction::VISIT:
		case EPathNodeAction::BLOCKING_VISIT:
		case EPathNodeAction::TELEPORT_BLOCKING_VISIT:
			return "visit";
		default:
			return "walk";
	}
}

void appendReachableTiles(std::string & out, CPlayerInterface & interface, const MapViewport & view)
{
	const CGHeroInstance * hero = interface.localState->getCurrentHero();
	if(!hero && !interface.localState->getWanderingHeroes().empty())
		hero = interface.localState->getWanderingHeroes().front();
	if(!hero || hero->isGarrisoned())
		return;

	const auto paths = interface.getPathsInfo(hero);
	if(!paths)
		return;

	const int3 heroPos = hero->visitablePos();
	std::map<std::string, std::vector<int3>> groups;

	for(int y = view.minY; y <= view.maxY; ++y)
	{
		for(int x = view.minX; x <= view.maxX; ++x)
		{
			const int3 pos(x, y, view.z);
			if(pos == heroPos)
				continue;
			const CGPathNode * node = paths->getPathInfo(pos);
			if(!node || node->turns != 0 || node->action == EPathNodeAction::UNKNOWN)
				continue;

			groups[reachKind(node->action)].push_back(pos);
		}
	}

	if(groups.empty())
	{
		out += "reach " + field("hero", std::to_string(hero->id.getNum())) + " tiles=none\n";
		return;
	}

	for(const char * kind : { "walk", "visit", "battle" })
	{
		const auto found = groups.find(kind);
		if(found == groups.end() || found->second.empty())
			continue;

		std::string coords;
		int runY = -1;
		int runStartX = -1;
		int runEndX = -1;
		const auto flushRun = [&]()
		{
			if(runStartX < 0)
				return;
			if(!coords.empty())
				coords += " ";
			coords += xyRange(runStartX, runEndX, runY);
			runStartX = -1;
			runEndX = -1;
			runY = -1;
		};
		for(const int3 & pos : found->second)
		{
			if(runStartX >= 0 && pos.y == runY && pos.x == runEndX + 1)
			{
				runEndX = pos.x;
				continue;
			}
			flushRun();
			runY = pos.y;
			runStartX = pos.x;
			runEndX = pos.x;
		}
		flushRun();

		out += "reach";
		out += " " + field("hero", std::to_string(hero->id.getNum()));
		out += " " + field("kind", kind);
		out += " " + field("tiles", coords);
		out += "\n";
	}
}

}

bool mcpBattleIsBusy()
{
	if(!GAME || !GAME->interface() || !GAME->interface()->cb)
		return false;

	CCallback & cb = *GAME->interface()->cb;
	std::shared_lock stateLock(CGameState::mutex);

	const auto battles = cb.getActiveBattles();
	if(battles.empty() || !battles.begin()->second)
		return false;

	const BattleID battleID = battles.begin()->first;
	const CPlayerBattleCallback & battle = *battles.begin()->second;
	const std::shared_ptr<BattleInterface> battleUi = CPlayerInterface::battleInt;
	if(!battleUi || battleUi->getBattleID() != battleID)
		return true;

	if(battleUi->hasAnimations())
		return true;

	const battle::Unit * active = battle.battleActiveUnit();
	if(!active)
		return true;

	if(active->unitSide() != battle.battleGetMySide())
		return false;

	return !battleUi->makingTurn();
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

	appendMcpDialogs(out, interface);

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
			appendHeroIndex(out, *hero, translator);
		}
		catch(const std::exception & error)
		{
			appendCaught(out, "hero", hero->id.getNum(), error);
		}
	}

	std::vector<const CGTownInstance *> towns;
	std::set<int> townIds;
	const auto addTown = [&](const CGTownInstance * town)
	{
		if(!town)
			return;
		if(!townIds.insert(town->id.getNum()).second)
			return;
		towns.push_back(town);
	};

	for(const CGTownInstance * town : cb.getTownsInfo(false))
		addTown(town);

	for(const CGObjectInstance * obj : cb.getAllVisitableObjs())
	{
		if(obj->ID == Obj::TOWN || obj->ID == Obj::RANDOM_TOWN)
			addTown(dynamic_cast<const CGTownInstance *>(obj));
	}

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
			appendTownIndex(out, cb, *town, translator);
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

	return out;
}

std::string buildMcpBattleState()
{
	if(!GAME || !GAME->interface() || !GAME->interface()->cb)
		return "No game is in progress.\n";

	std::shared_lock stateLock(CGameState::mutex);

	CCallback & cb = *GAME->interface()->cb;
	const ITranslator & translator = GAME->translator();

	std::string out;
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

std::string buildMcpMapState()
{
	if(!GAME || !GAME->interface() || !GAME->interface()->cb)
		return "No game is in progress.\n";

	std::shared_lock stateLock(CGameState::mutex);

	CPlayerInterface & interface = *GAME->interface();
	CCallback & cb = *interface.cb;
	const ITranslator & translator = GAME->translator();
	const int3 mapSize = cb.getMapSize();
	const MapViewport view = cameraViewport(cb, interface);

	std::string out;
	out += "map";
	out += " " + field("width", std::to_string(mapSize.x));
	out += " " + field("height", std::to_string(mapSize.y));
	out += " " + field("levels", std::to_string(mapSize.z));
	out += " " + field("z", std::to_string(view.z));
	out += " " + field("view", std::to_string(view.minX) + "," + std::to_string(view.minY) + "-" + std::to_string(view.maxX) + "," + std::to_string(view.maxY));
	out += "\n";
	appendViewportTerrain(out, cb, view);
	appendViewportObjects(out, cb, view, translator);
	appendReachableTiles(out, interface, view);
	return out;
}

std::string buildMcpHeroState(std::optional<int> heroId)
{
	if(!GAME || !GAME->interface() || !GAME->interface()->cb)
		return "error: no game is in progress\n";

	std::shared_lock stateLock(CGameState::mutex);

	CPlayerInterface & interface = *GAME->interface();
	CCallback & cb = *interface.cb;
	const ITranslator & translator = GAME->translator();
	const auto heroes = cb.getHeroesInfo();

	const CGHeroInstance * hero = nullptr;
	if(heroId)
	{
		for(const CGHeroInstance * candidate : heroes)
		{
			if(candidate && candidate->id.getNum() == *heroId)
			{
				hero = candidate;
				break;
			}
		}
		if(!hero)
			return "error: no hero with id " + std::to_string(*heroId) + "\n";
	}
	else
	{
		hero = interface.localState->getCurrentHero();
		if(!hero)
		{
			for(const CGHeroInstance * candidate : heroes)
			{
				if(candidate)
				{
					hero = candidate;
					break;
				}
			}
		}
		if(!hero)
			return "error: you have no hero\n";
	}

	std::string out;
	appendHeroDetail(out, *hero, translator);
	return out;
}

std::string buildMcpTownState(int townId)
{
	if(!GAME || !GAME->interface() || !GAME->interface()->cb)
		return "error: no game is in progress\n";

	std::shared_lock stateLock(CGameState::mutex);

	CCallback & cb = *GAME->interface()->cb;
	const ITranslator & translator = GAME->translator();

	const CGTownInstance * town = nullptr;
	const auto add = [&](const CGTownInstance * candidate)
	{
		if(candidate && candidate->id.getNum() == townId)
			town = candidate;
	};
	for(const CGTownInstance * candidate : cb.getTownsInfo(false))
		add(candidate);
	if(!town)
	{
		for(const CGObjectInstance * obj : cb.getAllVisitableObjs())
		{
			if(obj && (obj->ID == Obj::TOWN || obj->ID == Obj::RANDOM_TOWN))
				add(dynamic_cast<const CGTownInstance *>(obj));
		}
	}
	if(!town)
		return "error: no town with id " + std::to_string(townId) + "\n";

	std::string out;
	appendTownDetail(out, cb, *town, translator);
	return out;
}
