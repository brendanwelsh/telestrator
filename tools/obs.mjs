// Minimal obs-websocket v5 client for Node (v21+, uses global WebSocket).
// No external deps. Used by the telestrator verification harness.
import crypto from "node:crypto";

const OP = { HELLO: 0, IDENTIFY: 1, IDENTIFIED: 2, REQUEST: 6, REQUEST_RESPONSE: 7 };
const sha256b64 = (s) => crypto.createHash("sha256").update(s).digest("base64");

export class Obs {
	constructor({ host = "127.0.0.1", port = 4455, password = "" } = {}) {
		this.url = `ws://${host}:${port}`;
		this.password = password;
		this.ws = null;
		this.pending = new Map();
		this.reqId = 0;
	}

	connect() {
		return new Promise((resolve, reject) => {
			const ws = new WebSocket(this.url);
			this.ws = ws;
			const to = setTimeout(() => reject(new Error("connect timeout")), 8000);
			ws.addEventListener("message", (ev) => {
				const msg = JSON.parse(ev.data);
				if (msg.op === OP.HELLO) {
					const d = msg.d;
					const ident = { rpcVersion: 1 };
					if (d.authentication) {
						const secret = sha256b64(this.password + d.authentication.salt);
						ident.authentication = sha256b64(secret + d.authentication.challenge);
					}
					ws.send(JSON.stringify({ op: OP.IDENTIFY, d: ident }));
				} else if (msg.op === OP.IDENTIFIED) {
					clearTimeout(to);
					resolve();
				} else if (msg.op === OP.REQUEST_RESPONSE) {
					const p = this.pending.get(msg.d.requestId);
					if (!p) return;
					this.pending.delete(msg.d.requestId);
					if (msg.d.requestStatus && msg.d.requestStatus.result) p.resolve(msg.d.responseData || {});
					else p.reject(new Error((msg.d.requestStatus && msg.d.requestStatus.comment) || "request failed"));
				}
			});
			ws.addEventListener("error", () => {
				clearTimeout(to);
				reject(new Error("ws error (is OBS running with obs-websocket enabled?)"));
			});
			ws.addEventListener("close", () => {
				for (const p of this.pending.values()) p.reject(new Error("connection closed"));
			});
		});
	}

	request(type, data = {}) {
		return new Promise((resolve, reject) => {
			const id = String(++this.reqId);
			this.pending.set(id, { resolve, reject });
			this.ws.send(JSON.stringify({ op: OP.REQUEST, d: { requestType: type, requestId: id, requestData: data } }));
			setTimeout(() => {
				if (this.pending.has(id)) {
					this.pending.delete(id);
					reject(new Error("request timeout: " + type));
				}
			}, 8000);
		});
	}

	// Resolves true if the request succeeds, false if it errors (for existence checks).
	async ok(type, data = {}) {
		try {
			await this.request(type, data);
			return true;
		} catch {
			return false;
		}
	}

	close() {
		if (this.ws) this.ws.close();
	}
}
