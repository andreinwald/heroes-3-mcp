import http from "node:http";
import { CURSOR_HTTP_PORT, GAME_PORT, dispatchMessage } from "./mcp_shared.mjs";

const cors = {
	"Access-Control-Allow-Origin": "*",
	"Access-Control-Allow-Headers":
		"Content-Type, Accept, Mcp-Session-Id, MCP-Protocol-Version, mcp-protocol-version",
	"Access-Control-Allow-Methods": "POST, GET, OPTIONS",
};

function readBody(req) {
	return new Promise((resolve, reject) => {
		const chunks = [];
		req.on("data", (chunk) => chunks.push(chunk));
		req.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
		req.on("error", reject);
	});
}

const server = http.createServer(async (req, res) => {
	const path = req.url?.split("?")[0] ?? "/";

	if (req.method === "OPTIONS") {
		res.writeHead(204, cors);
		res.end();
		return;
	}

	if (path !== "/mcp") {
		res.writeHead(404, { ...cors, "Content-Type": "text/plain" });
		res.end("Use POST http://127.0.0.1:" + CURSOR_HTTP_PORT + "/mcp\n");
		return;
	}

	if (req.method === "GET") {
		res.writeHead(405, { ...cors, "Content-Type": "text/plain" });
		res.end("GET not supported; use POST with JSON-RPC\n");
		return;
	}

	if (req.method !== "POST") {
		res.writeHead(405, { ...cors, "Content-Type": "text/plain" });
		res.end("POST /mcp\n");
		return;
	}

	let message;
	try {
		const raw = await readBody(req);
		if (!raw.trim()) {
			res.writeHead(400, { ...cors, "Content-Type": "application/json" });
			res.end(JSON.stringify({ error: "empty body" }));
			return;
		}
		message = JSON.parse(raw);
	} catch (error) {
		res.writeHead(400, { ...cors, "Content-Type": "application/json" });
		res.end(JSON.stringify({ error: error.message }));
		return;
	}

	try {
		const { status, json } = await dispatchMessage(message);
		if (status === 202) {
			res.writeHead(202, cors);
			res.end();
			return;
		}
		res.writeHead(status, { ...cors, "Content-Type": "application/json" });
		res.end(JSON.stringify(json));
	} catch (error) {
		console.error(error);
		res.writeHead(500, { ...cors, "Content-Type": "application/json" });
		res.end(JSON.stringify({ error: error.message }));
	}
});

server.listen(CURSOR_HTTP_PORT, "127.0.0.1", () => {
	console.log(`VCMI MCP bridge for Cursor: http://127.0.0.1:${CURSOR_HTTP_PORT}/mcp`);
	console.log(`Expecting the game MCP on http://127.0.0.1:${GAME_PORT}/mcp — start vcmilauncher first.`);
	console.log("Leave this process running while you use Cursor.");
});
