import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { styleText } from "node:util";

import { config as loadEnv } from "dotenv";
import { Mistral } from "@mistralai/mistralai";
import { MistralError } from "@mistralai/mistralai/models/errors/mistralerror.js";
import type { Tool } from "@mistralai/mistralai/models/components/tool.js";

import { GameMcp, type McpTool } from "./gameMcp.ts";
import { RunLog } from "./logger.ts";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..");

loadEnv({ path: path.join(root, ".env") });

type ToolCall = {
	id?: string | null;
	function?: { name?: string | null; arguments?: string | Record<string, unknown> | null };
};

type ChatMessage =
	| { role: "system"; content: string }
	| { role: "user"; content: string }
	| { role: "assistant"; content?: string | null; toolCalls?: ToolCall[] }
	| { role: "tool"; toolCallId: string; name: string; content: string };

const MAX_MESSAGES = 80;
const IDLE_LIMIT = 3;
const MISTRAL_MODEL = "mistral-large-latest";
const MCP_URL = "http://127.0.0.1:47321/mcp";
/** 0 = no limit */
const MAX_STEPS = 0;

function env(name: string, fallback = ""): string {
	return (process.env[name] ?? fallback).trim();
}

function printError(message: string): void {
	const text = process.stderr.isTTY ? styleText("red", message) : message;
	console.error(text);
}

function parseJsonBody(raw: string): unknown {
	try {
		return JSON.parse(raw) as unknown;
	} catch {
		return raw;
	}
}

function llmErrorRecord(error: unknown): Record<string, unknown> {
	if (error instanceof MistralError) {
		return {
			ok: false,
			message: error.message,
			statusCode: error.statusCode,
			body: parseJsonBody(error.body),
		};
	}
	if (error instanceof Error)
		return { ok: false, message: error.message, name: error.name };
	return { ok: false, message: String(error) };
}

function mcpToolsToMistral(tools: McpTool[]): Tool[] {
	return tools.map((tool) => ({
		type: "function",
		function: {
			name: tool.name,
			description: tool.description,
			parameters: {
				type: "object",
				properties: tool.inputSchema.properties ?? {},
				required: tool.inputSchema.required ?? [],
			},
		},
	}));
}

function mcpToolsCatalog(tools: McpTool[]): string {
	const lines = tools.map((tool) => {
		const schema = {
			type: tool.inputSchema.type ?? "object",
			properties: tool.inputSchema.properties ?? {},
			required: tool.inputSchema.required ?? [],
		};
		const description = tool.description?.trim() || "(no description)";
		return `- \`${tool.name}\`\n  ${description}\n  parameters: ${JSON.stringify(schema)}`;
	});
	return ["## Current MCP methods (from the live server)", ...lines].join("\n");
}

function parseArgs(raw: string | Record<string, unknown> | null | undefined): Record<string, unknown> {
	if (raw == null || raw === "")
		return {};
	if (typeof raw === "object")
		return raw;
	try {
		const parsed = JSON.parse(raw) as unknown;
		if (parsed && typeof parsed === "object" && !Array.isArray(parsed))
			return parsed as Record<string, unknown>;
		return {};
	} catch {
		return {};
	}
}

function preview(text: string, limit = 240): string {
	const flat = text.replace(/\s+/g, " ").trim();
	if (flat.length <= limit)
		return flat;
	return flat.slice(0, limit - 1) + "…";
}

function trimHistory(messages: ChatMessage[]): ChatMessage[] {
	if (messages.length <= MAX_MESSAGES)
		return messages;
	const system = messages[0];
	return system ? [system, ...messages.slice(-(MAX_MESSAGES - 1))] : messages.slice(-MAX_MESSAGES);
}

async function main(): Promise<void> {
	const apiKey = env("MISTRAL_API_KEY");
	if (!apiKey) {
		throw new Error(
			"Cannot call Mistral: MISTRAL_API_KEY is missing or empty. " +
				"In the mcp_client folder, copy .env.example to .env and put your API key on the MISTRAL_API_KEY= line.",
		);
	}

	const system = (await readFile(path.join(root, "system.md"), "utf8")).trim();
	const log = new RunLog(path.join(root, "logs"), new Date());
	await log.open();
	console.log(`log ${log.file}`);

	const game = await GameMcp.connect(MCP_URL);
	const tools = await game.listTools();
	await log.write({ type: "mcp_connected", url: MCP_URL, tools: tools.map((tool) => tool.name) });
	console.log(`mcp ${MCP_URL} tools=${tools.map((tool) => tool.name).join(",")}`);

	const mistral = new Mistral({ apiKey });
	const mistralTools = mcpToolsToMistral(tools);

	let messages: ChatMessage[] = [
		{ role: "system", content: `${system}\n\n${mcpToolsCatalog(tools)}` },
		{ role: "user", content: "The game is running. Observe the current situation, then start playing." },
	];

	let step = 0;
	let idle = 0;
	let stopReason: string | null = null;

	process.on("SIGINT", () => {
		stopReason = "SIGINT";
	});
	process.on("SIGTERM", () => {
		stopReason = "SIGTERM";
	});

	await log.write({ type: "run_start", model: MISTRAL_MODEL, mcpUrl: MCP_URL, systemFile: "system.md" });

	while (!stopReason) {
		if (MAX_STEPS > 0 && step >= MAX_STEPS) {
			stopReason = "max_steps";
			break;
		}
		step += 1;
		messages = trimHistory(messages);

		await log.write({ type: "llm_request", step, model: MISTRAL_MODEL, messages });

		let response: Awaited<ReturnType<typeof mistral.chat.complete>>;
		try {
			response = await mistral.chat.complete({
				model: MISTRAL_MODEL,
				messages: messages as Parameters<typeof mistral.chat.complete>[0]["messages"],
				tools: mistralTools,
				toolChoice: "auto",
			});
		} catch (error) {
			const record = llmErrorRecord(error);
			await log.write({ type: "llm_response", step, ...record });
			const detail =
				record.body && typeof record.body === "object"
					? JSON.stringify(record.body)
					: String(record.message ?? error);
			printError(detail);
			stopReason = "llm_error";
			break;
		}

		const choice = response.choices?.[0];
		const message = choice?.message;
		if (!message) {
			await log.write({
				type: "llm_response",
				step,
				ok: false,
				message: "Mistral returned no message",
				response,
			});
			printError("Mistral returned no message");
			stopReason = "llm_error";
			break;
		}

		const toolCalls = (message.toolCalls ?? []) as ToolCall[];
		const content = typeof message.content === "string" ? message.content : "";
		await log.write({
			type: "llm_response",
			step,
			ok: true,
			finishReason: choice.finishReason,
			content,
			toolCalls,
			response,
		});

		if (content)
			console.log(`\n#${step} ${preview(content, 400)}`);

		if (toolCalls.length === 0) {
			idle += 1;
			if (content)
				messages.push({ role: "assistant", content });
			if (idle >= IDLE_LIMIT) {
				stopReason = "idle";
				break;
			}
			messages.push({
				role: "user",
				content: "Continue playing. Observe the current situation if you need it, then act.",
			});
			continue;
		}

		idle = 0;
		messages.push({
			role: "assistant",
			content: content || null,
			toolCalls,
		});

		for (const call of toolCalls) {
			const name = call.function?.name ?? "";
			const args = parseArgs(call.function?.arguments);
			const id = call.id ?? name;
			console.log(`→ ${name} ${JSON.stringify(args)}`);
			await log.write({ type: "tool_call", step, id, name, arguments: args });

			let text: string;
			let isError = false;
			try {
				if (!name)
					throw new Error("tool call missing function name");
				const result = await game.callTool(name, args);
				text = result.text;
				isError = result.isError;
			} catch (error) {
				text = error instanceof Error ? error.message : String(error);
				isError = true;
			}

			await log.write({ type: "tool_result", step, id, name, isError, text });
			const resultLine = `← ${isError ? "error " : ""}${preview(text)}`;
			if (isError)
				printError(resultLine);
			else
				console.log(resultLine);
			messages.push({ role: "tool", toolCallId: id, name, content: text });
		}
	}

	await log.write({ type: "run_end", reason: stopReason ?? "done", steps: step });
	await game.close();
	console.log(`stop: ${stopReason ?? "done"}`);
}

main().catch(async (error) => {
	printError(error instanceof Error ? error.message : String(error));
	process.exitCode = 1;
});
