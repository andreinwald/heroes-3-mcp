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

#include <optional>
#include <string>

class CPlayerInterface;

/// Blocking dialogs visible to the agent (top window first).
void appendMcpDialogs(std::string & out, CPlayerInterface & interface);

/// Apply choice from get_state on the top blocking dialog.
/// questionOverride sends QuestionAnswer to the server when the UI has no matching window.
std::string applyMcpDialogAnswer(int choice, std::optional<int> questionOverride = std::nullopt);

/// Adventure actions must wait while this is true (recruitment, info dialogs, etc.).
bool mcpHasBlockingDialog();
