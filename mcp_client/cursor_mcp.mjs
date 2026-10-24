import { dispatchMessage } from "./mcp_shared.mjs";

function send(message) {
	const json = JSON.stringify(message);
	process.stdout.write(`Content-Length: ${Buffer.byteLength(json)}\r\n\r\n${json}`);
}

async function handle(message) {
	const id = message.id;
	const { status, json } = await dispatchMessage(message);
	if (status === 202)
		return;
	if (json)
		send(json);
	else if (id !== undefined && id !== null)
		send({ jsonrpc: "2.0", id, error: { code: -32603, message: "empty response" } });
}

let pending = Promise.resolve();
let buffer = Buffer.alloc(0);

function onChunk(chunk) {
	buffer = Buffer.concat([buffer, chunk]);
	while (true) {
		const headerEnd = buffer.indexOf("\r\n\r\n");
		if (headerEnd < 0)
			return;
		const header = buffer.subarray(0, headerEnd).toString("utf8");
		const match = header.match(/content-length:\s*(\d+)/i);
		if (!match) {
			buffer = buffer.subarray(headerEnd + 4);
			continue;
		}
		const length = Number(match[1]);
		const bodyStart = headerEnd + 4;
		if (buffer.length < bodyStart + length)
			return;
		const body = buffer.subarray(bodyStart, bodyStart + length).toString("utf8");
		buffer = buffer.subarray(bodyStart + length);
		let message;
		try {
			message = JSON.parse(body);
		} catch (error) {
			console.error(`Ignored invalid MCP message: ${error.message}`);
			continue;
		}
		pending = pending.then(() => handle(message)).catch((error) => {
			console.error(error);
		});
	}
}

process.stdin.on("data", onChunk);
process.stdin.on("error", (error) => {
	console.error(error);
	process.exit(1);
});
