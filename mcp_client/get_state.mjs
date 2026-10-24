import http from "node:http";

// Same port as McpServer::port in client/mcp/McpServer.h
const port = 47321;

function post(message) {
	const payload = JSON.stringify(message);
	return new Promise((resolve, reject) => {
		const request = http.request(
			{
				hostname: "127.0.0.1",
				port,
				path: "/mcp",
				method: "POST",
				headers: {
					"Content-Type": "application/json",
					Accept: "application/json, text/event-stream",
					"Content-Length": Buffer.byteLength(payload),
				},
			},
			(response) => {
				const chunks = [];
				response.on("data", (chunk) => chunks.push(chunk));
				response.on("end", () => {
					const raw = Buffer.concat(chunks).toString("utf8");
					if (response.statusCode === 202 || raw.length === 0) {
						resolve(null);
						return;
					}
					try {
						resolve(JSON.parse(raw));
					} catch (error) {
						reject(new Error(`Response was not JSON (${response.statusCode}): ${raw}`));
					}
				});
			},
		);

		request.setTimeout(8000, () => {
			request.destroy(new Error("Timed out. Leave the game window open and try again."));
		});
		request.on("error", (error) => {
			if (error.code === "ECONNREFUSED")
				reject(new Error(`Nothing is listening on 127.0.0.1:${port}. Start the game first.`));
			else
				reject(error);
		});
		request.end(payload);
	});
}

const initialized = await post({
	jsonrpc: "2.0",
	id: 1,
	method: "initialize",
	params: {
		protocolVersion: "2024-11-05",
		capabilities: {},
		clientInfo: { name: "vcmi-mcp-client", version: "0.1.0" },
	},
});

if (initialized?.error)
	throw new Error(initialized.error.message ?? "initialize failed");

await post({
	jsonrpc: "2.0",
	method: "notifications/initialized",
});

const called = await post({
	jsonrpc: "2.0",
	id: 2,
	method: "tools/call",
	params: { name: "get_state", arguments: {} },
});

if (!called)
	throw new Error("Empty response from get_state");
if (called.error)
	throw new Error(called.error.message ?? "get_state failed");

const text = called.result?.content?.[0]?.text;
if (called.result?.isError)
	throw new Error(text || "get_state failed");
if (typeof text !== "string")
	throw new Error(`Unexpected result: ${JSON.stringify(called)}`);

process.stdout.write(text.endsWith("\n") ? text : `${text}\n`);
