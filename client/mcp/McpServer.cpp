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
#include "GameStateSnapshot.h"

#include "../lib/CThreadHelper.h"
#include "../lib/gameState/CGameState.h"
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
			JsonNode tool;
			tool["name"].String() = "get_state";
			tool["description"].String() = "Text snapshot of the visible game: date, resources, your heroes and towns, visible map objects, and the current battle.";
			tool["inputSchema"]["type"].String() = "object";
			tool["inputSchema"]["properties"].Struct();

			JsonNode result;
			result["tools"].Vector().push_back(std::move(tool));
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
			if(name != "get_state")
			{
				response.body = jsonToolResult(request, "Unknown tool: " + name, true);
				return response;
			}

			const std::string text = callOnGui([] { return buildMcpGameState(); });
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
	std::shared_lock stateLock(CGameState::mutex);
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
