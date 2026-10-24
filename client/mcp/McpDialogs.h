/*
 * McpDialogs.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include <string>

class CPlayerInterface;

/// Blocking dialogs visible to the agent (top window first).
void appendMcpDialogs(std::string & out, CPlayerInterface & interface);

/// Apply choice from get_state on the top blocking dialog.
std::string applyMcpDialogAnswer(int choice);
