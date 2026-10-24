/*
 * McpServer.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include <cstdint>
#include <memory>

/// Local MCP endpoint for the human client. Listens on 127.0.0.1 while the game window is open.
class McpServer
{
	struct Impl;
	std::unique_ptr<Impl> impl;

	McpServer();

public:
	static constexpr std::uint16_t port = 47321;

	~McpServer();

	static McpServer & instance();

	void start();

	/// Runs queued tool calls on the GUI thread. Call from the main loop.
	void drain();

	void stop();
};
