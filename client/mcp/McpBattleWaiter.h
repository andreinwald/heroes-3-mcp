/*
 * McpBattleWaiter.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

/// Lets an MCP battle tool wait off the GUI thread until the player can act again.
class McpBattleWaiter
{
	std::mutex mutex;
	std::condition_variable ready;
	bool armed = false;
	bool signaled = false;

public:
	static McpBattleWaiter & instance();

	/// Call on the GUI thread immediately before sending a battle action.
	void arm();

	/// Call on the GUI thread when the next stack is ready, a dialog opened, or the battle ended.
	void notifyReady();

	/// Call from the MCP HTTP thread after the action was sent. Returns false on timeout.
	bool waitUntilReady(std::chrono::milliseconds timeout);

	void cancel();
};
