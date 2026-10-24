/*
 * McpBattleWaiter.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "McpBattleWaiter.h"

McpBattleWaiter & McpBattleWaiter::instance()
{
	static McpBattleWaiter waiter;
	return waiter;
}

void McpBattleWaiter::arm()
{
	std::lock_guard lock(mutex);
	armed = true;
	signaled = false;
}

void McpBattleWaiter::notifyReady()
{
	std::lock_guard lock(mutex);
	if(!armed)
		return;
	signaled = true;
	armed = false;
	ready.notify_all();
}

bool McpBattleWaiter::waitUntilReady(std::chrono::milliseconds timeout)
{
	std::unique_lock lock(mutex);
	if(signaled)
		return true;
	if(!armed)
		return true;

	const bool woke = ready.wait_for(lock, timeout, [this] { return signaled || !armed; });
	if(woke && signaled)
		return true;

	armed = false;
	return false;
}

void McpBattleWaiter::cancel()
{
	std::lock_guard lock(mutex);
	armed = false;
	signaled = false;
	ready.notify_all();
}
