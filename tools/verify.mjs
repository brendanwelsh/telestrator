// Telestrator verification harness — drives OBS over obs-websocket so the C++
// plugin can be tested without a human watching the screen.
//
// Usage (password via env so no secret is committed):
//   OBS_WS_PASSWORD=... node tools/verify.mjs <command> [args]
//
// Commands:
//   setup                 Create a "Telestrator Test" scene: dark backdrop +
//                         the Telestrator source on top; switch program to it.
//   arm | disarm          Trigger telestrator.armon / telestrator.armoff.
//   projector             Open a windowed Program projector (the draw surface).
//   shot <file.png>       Screenshot the current program scene to <file>.
//   req <Type> [json]     Raw request, e.g. req GetVersion
//   probe                 Print OBS version + whether the telestrator source
//                         kind is registered (plugin loaded?).
//
// Exit code is non-zero on failure so a build/verify loop can branch on it.
import { Obs } from "./obs.mjs";

const TEST_SCENE = "Telestrator Test";
const BACKDROP = "Telestrator Test Backdrop";
const TEL_KIND = "telestrator";
const TEL_NAME = "Telestrator";

const [, , cmd, ...args] = process.argv;

const obs = new Obs({
	host: process.env.OBS_WS_HOST || "127.0.0.1",
	port: Number(process.env.OBS_WS_PORT) || 4455,
	password: process.env.OBS_WS_PASSWORD || "",
});

async function telName() {
	const r = await obs.request("GetInputList", { inputKind: TEL_KIND }).catch(() => ({ inputs: [] }));
	const inputs = (r && r.inputs) || [];
	return inputs.length ? inputs[0].inputName : TEL_NAME;
}

async function sceneExists(name) {
	const r = await obs.request("GetSceneList");
	return (r.scenes || []).some((s) => s.sceneName === name);
}

async function itemId(scene, source) {
	try {
		const r = await obs.request("GetSceneItemId", { sceneName: scene, sourceName: source });
		return r.sceneItemId;
	} catch {
		return null;
	}
}

async function probe() {
	const v = await obs.request("GetVersion");
	const kinds = v.supportedImageFormats ? "" : "";
	const hasKind = (v.availableRequests || []) && true;
	// Plugin presence = can we create/find the telestrator input kind?
	const kindList = await obs.request("GetInputKindList").catch(() => ({ inputKinds: [] }));
	const loaded = (kindList.inputKinds || []).includes(TEL_KIND);
	console.log(`OBS ${v.obsVersion} (ws ${v.obsWebSocketVersion})`);
	console.log(`telestrator source kind registered: ${loaded ? "YES" : "NO"}`);
	void kinds;
	void hasKind;
	return loaded;
}

async function setup() {
	// Scene
	if (!(await sceneExists(TEST_SCENE))) await obs.request("CreateScene", { sceneName: TEST_SCENE });

	// Backdrop: a mid-gray color source filling the canvas, so ink is visible and
	// the screenshot isn't transparent. color_source_v3 takes ABGR int `color`.
	if ((await itemId(TEST_SCENE, BACKDROP)) == null) {
		const exists = await obs.ok("GetInputSettings", { inputName: BACKDROP });
		if (exists) {
			await obs.request("CreateSceneItem", { sceneName: TEST_SCENE, sourceName: BACKDROP, sceneItemEnabled: true });
		} else {
			await obs.request("CreateInput", {
				sceneName: TEST_SCENE,
				inputName: BACKDROP,
				inputKind: "color_source_v3",
				inputSettings: { color: 0xff202020, width: 1920, height: 1080 },
				sceneItemEnabled: true,
			});
		}
	}

	// Telestrator source on top (reference the single existing one, or create it).
	const tel = await telName();
	if ((await itemId(TEST_SCENE, tel)) == null) {
		const exists = await obs.ok("GetInputSettings", { inputName: tel });
		if (exists) {
			await obs.request("CreateSceneItem", { sceneName: TEST_SCENE, sourceName: tel, sceneItemEnabled: true });
		} else {
			await obs.request("CreateInput", {
				sceneName: TEST_SCENE,
				inputName: tel,
				inputKind: TEL_KIND,
				sceneItemEnabled: true,
			});
		}
	}

	await obs.request("SetCurrentProgramScene", { sceneName: TEST_SCENE });
	console.log(`Set up "${TEST_SCENE}" (backdrop + ${tel}) and switched program to it.`);
}

async function shot(file) {
	if (!file) throw new Error("shot needs a file path");
	const r = await obs.request("GetCurrentProgramScene");
	const scene = r.currentProgramSceneName || r.sceneName;
	await obs.request("SaveSourceScreenshot", {
		sourceName: scene,
		imageFormat: "png",
		imageFilePath: file,
	});
	console.log(`Saved screenshot of "${scene}" -> ${file}`);
}

async function main() {
	await obs.connect();
	switch (cmd) {
		case "probe":
			process.exitCode = (await probe()) ? 0 : 2;
			break;
		case "setup":
			await setup();
			break;
		case "arm":
			await obs.request("TriggerHotkeyByName", { hotkeyName: "telestrator.armon" });
			console.log("armed (telestrator.armon)");
			break;
		case "disarm":
			await obs.request("TriggerHotkeyByName", { hotkeyName: "telestrator.armoff" });
			console.log("disarmed (telestrator.armoff)");
			break;
		case "projector":
			await obs.request("OpenVideoMixProjector", {
				videoMixType: "OBS_WEBSOCKET_VIDEO_MIX_TYPE_PROGRAM",
				monitorIndex: -1,
			});
			console.log("opened windowed Program projector");
			break;
		case "shot":
			await shot(args[0]);
			break;
		case "req": {
			const out = await obs.request(args[0], args[1] ? JSON.parse(args[1]) : {});
			console.log(JSON.stringify(out, null, 2));
			break;
		}
		default:
			console.error("unknown command: " + cmd);
			process.exitCode = 1;
	}
	obs.close();
}

main().catch((e) => {
	console.error("ERROR:", e.message);
	process.exitCode = 1;
	obs.close();
});
