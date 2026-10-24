/*
 * McpDialogs.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "McpDialogs.h"

#include "CPlayerInterface.h"
#include "GameEngine.h"
#include "GameInstance.h"
#include "battle/BattleResultWindow.h"
#include "gui/WindowHandler.h"
#include "widgets/CComponent.h"
#include "widgets/TextControls.h"
#include "windows/CCreatureWindow.h"
#include "windows/CExchangeWindow.h"
#include "windows/CTutorialWindow.h"
#include "windows/GUIClasses.h"
#include "windows/InfoWindows.h"

#include "../lib/ConditionalWait.h"
#include "../lib/CSkillHandler.h"
#include "../lib/GameLibrary.h"
#include "../lib/callback/CCallback.h"
#include "../lib/constants/EntityIdentifiers.h"
#include "../lib/entities/ResourceTypeHandler.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/mapObjects/CGHeroInstance.h"

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

std::string componentTypeName(ComponentType type)
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

void appendBattleResult(std::string & out, const BattleResultWindow & window)
{
	out += "dialog blocking=yes type=battle_result\n";
	out += "  option " + field("choice", "0") + " name=ok\n";
	if(window.hasReplayButton())
		out += "  option " + field("choice", "1") + " name=repeat\n";
}

void appendLevelUp(std::string & out, const CLevelWindow & window)
{
	out += "dialog blocking=yes type=level_up";
	if(const CGHeroInstance * hero = window.levelUpHero())
		out += " " + field("hero", GAME->translator().translate(hero->getNameTextID()));
	out += "\n";

	const auto & skills = window.levelUpSkills();
	for(int index = 0; index < static_cast<int>(skills.size()); ++index)
	{
		const SecondarySkill skill = skills[index];
		std::string name = LIBRARY->skillh->getById(skill)->getNameTranslated();
		out += "  option " + field("choice", std::to_string(index)) + " " + field("name", name) + "\n";
	}
	if(skills.empty())
		out += "  option " + field("choice", "0") + " name=ok\n";
}

void appendCommanderLevelUp(std::string & out, const CStackWindow & window)
{
	out += "dialog blocking=yes type=commander_level_up\n";
	for(const ui32 skillId : window.commanderLevelUpSkillChoices())
	{
		const std::string name = window.commanderLevelUpSkillLabel(skillId);
		out += "  option " + field("choice", std::to_string(skillId)) + " " + field("name", name) + "\n";
	}
	if(window.commanderLevelUpSkillChoices().empty())
		out += "  option " + field("choice", "0") + " name=ok\n";
}

void appendTutorial(std::string & out)
{
	out += "dialog blocking=yes type=tutorial\n";
	out += "  option " + field("choice", "1") + " name=ok\n";
}

void appendExchange(std::string & out, const CExchangeWindow & window)
{
	out += "dialog blocking=yes type=exchange";
	if(window.getQuestionID().getNum() >= 0)
		out += " " + field("question", std::to_string(window.getQuestionID().getNum()));
	out += "\n";
	out += "  option " + field("choice", "0") + " name=leave\n";
}

void appendRecruitment(std::string & out, const CRecruitmentWindow & window)
{
	out += "dialog blocking=yes type=recruitment\n";
	out += "  text: \"Recruit creatures or leave to continue.\"\n";
	out += "  option " + field("choice", "0") + " name=leave\n";
}

void appendUnknownBusy(std::string & out)
{
	out += "dialog blocking=yes type=unknown\n";
	out += "  option " + field("choice", "1") + " name=ok\n";
}

bool appendTopDialog(std::string & out, CPlayerInterface & interface)
{
	if(!ENGINE)
		return false;

	if(const auto window = ENGINE->windows().topWindow<BattleResultWindow>())
	{
		appendBattleResult(out, *window);
		return true;
	}

	if(const auto window = ENGINE->windows().topWindow<CLevelWindow>())
	{
		appendLevelUp(out, *window);
		return true;
	}

	if(const auto window = ENGINE->windows().topWindow<CStackWindow>())
	{
		if(window->isCommanderLevelUpDialog())
		{
			appendCommanderLevelUp(out, *window);
			return true;
		}
	}

	if(const auto window = ENGINE->windows().topWindow<CTutorialWindow>())
	{
		appendTutorial(out);
		return true;
	}

	if(const auto window = ENGINE->windows().topWindow<CRecruitmentWindow>())
	{
		appendRecruitment(out, *window);
		return true;
	}

	if(const auto window = ENGINE->windows().topWindow<CInfoWindow>())
	{
		appendInfoWindow(out, *window);
		return true;
	}

	if(const auto window = ENGINE->windows().topWindow<CExchangeWindow>())
	{
		appendExchange(out, *window);
		return true;
	}

	if(interface.showingDialog && interface.showingDialog->isBusy())
	{
		appendUnknownBusy(out);
		return true;
	}

	return false;
}

std::string answerInfoWindow(CInfoWindow & window, int choice, CCallback & cb)
{
	const bool choose = dynamic_cast<CSelWindow *>(&window) != nullptr;
	const bool yesNo = !choose && window.buttons.size() >= 2;
	const int question = window.ID.getNum();
	int minChoice = 1;
	int maxChoice = 1;

	if(choose)
	{
		maxChoice = window.components ? static_cast<int>(window.components->getComponents().size()) : 0;
		minChoice = window.buttons.size() > 1 ? 0 : 1;
	}
	else if(yesNo)
	{
		minChoice = 0;
		maxChoice = 1;
	}

	if(choice < minChoice || choice > maxChoice)
		return "error: choice must be between " + std::to_string(minChoice) + " and " + std::to_string(maxChoice);

	return "answer " + field("choice", std::to_string(choice))
		+ (question >= 0 ? " " + field("question", std::to_string(question)) : "")
		+ " type=info\n";
}

}

void appendMcpDialogs(std::string & out, CPlayerInterface & interface)
{
	appendTopDialog(out, interface);
}

bool mcpHasBlockingDialog()
{
	if(!ENGINE || !GAME || !GAME->interface())
		return false;

	CPlayerInterface & interface = *GAME->interface();
	if(interface.showingDialog && interface.showingDialog->isBusy())
		return true;

	const auto & windows = ENGINE->windows();
	if(windows.topWindow<BattleResultWindow>())
		return true;
	if(windows.topWindow<CLevelWindow>())
		return true;
	if(const auto stack = windows.topWindow<CStackWindow>(); stack && stack->isCommanderLevelUpDialog())
		return true;
	if(windows.topWindow<CTutorialWindow>())
		return true;
	if(windows.topWindow<CRecruitmentWindow>())
		return true;
	if(windows.topWindow<CInfoWindow>())
		return true;
	if(windows.topWindow<CExchangeWindow>())
		return true;
	return false;
}

std::string applyMcpDialogAnswer(int choice, std::optional<int> questionOverride)
{
	if(!GAME || !GAME->interface() || !GAME->interface()->cb || !ENGINE)
		return "error: no game is in progress";

	CPlayerInterface & interface = *GAME->interface();
	CCallback & cb = *interface.cb;

	std::shared_lock stateLock(CGameState::mutex);

	if(const auto window = ENGINE->windows().topWindow<BattleResultWindow>())
	{
		const int maxChoice = window->hasReplayButton() ? 1 : 0;
		if(choice < 0 || choice > maxChoice)
			return "error: choice must be between 0 and " + std::to_string(maxChoice);

		const std::string result = "answer " + field("choice", std::to_string(choice)) + " type=battle_result\n";
		stateLock.unlock();
		if(choice == 0)
			window->bExitf();
		else
			window->bRepeatf();
		return result;
	}

	if(const auto window = ENGINE->windows().topWindow<CLevelWindow>())
	{
		const int maxChoice = window->levelUpSkills().empty() ? 0 : static_cast<int>(window->levelUpSkills().size()) - 1;
		if(window->levelUpSkills().empty())
		{
			if(choice != 0)
				return "error: choice must be 0";
		}
		else if(choice < 0 || choice > maxChoice)
			return "error: choice must be between 0 and " + std::to_string(maxChoice);

		const std::string result = "answer " + field("choice", std::to_string(choice)) + " type=level_up\n";
		stateLock.unlock();
		if(!window->applyMcpLevelUpChoice(choice))
			return "error: could not apply level-up choice";
		return result;
	}

	if(const auto window = ENGINE->windows().topWindow<CStackWindow>())
	{
		if(window->isCommanderLevelUpDialog())
		{
			const auto skills = window->commanderLevelUpSkillChoices();
			if(skills.empty())
			{
				if(choice != 0)
					return "error: choice must be 0";
			}
			else if(!vstd::contains(skills, static_cast<ui32>(choice)))
				return "error: choice must be one of the listed commander skill ids";

			const std::string result = "answer " + field("choice", std::to_string(choice)) + " type=commander_level_up\n";
			stateLock.unlock();
			if(!window->applyMcpCommanderLevelUpChoice(static_cast<ui32>(choice)))
				return "error: could not apply commander level-up choice";
			return result;
		}
	}

	if(const auto window = ENGINE->windows().topWindow<CTutorialWindow>())
	{
		if(choice != 1)
			return "error: tutorial only supports choice=1 (ok)";
		const std::string result = "answer choice=1 type=tutorial\n";
		stateLock.unlock();
		window->dismissForMcp();
		return result;
	}

	if(const auto window = ENGINE->windows().topWindow<CRecruitmentWindow>())
	{
		if(choice != 0)
			return "error: recruitment only supports choice=0 (leave); recruiting from MCP is not supported yet";
		const std::string result = "answer choice=0 type=recruitment\n";
		stateLock.unlock();
		window->close();
		return result;
	}

	if(auto window = ENGINE->windows().topWindow<CInfoWindow>())
	{
		std::string result = answerInfoWindow(*window, choice, cb);
		if(result.starts_with("error:"))
			return result;
		const int question = window->ID.getNum();
		stateLock.unlock();
		if(question >= 0)
			cb.selectionMade(choice, window->ID);
		window->close();
		return result;
	}

	if(const auto window = ENGINE->windows().topWindow<CExchangeWindow>())
	{
		if(choice != 0)
			return "error: exchange window only supports choice=0 (leave)";
		const QuestionID question = window->getQuestionID();
		const std::string result = "answer choice=0 type=exchange\n";
		stateLock.unlock();
		if(question.getNum() > 0)
			cb.selectionMade(0, question);
		window->close();
		return result;
	}

	if(interface.showingDialog && interface.showingDialog->isBusy())
		return "error: a dialog is open but the agent cannot act on it yet (type=unknown)";

	if(questionOverride)
	{
		if(*questionOverride < 0)
			return "error: question must be a non-negative server question id";
		const std::string result = "answer " + field("choice", std::to_string(choice))
			+ " " + field("question", std::to_string(*questionOverride)) + " type=question\n";
		stateLock.unlock();
		cb.selectionMade(choice, QuestionID(*questionOverride));
		return result;
	}

	return "error: no dialog is open";
}
