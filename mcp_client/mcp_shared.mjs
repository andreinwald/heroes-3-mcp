import http from "node:http";

// Game MCP (in-process in vcmiclient). See client/mcp/McpServer.h
export const GAME_PORT = 47321;
// Cursor connects here when using .cursor/mcp.json "url" mode.
export const CURSOR_HTTP_PORT = 47322;

export const tools = [
	{
		name: "get_state",
		description:
			"Text snapshot of the visible game: date, resources, heroes, visible towns (pos, owner, relation=enemy|ally|neutral), map objects, the top blocking dialog with numbered choices (use answer), and the current battle (active_turn=yours|enemy|busy|none; call attack/wait/defend only when active_turn=yours).",
		inputSchema: { type: "object", properties: {} },
	},
	{
		name: "move_hero",
		description:
			"Walk the selected hero toward a map tile. Uses the hero's remaining movement and stops at a monster to attack it. z defaults to the hero's floor. hero defaults to the hero selected on the adventure map, or your first hero.",
		inputSchema: {
			type: "object",
			properties: {
				x: { type: "integer", description: "Map column." },
				y: { type: "integer", description: "Map row." },
				z: { type: "integer", description: "Map floor, 0 for the surface." },
				hero: { type: "integer", description: "Hero id from get_state. Omit to move the current hero." },
			},
			required: ["x", "y"],
		},
	},
	{
		name: "attack",
		description:
			"In a battle, attack an enemy stack with the active unit. Shoots when the unit can shoot. Otherwise walks up and strikes in melee. Returns after the next player decision, with a get_state snapshot.",
		inputSchema: {
			type: "object",
			properties: {
				stack: { type: "integer", description: "Enemy stack id from get_state." },
			},
			required: ["stack"],
		},
	},
	{
		name: "wait",
		description:
			"In a battle, wait with the active stack so another unit can act first this round. Returns after the next player decision, with a get_state snapshot.",
		inputSchema: { type: "object", properties: {} },
	},
	{
		name: "defend",
		description:
			"In a battle, defend with the active stack, ending its turn for this round. Returns after the next player decision, with a get_state snapshot.",
		inputSchema: { type: "object", properties: {} },
	},
	{
		name: "end_turn",
		description:
			"End your adventure turn and advance to the next day. Fails during battle, while a dialog is open, or while a hero is moving.",
		inputSchema: { type: "object", properties: {} },
	},
	{
		name: "answer",
		description:
			"Pick an option on the top blocking dialog from get_state: treasure choose 1/2, yes_no 1/0, info ok=1, battle_result ok=0 repeat=1, level_up skill index, commander_level_up skill id, exchange leave=0, tutorial ok=1, recruitment leave=0. Optional question id clears a stuck server activity.",
		inputSchema: {
			type: "object",
			properties: {
				choice: { type: "integer", description: "Option number from get_state." },
				question: { type: "integer", description: "Server question id from dialog line or error log; use with choice when the UI is stuck." },
			},
			required: ["choice"],
		},
	},
];

function postToGame(message) {
	const payload = JSON.stringify(message);
	return new Promise((resolve, reject) => {
		const request = http.request(
			{
				hostname: "127.0.0.1",
				port: GAME_PORT,
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
						reject(new Error(`Game response was not JSON (${response.statusCode}): ${raw}`));
					}
				});
			},
		);

		request.setTimeout(120000, () => {
			request.destroy(new Error("Timed out waiting for the game. Leave the game window open."));
		});
		request.on("error", (error) => {
			if (error.code === "ECONNREFUSED")
				reject(
					new Error(
						`Nothing is listening on 127.0.0.1:${GAME_PORT}. Start the game (vcmilauncher / vcmiclient) first.`,
					),
				);
			else reject(error);
		});
		request.end(payload);
	});
}

function rpcOk(id, result) {
	return { status: 200, json: { jsonrpc: "2.0", id, result } };
}

function rpcErr(id, code, message) {
	return { status: 200, json: { jsonrpc: "2.0", id, error: { code, message } } };
}

function accepted() {
	return { status: 202, json: null };
}

async function callGameTool(name, arguments_) {
	const called = await postToGame({
		jsonrpc: "2.0",
		id: 1,
		method: "tools/call",
		params: { name, arguments: arguments_ ?? {} },
	});
	if (!called)
		throw new Error(`Empty response from ${name}`);
	if (called.error)
		throw new Error(called.error.message ?? `${name} failed`);
	if (!called.result)
		throw new Error(`Unexpected result: ${JSON.stringify(called)}`);
	return called.result;
}

/** @returns {Promise<{ status: number, json: object | null }>} */
export async function dispatchMessage(message) {
	const id = message.id;
	const notification = id === undefined || id === null;
	const method = message.method;

	if (method === "initialize") {
		const requested = message.params?.protocolVersion;
		return rpcOk(id, {
			protocolVersion: typeof requested === "string" && requested.length > 0 ? requested : "2025-03-26",
			capabilities: { tools: { listChanged: false } },
			serverInfo: { name: "vcmi", version: "0.1.0" },
		});
	}

	if (method === "notifications/initialized" || method === "initialized")
		return accepted();

	if (notification)
		return accepted();

	if (method === "ping")
		return rpcOk(id, {});

	if (method === "tools/list")
		return rpcOk(id, { tools });

	if (method === "resources/list")
		return rpcOk(id, { resources: [] });

	if (method === "prompts/list")
		return rpcOk(id, { prompts: [] });

	if (method === "tools/call") {
		const name = message.params?.name;
		if (typeof name !== "string")
			return rpcErr(id, -32602, "tools/call requires params.name");
		try {
			return rpcOk(id, await callGameTool(name, message.params.arguments));
		} catch (error) {
			return rpcOk(id, {
				content: [{ type: "text", text: error.message }],
				isError: true,
			});
		}
	}

	return rpcErr(id, -32601, `Method not found: ${method}`);
}
