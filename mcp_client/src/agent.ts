import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { styleText } from "node:util";

import { config as loadEnv } from "dotenv";
import { Mistral } from "@mistralai/mistralai";
import { MistralError } from "@mistralai/mistralai/models/errors/mistralerror.js";
import { ReasoningEffort } from "@mistralai/mistralai/models/components/reasoningeffort.js";
import type { Tool } from "@mistralai/mistralai/models/components/tool.js";

import { GameMcp, type McpTool } from "./gameMcp.ts";
import { RunLog } from "./logger.ts";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..");

loadEnv({ path: path.join(root, ".env"), quiet: true });

type ToolCall = {
	id?: string | null;
	function?: { name?: string | null; arguments?: string | Record<string, unknown> | null };
};

type ChatMessage =
	| { role: "system"; content: string }
	| { role: "user"; content: string }
	| { role: "assistant"; content?: string | null; toolCalls?: ToolCall[] }
	| { role: "tool"; toolCallId: string; name: string; content: string };

const KEEP_HISTORY = 100;
const IDLE_LIMIT = 3;
const MISTRAL_MODEL = "mistral-large-4";
const REASONING_EFFORT = ReasoningEffort.None;
const MCP_URL = "http://127.0.0.1:47321/mcp";
const MAX_STEPS = 1000;

function env(name: string, fallback = ""): string {
	return (process.env[name] ?? fallback).trim();
}

/** Extra args after `npm start -- …` or `tsx src/agent.ts …` — one suggested next step, not a campaign goal. */
function cliNextStep(): string | null {
	const args = process.argv.slice(2).map((arg) => arg.trim()).filter(Boolean);
	if (args.length === 0)
		return null;
	return args.join(" ");
}

function initialUserMessage(nextStep: string | null): string {
	if (nextStep)
		return (
			`The game is running. Observe the current situation, then take this as your next step: ${nextStep}. ` +
			"After that, keep playing on your own unless you are told otherwise."
		);
	return "The game is running. Observe the current situation, then start playing.";
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

const DESCRIBED_ACTION_TOOLS = new Set(["move_hero", "attack", "answer"]);

function actionDescription(args: Record<string, unknown>): string | undefined {
	const raw = args.description;
	if (typeof raw !== "string")
		return undefined;
	const text = raw.replace(/\s+/g, " ").trim();
	return text || undefined;
}

function toolErrorDetail(text: string): string {
	const line = text.split("\n").map((part) => part.trim()).find(Boolean) ?? text;
	const flat = line.replace(/\s+/g, " ").trim();
	if (flat.toLowerCase().startsWith("error:"))
		return flat.slice(6).trim();
	return flat;
}

function actionNameLabel(name: string, tty: boolean): string {
	const label = `[${name}]`;
	return tty ? styleText("yellow", label) : label;
}

function printActionStart(name: string, description?: string): void {
	const suffix = description ? ` ${description}` : "";
	console.log(`${actionNameLabel(name, process.stdout.isTTY)}${suffix}`);
}

function printActionError(name: string, text: string): void {
	const detail = toolErrorDetail(text);
	if (process.stderr.isTTY)
		console.error(`${actionNameLabel(name, true)} — ${styleText("red", detail)}`);
	else
		console.error(`[${name}] — ${detail}`);
}

function modelTextForConsole(content: string): string {
	return content.replace(/\s+/g, " ").trim();
}

function printModel(content: string): void {
	const line = modelTextForConsole(content);
	if (!line)
		return;
	console.log(`[model] ${line}`);
}

function trimHistory(messages: ChatMessage[]): ChatMessage[] {
	if (messages.length <= KEEP_HISTORY)
		return messages;

	const head: ChatMessage[] = [];
	let restStart = 0;
	if (messages[0]?.role === "system") {
		head.push(messages[0]);
		restStart = 1;
	}
	// Keep the opening user turn; Mistral rejects `tool`/`assistant` immediately after `system`.
	if (messages[restStart]?.role === "user") {
		head.push(messages[restStart]);
		restStart += 1;
	}

	const budget = KEEP_HISTORY - head.length;
	if (budget <= 0)
		return head;

	let tail = messages.slice(restStart).slice(-budget);
	// Head already includes the opening user turn; drop orphaned tool results at the tail cut.
	while (tail.length > 0 && tail[0].role === "tool")
		tail = tail.slice(1);

	return [...head, ...tail];
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

	const game = await GameMcp.connect(MCP_URL);
	const tools = await game.listTools();
	await log.write({ type: "mcp_connected", url: MCP_URL, tools: tools.map((tool) => tool.name) });

	const mistral = new Mistral({ apiKey });
	const mistralTools = mcpToolsToMistral(tools);
	const nextStep = cliNextStep();
	if (nextStep)
		console.log(`next step: ${nextStep}`);

	let messages: ChatMessage[] = [
		{ role: "system", content: `${system}\n\n${mcpToolsCatalog(tools)}` },
		{ role: "user", content: initialUserMessage(nextStep) },
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

	await log.write({
		type: "run_start",
		model: MISTRAL_MODEL,
		reasoningEffort: REASONING_EFFORT,
		mcpUrl: MCP_URL,
		systemFile: "system.md",
		nextStep,
	});

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
				reasoningEffort: REASONING_EFFORT,
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
		const usage = response.usage;
		await log.write({
			type: "llm_response",
			step,
			ok: true,
			finishReason: choice.finishReason,
			content,
			toolCalls,
			promptTokens: usage?.promptTokens ?? null,
			completionTokens: usage?.completionTokens ?? null,
			totalTokens: usage?.totalTokens ?? null,
			response,
		});
		if (content)
			printModel(content);

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
			const description = DESCRIBED_ACTION_TOOLS.has(name) ? actionDescription(args) : undefined;
			await log.write({ type: "tool_call", step, id, name, description: description ?? null });
			printActionStart(name, description);

			const started = performance.now();
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
			const durationMs = performance.now() - started;

			await log.write({ type: "tool_result", step, id, name, isError, durationMs, text });
			if (isError)
				printActionError(name, text);
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
