// drive.mjs — drive an all-tools telestrator showcase over obs-websocket.
// Tool/color/size changes fire hotkeys; strokes go through the "sim_stroke"
// vendor request, which feeds the engine's canonical dock input path — no
// window focus, no projector, no cursor games. Run while recording OBS.
//   OBS_WS_PASSWORD=... node tools/drive.mjs
import { Obs } from "./obs.mjs";

const obs = new Obs({ password: process.env.OBS_WS_PASSWORD || "" });
const hk = (name) => obs.request("TriggerHotkeyByName", { hotkeyName: name });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const W = 1920, H = 1080; // canvas coords (normalized inputs scale up)

const stroke = (pts, ms = 700) =>
	obs.request("CallVendorRequest", {
		vendorName: "telestrator",
		requestType: "sim_stroke",
		requestData: { points: pts.map(([x, y]) => ({ x: x * W, y: y * H })), duration_ms: ms },
	});

// one demo beat: switch tool/colour/size, draw a stroke, pause so it reads
async function beat(keys, pts, ms = 700, pause = 650) {
	for (const k of keys) { await hk(k); await sleep(60); }
	if (pts) { await stroke(pts, ms); await sleep(ms + pause); }
}

// gentle freehand arc between two points (for pen/highlighter beats)
function arc(x1, y1, x2, y2, bow = 0.08, n = 14) {
	const pts = [];
	for (let i = 0; i <= n; i++) {
		const t = i / n;
		const px = x1 + (x2 - x1) * t;
		const py = y1 + (y2 - y1) * t + Math.sin(t * Math.PI) * bow;
		pts.push([px, py]);
	}
	return pts;
}

// Three short "plays", each cleared before the next — like a real analysis
// segment, not a canvas that keeps piling up.
async function main() {
	await obs.connect();
	await hk("telestrator.armon");
	await sleep(500);

	// PLAY 1 — mark the player, show the path, show the vision cone
	const loop = [];
	for (let i = 0; i <= 22; i++) {
		const a = (i / 22) * 2 * Math.PI;
		loop.push([0.23 + 0.075 * Math.cos(a), 0.62 + 0.13 * Math.sin(a)]);
	}
	await beat(["telestrator.tool.pen", "telestrator.color.red", "telestrator.size.med"], loop, 900);
	await beat(["telestrator.tool.arrow", "telestrator.color.yellow", "telestrator.size.thick"],
		   [[0.28, 0.55], [0.52, 0.30]], 600);
	await beat(["telestrator.tool.cone", "telestrator.color.orange", "telestrator.size.med"],
		   [[0.30, 0.62], [0.52, 0.50]], 600);
	await sleep(1100);
	await hk("telestrator.clear");
	await sleep(500);

	// PLAY 2 — zones (modern dashes) + matchup + highlight
	await beat(["telestrator.dash", "telestrator.tool.rect", "telestrator.color.green"],
		   [[0.58, 0.16], [0.90, 0.44]], 650);
	await beat(["telestrator.tool.ellipse", "telestrator.color.white"],
		   [[0.16, 0.20], [0.40, 0.44]], 650);
	await hk("telestrator.dash");
	await sleep(100);
	await beat(["telestrator.tool.dblarrow", "telestrator.color.white", "telestrator.size.med"],
		   [[0.40, 0.32], [0.58, 0.32]], 550);
	await beat(["telestrator.highlight", "telestrator.tool.pen", "telestrator.color.yellow",
		    "telestrator.size.thick"],
		   arc(0.30, 0.70, 0.72, 0.70, 0.015), 700);
	await hk("telestrator.highlight");
	await sleep(1100);
	await hk("telestrator.clear");
	await sleep(500);

	// PLAY 3 — guide lines, the path-following curved arrow (bows BOTH ways),
	// and a fading laser note
	await beat(["telestrator.tool.firstdown", "telestrator.color.yellow", "telestrator.size.med"],
		   [[0.5, 0.845], [0.5, 0.845]], 250);
	await beat(["telestrator.tool.vertical"], [[0.115, 0.5], [0.115, 0.5]], 250);
	// curved arrow drawn along an UPWARD sweep...
	await beat(["telestrator.tool.curvedarrow", "telestrator.color.cyan"],
		   arc(0.30, 0.60, 0.62, 0.40, -0.14, 10), 650);
	// ...and another along a DOWNWARD sweep (the curve follows your drag)
	await beat([], arc(0.55, 0.65, 0.87, 0.55, 0.14, 10), 650);
	await beat(["telestrator.laser", "telestrator.color.red"], arc(0.55, 0.28, 0.80, 0.34, 0.05), 800, 1400);
	await hk("telestrator.laser");
	await sleep(700);
	await hk("telestrator.clear");
	await sleep(700);
	await hk("telestrator.armoff");
	obs.close();
}
main().catch((e) => { console.error("drive ERROR:", e.message); obs.close(); process.exit(1); });
