import { mkdir, appendFile } from "node:fs/promises";
import path from "node:path";

export type LogEvent = {
	ts: string;
	type: string;
	[key: string]: unknown;
};

export class RunLog {
	readonly file: string;

	constructor(logsDir: string, startedAt: Date) {
		this.file = path.join(logsDir, `run-${stamp(startedAt)}.jsonl`);
	}

	async open(): Promise<void> {
		await mkdir(path.dirname(this.file), { recursive: true });
		await this.write({ type: "run_open", file: this.file });
	}

	async write(event: Omit<LogEvent, "ts">): Promise<void> {
		const line = JSON.stringify({ ts: new Date().toISOString(), ...event }) + "\n";
		await appendFile(this.file, line, "utf8");
	}
}

function stamp(date: Date): string {
	const pad = (value: number) => String(value).padStart(2, "0");
	return (
		`${date.getFullYear()}${pad(date.getMonth() + 1)}${pad(date.getDate())}-` +
		`${pad(date.getHours())}${pad(date.getMinutes())}${pad(date.getSeconds())}`
	);
}
