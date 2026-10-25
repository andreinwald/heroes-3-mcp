import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import type { Transport } from "@modelcontextprotocol/sdk/shared/transport.js";
import type { JSONRPCMessage } from "@modelcontextprotocol/sdk/types.js";

const REQUEST_TIMEOUT_MS = 120_000;

export type McpTool = {
	name: string;
	description?: string;
	inputSchema: {
		type?: string;
		properties?: Record<string, unknown>;
		required?: string[];
		[key: string]: unknown;
	};
};

class GameHttpTransport implements Transport {
	onclose?: () => void;
	onerror?: (error: Error) => void;
	onmessage?: (message: JSONRPCMessage) => void;

	constructor(private readonly url: string) {}

	async start(): Promise<void> {}

	async send(message: JSONRPCMessage, _options?: unknown): Promise<void> {
		const controller = new AbortController();
		const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
		try {
			const response = await fetch(this.url, {
				method: "POST",
				headers: {
					"Content-Type": "application/json",
					Accept: "application/json, text/event-stream",
				},
				body: JSON.stringify(message),
				signal: controller.signal,
			});

			if (response.status === 202 || response.status === 204)
				return;

			const raw = await response.text();
			if (!response.ok)
				throw new Error(`MCP HTTP ${response.status}: ${raw || response.statusText}`);
			if (!raw)
				return;

			const parsed = JSON.parse(raw) as JSONRPCMessage;
			this.onmessage?.(parsed);
		} catch (error) {
			const wrapped = error instanceof Error ? error : new Error(String(error));
			if (wrapped.name === "AbortError")
				wrapped.message = `Timed out after ${REQUEST_TIMEOUT_MS / 1000}s waiting for ${this.url}`;
			this.onerror?.(wrapped);
			throw wrapped;
		} finally {
			clearTimeout(timer);
		}
	}

	async close(): Promise<void> {
		this.onclose?.();
	}
}

export class GameMcp {
	private constructor(
		readonly url: string,
		private readonly client: Client,
		private readonly transport: GameHttpTransport,
	) {}

	static async connect(url: string): Promise<GameMcp> {
		const client = new Client({ name: "vcmi-agent", version: "0.1.0" });
		const transport = new GameHttpTransport(url);
		try {
			await client.connect(transport);
		} catch (error) {
			const cause = error instanceof Error ? error.message : String(error);
			if (cause.includes("ECONNREFUSED") || cause.includes("fetch failed"))
				throw new Error(`Nothing is listening on ${url}. Start vcmiclient first.`);
			throw error;
		}
		return new GameMcp(url, client, transport);
	}

	async listTools(): Promise<McpTool[]> {
		const listed = await this.client.listTools();
		return listed.tools.map((tool) => ({
			name: tool.name,
			description: tool.description,
			inputSchema: tool.inputSchema as McpTool["inputSchema"],
		}));
	}

	async callTool(name: string, args: Record<string, unknown>): Promise<{ text: string; isError: boolean }> {
		const result = await this.client.callTool({ name, arguments: args });
		return {
			text: contentText(result.content),
			isError: result.isError === true,
		};
	}

	async close(): Promise<void> {
		await this.client.close();
		await this.transport.close();
	}
}

function contentText(content: unknown): string {
	if (!Array.isArray(content))
		return content == null ? "" : JSON.stringify(content);

	const parts: string[] = [];
	for (const part of content) {
		if (part && typeof part === "object" && "type" in part && part.type === "text" && "text" in part)
			parts.push(String(part.text));
		else
			parts.push(JSON.stringify(part));
	}
	return parts.join("\n");
}
