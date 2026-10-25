/*
 * McpServer.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "McpServer.h"

#include "GameEngine.h"
#include "GameActions.h"
#include "GameStateSnapshot.h"
#include "McpBattleWaiter.h"

#include "../lib/CThreadHelper.h"
#include "../lib/json/JsonNode.h"

#include <boost/asio.hpp>

#include <deque>

#ifdef _WIN32
#ifdef AUTO
#undef AUTO
#endif
#ifdef IGNORE
#undef IGNORE
#endif
#endif

namespace
{

struct HttpRequest
{
	std::string method;
	std::string target;
	std::string body;
};

struct HttpResponse
{
	int status = 200;
	std::string reason = "OK";
	std::string body;
	std::string contentType = "application/json";
};

void completePromise(const std::shared_ptr<std::promise<std::string>> & promise, std::string value)
{
	try
	{
		promise->set_value(std::move(value));
	}
	catch(const std::future_error &)
	{
	}
}

std::optional<HttpRequest> readHttpRequest(boost::asio::ip::tcp::socket & socket)
{
	boost::asio::streambuf buffer;
	boost::system::error_code error;
	boost::asio::read_until(socket, buffer, "\r\n\r\n", error);
	if(error)
		return std::nullopt;

	const std::string raw(boost::asio::buffers_begin(buffer.data()), boost::asio::buffers_end(buffer.data()));
	const auto headerEnd = raw.find("\r\n\r\n");
	if(headerEnd == std::string::npos)
		return std::nullopt;

	const std::string header = raw.substr(0, headerEnd);
	std::string body = raw.substr(headerEnd + 4);

	std::istringstream lines(header);
	std::string requestLine;
	if(!std::getline(lines, requestLine))
		return std::nullopt;
	if(!requestLine.empty() && requestLine.back() == '\r')
		requestLine.pop_back();

	HttpRequest request;
	std::istringstream requestLineStream(requestLine);
	requestLineStream >> request.method >> request.target;

	std::size_t contentLength = 0;
	std::string headerLine;
	while(std::getline(lines, headerLine))
	{
		if(!headerLine.empty() && headerLine.back() == '\r')
			headerLine.pop_back();

		const auto colon = headerLine.find(':');
		if(colon == std::string::npos)
			continue;

		std::string name = headerLine.substr(0, colon);
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c)
		{
			return static_cast<char>(std::tolower(c));
		});
		if(name == "content-length")
			contentLength = static_cast<std::size_t>(std::strtoul(headerLine.c_str() + colon + 1, nullptr, 10));
	}

	if(contentLength > 1024 * 1024)
		return std::nullopt;

	if(body.size() < contentLength)
	{
		std::string rest(contentLength - body.size(), '\0');
		boost::asio::read(socket, boost::asio::buffer(rest), error);
		if(error)
			return std::nullopt;
		body += rest;
	}
	else if(body.size() > contentLength)
	{
		body.resize(contentLength);
	}

	request.body = std::move(body);
	return request;
}

void writeHttpResponse(boost::asio::ip::tcp::socket & socket, const HttpResponse & response)
{
	std::ostringstream out;
	out << "HTTP/1.1 " << response.status << ' ' << response.reason << "\r\n";
	if(!response.contentType.empty())
		out << "Content-Type: " << response.contentType << "\r\n";
	out << "Content-Length: " << response.body.size() << "\r\n";
	out << "Access-Control-Allow-Origin: *\r\n";
	out << "Access-Control-Allow-Headers: Content-Type, Accept, Mcp-Session-Id\r\n";
	out << "Access-Control-Allow-Methods: POST, OPTIONS\r\n";
	out << "Connection: close\r\n\r\n";
	out << response.body;

	boost::system::error_code error;
	boost::asio::write(socket, boost::asio::buffer(out.str()), error);
}

std::string jsonError(const JsonNode & request, int code, const std::string & message)
{
	JsonNode response;
	response["jsonrpc"].String() = "2.0";
	if(request.isStruct() && !request["id"].isNull())
		response["id"] = request["id"];
	response["error"]["code"].Integer() = code;
	response["error"]["message"].String() = message;
	return response.toCompactString();
}

std::string jsonResult(const JsonNode & request, JsonNode result)
{
	JsonNode response;
	response["jsonrpc"].String() = "2.0";
	if(request.isStruct() && !request["id"].isNull())
		response["id"] = request["id"];
	response["result"] = std::move(result);
	return response.toCompactString();
}

std::string jsonToolResult(const JsonNode & request, const std::string & text, bool isError)
{
	JsonNode result;
	JsonNode part;
	part["type"].String() = "text";
	part["text"].String() = text;
	result["content"].Vector().push_back(std::move(part));
	result["isError"].Bool() = isError;
	return jsonResult(request, std::move(result));
}

JsonNode parseJson(const std::string & body)
{
	JsonParsingSettings parsing;
	parsing.mode = JsonParsingSettings::JsonFormatMode::JSON;
	parsing.strict = true;
	return JsonNode(body.data(), body.size(), parsing, std::string("mcp"));
}

JsonNode jsonString(const std::string & value)
{
	JsonNode node;
	node.String() = value;
	return node;
}

JsonNode integerProperty(const std::string & description)
{
	JsonNode property;
	property["type"].String() = "integer";
	property["description"].String() = description;
	return property;
}

std::optional<int> readInt(const JsonNode & node, std::string & error, const std::string & name, bool required)
{
	if(node.isNull())
	{
		if(required)
			error = "error: " + name + " is required";
		return std::nullopt;
	}
	if(!node.isNumber())
	{
		error = "error: " + name + " must be an integer";
		return std::nullopt;
	}

	const si64 value = node.Integer();
	if(value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
	{
		error = "error: " + name + " is out of range";
		return std::nullopt;
	}
	return static_cast<int>(value);
}

}

struct McpServer::Impl
{
	struct Job
	{
		std::function<std::string()> work;
		std::shared_ptr<std::promise<std::string>> promise;
	};

	std::mutex queueMutex;
	std::deque<Job> queue;
	std::atomic<bool> stopping = false;

	boost::asio::io_context io;
	std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor;
	std::thread thread;

	std::string callOnGui(const std::function<std::string()> & work)
	{
		if(ENGINE && ENGINE->amIGuiThread())
			return work();

		auto promise = std::make_shared<std::promise<std::string>>();
		{
			std::lock_guard lock(queueMutex);
			if(stopping)
				return "MCP server is stopping";
			queue.push_back(Job{work, promise});
		}

		auto future = promise->get_future();
		if(future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
			return "Timed out waiting for the game. It is busy or still starting.";
		return future.get();
	}

	static constexpr auto BATTLE_IDLE_TIMEOUT = std::chrono::seconds(90);

	bool waitForBattleIdle()
	{
		auto status = callOnGui([] { return mcpBattleIsBusy() ? "busy" : "idle"; });
		if(status == "idle")
			return true;
		if(status != "busy")
			return false;

		McpBattleWaiter::instance().arm();
		status = callOnGui([] { return mcpBattleIsBusy() ? "busy" : "idle"; });
		if(status != "busy")
		{
			McpBattleWaiter::instance().cancel();
			return status == "idle";
		}
		return McpBattleWaiter::instance().waitUntilReady(BATTLE_IDLE_TIMEOUT);
	}

	std::string completeBattleTool(std::string text)
	{
		if(text.rfind("error:", 0) == 0 || text.rfind("Timed out", 0) == 0 || text.rfind("MCP server", 0) == 0)
			return text;

		if(!McpBattleWaiter::instance().waitUntilReady(BATTLE_IDLE_TIMEOUT))
			text += "warning: timed out waiting for the next battle decision\n";

		text += callOnGui([] { return buildMcpBattleState(); });
		return text;
	}

	std::string runBattleAction(const std::function<std::string()> & action)
	{
		waitForBattleIdle();
		std::string text = callOnGui(action);
		if(text.find("battle animations are playing") != std::string::npos
			|| text.find("wait for the current battle action to finish") != std::string::npos)
		{
			waitForBattleIdle();
			text = callOnGui(action);
		}
		return completeBattleTool(text);
	}

	HttpResponse handleJson(const std::string & body)
	{
		JsonNode request;
		try
		{
			request = parseJson(body);
		}
		catch(const std::exception & error)
		{
			HttpResponse response;
			response.body = jsonError(JsonNode(), -32700, error.what());
			return response;
		}

		if(!request.isStruct() || !request["method"].isString())
		{
			HttpResponse response;
			response.body = jsonError(request, -32600, "Invalid request");
			return response;
		}

		if(request["id"].isNull())
		{
			HttpResponse response;
			response.status = 202;
			response.reason = "Accepted";
			response.contentType.clear();
			return response;
		}

		const std::string method = request["method"].String();
		HttpResponse response;

		if(method == "initialize")
		{
			JsonNode result;
			result["protocolVersion"].String() = "2024-11-05";
			result["capabilities"]["tools"]["listChanged"].Bool() = false;
			result["serverInfo"]["name"].String() = "vcmi";
			result["serverInfo"]["version"].String() = "0.1.0";
			response.body = jsonResult(request, std::move(result));
			return response;
		}

		if(method == "ping")
		{
			JsonNode result;
			result.Struct();
			response.body = jsonResult(request, std::move(result));
			return response;
		}

		if(method == "tools/list")
		{
			JsonNode state;
			state["name"].String() = "get_state";
			state["description"].String() = "What is happening now: date, resources, activity (your_turn|battle|waiting), whether a hero is moving, blocking dialogs with numbered choices, a short hero list (id, name, pos, move), visible towns (id, pos, owner, relation), and other map objects. Army, skills, and buildings are in get_hero/get_town. Camera, roads, and reachable tiles are in get_map. Battle stacks, hexes, and legal actions are in get_battle.";
			state["inputSchema"]["type"].String() = "object";
			state["inputSchema"]["properties"].Struct();

			JsonNode map;
			map["name"].String() = "get_map";
			map["description"].String() = "Adventure-map camera: view rectangle, terrain+walk (land/water/blocked/rock) with roads and rivers, fog count, visitable objects in that rectangle, and tiles the current hero can reach this turn (reach kind=walk|visit|battle). Decorative trees are omitted. Hero/town details stay in get_hero/get_town. Battle stays in get_battle.";
			map["inputSchema"]["type"].String() = "object";
			map["inputSchema"]["properties"].Struct();

			JsonNode battle;
			battle["name"].String() = "get_battle";
			battle["description"].String() = "Current battle: round, terrain, active stack (active=yes), whose turn (active_turn=yours|enemy|busy|none), battle heroes, stacks (id, side, creature, count, hp, hex, shots, waited), and a can line listing legal attack target ids plus wait=yes|no and defend=yes. Waits until battle animations finish. Use stack ids from here for attack. Returns battle: none when not in combat.";
			battle["inputSchema"]["type"].String() = "object";
			battle["inputSchema"]["properties"].Struct();

			JsonNode hero;
			hero["name"].String() = "get_hero";
			hero["description"].String() = "One owned hero in detail: army, mana, primary skills, secondary skills, artifacts, spellbook. Omit hero to use the selected adventure-map hero, or the first owned hero.";
			hero["inputSchema"]["type"].String() = "object";
			hero["inputSchema"]["properties"]["hero"] = integerProperty("Hero id from get_state. Omit for the selected hero.");

			JsonNode town;
			town["name"].String() = "get_town";
			town["description"].String() = "One visible town in detail: army, visiting and garrison heroes, buildings, and recruitable creatures.";
			town["inputSchema"]["type"].String() = "object";
			town["inputSchema"]["properties"]["town"] = integerProperty("Town id from get_state.");
			town["inputSchema"]["required"].Vector().push_back(jsonString("town"));

			JsonNode move;
			move["name"].String() = "move_hero";
			move["description"].String() = "Walk the selected hero toward a map tile. Uses the hero's remaining movement and stops at a monster to attack it. z defaults to the hero's floor. hero defaults to the hero selected on the adventure map, or your first hero.";
			move["inputSchema"]["type"].String() = "object";
			move["inputSchema"]["properties"]["x"] = integerProperty("Map column.");
			move["inputSchema"]["properties"]["y"] = integerProperty("Map row.");
			move["inputSchema"]["properties"]["z"] = integerProperty("Map floor, 0 for the surface.");
			move["inputSchema"]["properties"]["hero"] = integerProperty("Hero id from get_state. Omit to move the current hero.");
			move["inputSchema"]["required"].Vector().push_back(jsonString("x"));
			move["inputSchema"]["required"].Vector().push_back(jsonString("y"));

			JsonNode attack;
			attack["name"].String() = "attack";
			attack["description"].String() = "In a battle, attack an enemy stack with the active unit (stack active=yes in get_battle). Shoots when the unit can shoot. Otherwise walks up and strikes in melee. Waits until battle animations finish, then until the next player decision. If attack fails as out of range, use defend or wait instead of repeating attack. Returns a get_battle snapshot.";
			attack["inputSchema"]["type"].String() = "object";
			attack["inputSchema"]["properties"]["stack"] = integerProperty("Enemy stack id from get_battle.");
			attack["inputSchema"]["required"].Vector().push_back(jsonString("stack"));

			JsonNode answer;
			answer["name"].String() = "answer";
			answer["description"].String() = "Pick an option on the top blocking dialog from get_state (info/choose/yes_no, battle_result ok=0 repeat=1, level_up skill index, commander_level_up skill id, exchange leave=0, tutorial ok=1).";
			answer["inputSchema"]["type"].String() = "object";
			answer["inputSchema"]["properties"]["choice"] = integerProperty("Option number from get_state.");
			answer["inputSchema"]["properties"]["question"] = integerProperty("Optional server question id from get_state or error text; use with choice to clear a stuck OpenWindow activity.");
			answer["inputSchema"]["required"].Vector().push_back(jsonString("choice"));

			JsonNode wait;
			wait["name"].String() = "wait";
			wait["description"].String() = "In a battle, wait with the active stack so another unit can act first this round. Returns after the next player decision, with a get_battle snapshot.";
			wait["inputSchema"]["type"].String() = "object";
			wait["inputSchema"]["properties"].Struct();

			JsonNode defend;
			defend["name"].String() = "defend";
			defend["description"].String() = "In a battle, defend with the active stack, ending its turn for this round. Returns after the next player decision, with a get_battle snapshot.";
			defend["inputSchema"]["type"].String() = "object";
			defend["inputSchema"]["properties"].Struct();

			JsonNode endTurn;
			endTurn["name"].String() = "end_turn";
			endTurn["description"].String() = "End your adventure turn and advance to the next day. Fails during battle, while a dialog is open, or while a hero is moving.";
			endTurn["inputSchema"]["type"].String() = "object";
			endTurn["inputSchema"]["properties"].Struct();

			JsonNode result;
			result["tools"].Vector().push_back(std::move(state));
			result["tools"].Vector().push_back(std::move(map));
			result["tools"].Vector().push_back(std::move(battle));
			result["tools"].Vector().push_back(std::move(hero));
			result["tools"].Vector().push_back(std::move(town));
			result["tools"].Vector().push_back(std::move(move));
			result["tools"].Vector().push_back(std::move(attack));
			result["tools"].Vector().push_back(std::move(wait));
			result["tools"].Vector().push_back(std::move(defend));
			result["tools"].Vector().push_back(std::move(endTurn));
			result["tools"].Vector().push_back(std::move(answer));
			response.body = jsonResult(request, std::move(result));
			return response;
		}

		if(method == "tools/call")
		{
			const JsonNode & params = request["params"];
			if(!params.isStruct() || !params["name"].isString())
			{
				response.body = jsonError(request, -32602, "tools/call requires params.name");
				return response;
			}

			const std::string name = params["name"].String();
			const JsonNode & arguments = params["arguments"];
			if(!arguments.isNull() && !arguments.isStruct())
			{
				response.body = jsonToolResult(request, "error: arguments must be an object", true);
				return response;
			}

			std::string text;
			if(name == "get_state")
			{
				text = callOnGui([] { return buildMcpGameState(); });
			}
			else if(name == "get_map")
			{
				text = callOnGui([] { return buildMcpMapState(); });
			}
			else if(name == "get_battle")
			{
				if(!waitForBattleIdle())
					text = "warning: timed out waiting for battle animations to end\n";
				text += callOnGui([] { return buildMcpBattleState(); });
			}
			else if(name == "get_hero")
			{
				std::string error;
				const std::optional<int> hero = readInt(arguments["hero"], error, "hero", false);
				if(!error.empty())
				{
					response.body = jsonToolResult(request, error, true);
					return response;
				}
				text = callOnGui([hero] { return buildMcpHeroState(hero); });
			}
			else if(name == "get_town")
			{
				std::string error;
				const std::optional<int> town = readInt(arguments["town"], error, "town", true);
				if(!error.empty() || !town)
				{
					response.body = jsonToolResult(request, error.empty() ? "error: town is required" : error, true);
					return response;
				}
				text = callOnGui([town] { return buildMcpTownState(*town); });
			}
			else if(name == "move_hero")
			{
				std::string error;
				const std::optional<int> x = readInt(arguments["x"], error, "x", true);
				const std::optional<int> y = readInt(arguments["y"], error, "y", true);
				const std::optional<int> z = readInt(arguments["z"], error, "z", false);
				const std::optional<int> hero = readInt(arguments["hero"], error, "hero", false);
				if(!error.empty() || !x || !y)
				{
					response.body = jsonToolResult(request, error.empty() ? "error: x and y are required" : error, true);
					return response;
				}
				text = callOnGui([x, y, z, hero] { return mcpMoveHero(*x, *y, z, hero); });
			}
			else if(name == "attack")
			{
				std::string error;
				const std::optional<int> stack = readInt(arguments["stack"], error, "stack", true);
				if(!error.empty() || !stack)
				{
					response.body = jsonToolResult(request, error.empty() ? "error: stack is required" : error, true);
					return response;
				}
				text = runBattleAction([stack] { return mcpAttack(*stack); });
			}
			else if(name == "wait")
			{
				text = runBattleAction([] { return mcpBattleWait(); });
			}
			else if(name == "defend")
			{
				text = runBattleAction([] { return mcpBattleDefend(); });
			}
			else if(name == "answer")
			{
				std::string error;
				const std::optional<int> choice = readInt(arguments["choice"], error, "choice", true);
				const std::optional<int> question = readInt(arguments["question"], error, "question", false);
				if(!error.empty() || !choice)
				{
					response.body = jsonToolResult(request, error.empty() ? "error: choice is required" : error, true);
					return response;
				}
				text = callOnGui([choice, question] { return mcpAnswer(*choice, question); });
			}
			else if(name == "end_turn")
			{
				text = callOnGui([] { return mcpEndTurn(); });
			}
			else
			{
				response.body = jsonToolResult(request, "Unknown tool: " + name, true);
				return response;
			}

			const bool failed = text.rfind("Timed out", 0) == 0 || text.rfind("MCP server", 0) == 0 || text.rfind("error:", 0) == 0;
			response.body = jsonToolResult(request, text, failed);
			return response;
		}

		response.body = jsonError(request, -32601, "Method not found");
		return response;
	}

	void handleConnection(boost::asio::ip::tcp::socket & socket)
	{
		const std::optional<HttpRequest> request = readHttpRequest(socket);
		if(!request)
			return;

		if(request->method == "OPTIONS")
		{
			writeHttpResponse(socket, HttpResponse{204, "No Content", {}, {}});
			return;
		}

		if(request->method != "POST")
		{
			writeHttpResponse(socket, HttpResponse{405, "Method Not Allowed", "POST /mcp\n", "text/plain"});
			return;
		}

		writeHttpResponse(socket, handleJson(request->body));
	}

	void run()
	{
		setThreadName("mcp");
		while(!stopping.load())
		{
			boost::system::error_code error;
			boost::asio::ip::tcp::socket socket(io);
			acceptor->accept(socket, error);
			if(stopping.load())
				break;
			if(error)
			{
				logGlobal->warn("MCP accept failed: %s", error.message());
				break;
			}

			try
			{
				handleConnection(socket);
			}
			catch(const std::exception & exception)
			{
				logGlobal->warn("MCP request failed: %s", exception.what());
			}
		}
	}
};

McpServer::McpServer()
	: impl(std::make_unique<Impl>())
{
}

McpServer::~McpServer()
{
	stop();
}

McpServer & McpServer::instance()
{
	static McpServer server;
	return server;
}

void McpServer::start()
{
	if(impl->thread.joinable())
		return;

	impl->stopping = false;

	try
	{
		const boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address_v4::loopback(), port);
		impl->acceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(impl->io);
		impl->acceptor->open(endpoint.protocol());
		impl->acceptor->set_option(boost::asio::socket_base::reuse_address(true));
		impl->acceptor->bind(endpoint);
		impl->acceptor->listen();
	}
	catch(const std::exception & error)
	{
		logGlobal->error("MCP server failed to listen on 127.0.0.1:%d: %s", static_cast<int>(port), error.what());
		impl->acceptor.reset();
		return;
	}

	impl->thread = std::thread([this] { impl->run(); });
	logGlobal->info("MCP server listening on 127.0.0.1:%d", static_cast<int>(port));
}

void McpServer::drain()
{
	std::deque<Impl::Job> jobs;
	{
		std::lock_guard lock(impl->queueMutex);
		jobs.swap(impl->queue);
	}

	if(jobs.empty())
		return;

	if(!ENGINE)
	{
		for(const Impl::Job & job : jobs)
			completePromise(job.promise, "Game engine is not running");
		return;
	}

	std::scoped_lock interfaceLock(ENGINE->interfaceMutex);
	for(const Impl::Job & job : jobs)
	{
		if(impl->stopping.load())
		{
			completePromise(job.promise, "MCP server is stopping");
			continue;
		}

		try
		{
			completePromise(job.promise, job.work());
		}
		catch(const std::exception & error)
		{
			completePromise(job.promise, std::string("error: ") + error.what());
		}
		catch(...)
		{
			completePromise(job.promise, "error: unknown");
		}
	}
}

void McpServer::stop()
{
	{
		std::lock_guard lock(impl->queueMutex);
		impl->stopping = true;
	}
	McpBattleWaiter::instance().cancel();

	if(impl->acceptor)
	{
		boost::system::error_code error;
		impl->acceptor->close(error);
		impl->acceptor.reset();
	}

	std::deque<Impl::Job> jobs;
	{
		std::lock_guard lock(impl->queueMutex);
		jobs.swap(impl->queue);
	}
	for(const Impl::Job & job : jobs)
		completePromise(job.promise, "MCP server is stopping");

	if(impl->thread.joinable())
		impl->thread.join();
}
