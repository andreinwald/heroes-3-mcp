import http from "node:http";

// Same port as McpServer::port in client/mcp/McpServer.h
// node get_state.mjs [x y z]
// With no coordinates, the first hero on the map walks 3 tiles east.
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

async function callTool(id, name, arguments_) {
	const called = await post({
		jsonrpc: "2.0",
		id,
		method: "tools/call",
		params: { name, arguments: arguments_ },
	});

	if (!called)
		throw new Error(`Empty response from ${name}`);
	if (called.error)
		throw new Error(called.error.message ?? `${name} failed`);

	const text = called.result?.content?.[0]?.text;
	if (typeof text !== "string")
		throw new Error(`Unexpected result: ${JSON.stringify(called)}`);
	return { text, isError: called.result?.isError === true };
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

const state = await callTool(2, "get_state", {});
process.stdout.write(state.text.endsWith("\n") ? state.text : `${state.text}\n`);
if (state.isError)
	process.exit(1);

const hero = state.text.match(/^hero id=(\d+) .* pos=(-?\d+),(-?\d+),(-?\d+) .* garrison=no/m);
const map = state.text.match(/^map width=(\d+) height=(\d+) levels=(\d+)/m);
if (!hero || !map) {
	process.stdout.write("error: no wandering hero to move\n");
	process.exit(1);
}

const args = process.argv.slice(2);
let x;
let y;
let z;
if (args.length >= 2) {
	x = Number(args[0]);
	y = Number(args[1]);
	z = args.length >= 3 ? Number(args[2]) : Number(hero[4]);
	if (![x, y, z].every(Number.isInteger))
		throw new Error("Usage: node get_state.mjs [x y z]");
} else {
	const width = Number(map[1]);
	x = Number(hero[2]) + 3;
	y = Number(hero[3]);
	z = Number(hero[4]);
	if (x >= width)
		x = Number(hero[2]) - 3;
}

const move = await callTool(3, "move_hero", {
	hero: Number(hero[1]),
	x,
	y,
	z,
});
process.stdout.write("------------------\n");
process.stdout.write(move.text.endsWith("\n") ? move.text : `${move.text}\n`);
if (move.isError)
	process.exit(1);
