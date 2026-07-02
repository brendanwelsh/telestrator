/*
Telestrator — a broadcast-style "draw on top of your stream" source for OBS Studio.

Native C++/libobs port of the Lua engine (obs-telestrator-lua). Lineage:
  * Original obs-whiteboard: Herschel.
  * Second: Tari.
  * Lua port (obs-whiteboard-lua): katarai.
  * Telestrator + this C++ port: Brendan Welsh.
MIT licensed — see LICENSE.

Architecture mirrors the Lua engine 1:1:
  - The source owns two canvas-sized render-target textures: `canvas` (committed
    ink) and `preview` (transient overlay, rebuilt every tick).
  - Permanent strokes are baked into `canvas`. Pen/shapes/laser preview smoothly
    on `preview` each frame (Catmull-Rom for freehand); the eraser paints
    destructively onto `canvas`. Strokes bake on mouse-up.
  - Stroke history (g_strokes / g_redo) drives undo/redo by replay.
  - Temp strokes (laser / auto-fade) live in g_temp, fade each frame, get culled.
  - Hotkey handlers (OBS hotkey thread) only set flags; video_tick processes
    them on the graphics thread.

Engine-wide drawing state is file-scope (one logical telestrator, matching the
Lua design + the dock/Stream-Deck compat contract). Per-source data holds only
the textures, size, and mouse position.

Input is currently global-cursor polling + projector window-title matching (Win32),
matching the Lua engine. Native Qt input on the OBS preview panel is the next
milestone.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <plugin-support.h>

#include "obs-websocket-api.h" // obs-websocket vendor API (header-only proc-handler shim)

#include <QWidget>
#include <QPushButton>
#include <QToolButton>
#include <QButtonGroup>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QMainWindow>
#include <QDockWidget>
#include <QColorDialog>
#include <QColor>
#include <QIcon>
#include <QSize>
#include <QDialog>
#include <QFormLayout>
#include <QSpinBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QSlider>
#include <QTimer>
#include <QScrollArea>
#include <QFrame>
#include <QSizePolicy>
#include <QMouseEvent>
#include <QShowEvent>
#include <QResizeEvent>
#include <QMenu>
#include <QContextMenuEvent>
#include <initializer_list>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("telestrator", "en-US")

namespace {

// ---------------------------------------------------------------------------
// Tools / constants
// ---------------------------------------------------------------------------
enum Tool {
	TOOL_PEN = 1, TOOL_LINE = 2, TOOL_ARROW = 3, TOOL_RECT = 4,
	TOOL_ELLIPSE = 5, TOOL_DBLARROW = 6, TOOL_CURVEDARROW = 7, TOOL_SPOTLIGHT = 8,
	TOOL_FIRSTDOWN = 9, TOOL_CONE = 10, TOOL_VERTICAL = 11
};
constexpr int TOOL_COUNT = 11;

constexpr float PI_F = 3.14159265358979323846f;
constexpr int ELLIPSE_SEGS = 64;
constexpr float ARROW_SPREAD = 0.46f; // radians; half-angle of the arrowhead
constexpr int FREEHAND_SUB = 12;      // Catmull-Rom subdivisions per point span (smoother)
// Ink rasterizes into render targets SS x the canvas size and is drawn back at
// canvas size (exact 2x -> a 2x2 box filter downsample). libobs' solid effect
// has no MSAA path, so this supersample IS the anti-aliasing: it's what removes
// the hard stair-step edges on every stroke.
constexpr uint32_t SS = 2;
constexpr float LASER_LIFE = 1.3f;
constexpr float LASER_FADE = 0.5f;
constexpr int INDICATOR_SIZE = 10;
constexpr int INDICATOR_X = 34;
constexpr int INDICATOR_Y = 34;

// Preset colors: Yellow, Red, Green, Blue, White, Orange, Cyan, Custom(black).
// Stored as 0xAABBGGRR so vec4_from_rgba (low byte -> red) yields the color.
// Blue is 0xFFFFB728 = RGB(40,183,255); the prior 0xFFB7FF28 had its B/G bytes
// transposed and decoded to a turquoise green (a near-duplicate of Cyan).
uint32_t g_color_array[8] = {0xFF28FFFF, 0xFF0000FF, 0xFF00FF50, 0xFFFFB728,
			     0xFFFFFFFF, 0xFF0080FF, 0xFFFFFF00, 0xFF000000};
constexpr int COLOR_COUNT = 8;

// ---------------------------------------------------------------------------
// Stroke model
// ---------------------------------------------------------------------------
struct Vec2 {
	float x, y;
};

struct Stroke {
	int tool = TOOL_PEN;
	uint32_t color = 0xFFFFFFFF;
	int size = 2;
	bool eraser = false;
	bool temporary = false;
	std::vector<Vec2> points;
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	float born = 0, life = 0, fade = 0;
	bool dashed = false;     // dashed style (shapes only)
	bool filled = false;     // filled interior (rect/ellipse)
	float opacity = 1.0f;    // <1 for highlighter (translucent) strokes
};

// ---------------------------------------------------------------------------
// Engine-wide drawing state (file scope == one logical telestrator)
// ---------------------------------------------------------------------------
bool g_drawing_enabled = false; // the master "armed" toggle
bool g_eraser = false;
bool g_laser_mode = false;
int g_autofade_secs = 0;
bool g_show_indicator = false;

int g_tool = TOOL_PEN;
int g_color_index = 5; // 1-based; default White
int g_size = 2;
bool g_dashed = false;      // dashed style toggle (applies to shape strokes)
bool g_filled = false;      // filled-shape toggle (rect/ellipse)
bool g_highlighter = false; // translucent highlighter strokes
float g_opacity = 1.0f;     // base opacity for new strokes (transparency control)
constexpr int BRUSH_MAX = 12;

// CANONICAL input: the "Telestrator Draw" dock. It writes canvas coords + button
// state here from native Qt events on the UI thread; the graphics tick reads them
// and runs the full begin/continue/commit path. No global polling, no window-title
// matching, cross-platform — this is the native surface the C++ port was built for.
std::atomic<bool> g_dock_down{false};
std::atomic<float> g_dock_mx{0.0f}, g_dock_my{0.0f};
// Draw dock scaling mode: fit (letterbox, default) or fill (crop). Render and
// input mapping share this so ink stays pixel-exact in both modes.
std::atomic<bool> g_drawpad_fill{false};

// Scripted stroke playback (obs-websocket vendor request "sim_stroke"): timed
// canvas-space points fed through the SAME dock input path a human uses, so
// automated demos/tests exercise the real begin/continue/commit code with no
// window focus games. Queue written on the websocket thread, consumed on the
// graphics thread; playback state is graphics-thread-only.
struct SimStroke {
	std::vector<Vec2> pts;
	float duration = 0.4f; // seconds start -> finish
};
std::mutex g_sim_mtx;
std::vector<SimStroke> g_sim_queue;
SimStroke g_sim_active;
float g_sim_t = -1.0f; // <0 == idle

// LEGACY input (opt-in, default OFF): draw on a windowed projector or the OBS main
// preview via global Win32 cursor polling + window-title matching. This is the
// fragile path inherited from the Lua engine — Windows-only, foreground-dependent,
// and it couples to OBS UI internals (the "preview" widget + PREVIEW_EDGE_SIZE).
// CLAUDE.md names it "the single most fragile part"; the dock replaces it. Kept
// behind a Settings toggle so the projector workflow is recoverable, not the
// default. When off, none of the polling / title-matching / preview-rect code runs.
std::atomic<bool> g_legacy_cursor_input{false};

// "Preview mode" rect (legacy only): the OBS main preview widget's physical screen
// rect, refreshed on the UI thread while g_legacy_cursor_input is on.
std::atomic<bool> g_prev_valid{false};
std::atomic<float> g_prev_l{0.0f}, g_prev_t{0.0f}, g_prev_w{0.0f}, g_prev_h{0.0f};

std::vector<Stroke> g_strokes;     // committed permanent strokes
std::vector<Stroke> g_redo;        // redo stack
std::vector<Stroke> g_temp;        // transient fading strokes
std::unique_ptr<Stroke> g_active;  // stroke currently being drawn

float g_clock = 0.0f; // monotonic seconds from video_tick dt

// Projector window-title keyword (localized installs can override).
std::string g_projector_name = "Projector";
std::string g_scene_name;

struct vec4 g_eraser_v4;

// Command flags (set by hotkey handlers, processed in video_tick).
bool f_clear = false, f_colorswap = false, f_sizetoggle = false, f_toolcycle = false;
bool f_undo = false, f_redo = false, f_toggle = false, f_laser = false, f_sizedown = false;
bool f_arm_on = false, f_arm_off = false, f_close_proj = false;
std::string g_pending_close_match; // empty == close all

// ---------------------------------------------------------------------------
// Per-source data
// ---------------------------------------------------------------------------
struct TelSource {
	obs_source_t *source = nullptr;
	uint32_t width = 1920;
	uint32_t height = 1080;
	gs_texture_t *canvas = nullptr;
	gs_texture_t *preview = nullptr;
	gs_texture_t *scratch = nullptr; // one-stroke staging target for uniform translucency
	bool active = false;
	bool has_mouse = false;
	float mouse_x = 0, mouse_y = 0;
};

// ---------------------------------------------------------------------------
// Vertex-buffer cache: a filled disc "dot" (round caps/joins) + a unit "line"
// quad, sized per brush radius. Cached so undo/replay is cheap.
// ---------------------------------------------------------------------------
struct VBuf {
	gs_vertbuffer_t *line = nullptr;
	gs_vertbuffer_t *dot = nullptr;
};
std::unordered_map<int, VBuf> g_vbuf;
gs_vertbuffer_t *g_dot_vert = nullptr;
gs_vertbuffer_t *g_line_vert = nullptr;
int g_vert_size = 0;

void ensure_vertices(int s)
{
	if (g_vbuf.count(s))
		return;
	obs_enter_graphics();

	// LINE: a quad of height (s*2), length 1 unit in x (two triangles).
	gs_render_start(true);
	float width = (float)(s * 2);
	gs_vertex2f(0, 0);
	gs_vertex2f((float)s, 0);
	gs_vertex2f(0, width);
	gs_vertex2f(0, width);
	gs_vertex2f((float)s, width);
	gs_vertex2f((float)s, 0);
	gs_vertbuffer_t *lv = gs_render_save();

	// DOT: filled disc radius s centered at (s,s), as a triangle list (fan).
	int dot_segs = (int)fmaxf(16.0f, fminf(64.0f, floorf((float)s * 4.0f)));
	gs_render_start(true);
	for (int i = 0; i < dot_segs; i++) {
		float a0 = ((float)i / (float)dot_segs) * 2.0f * PI_F;
		float a1 = ((float)(i + 1) / (float)dot_segs) * 2.0f * PI_F;
		gs_vertex2f((float)s, (float)s);
		gs_vertex2f((float)s + (float)s * cosf(a0), (float)s + (float)s * sinf(a0));
		gs_vertex2f((float)s + (float)s * cosf(a1), (float)s + (float)s * sinf(a1));
	}
	gs_vertbuffer_t *dv = gs_render_save();

	obs_leave_graphics();
	g_vbuf[s] = VBuf{lv, dv};
}

void select_vertices(int s)
{
	auto it = g_vbuf.find(s);
	if (it == g_vbuf.end()) {
		ensure_vertices(s);
		it = g_vbuf.find(s);
	}
	g_line_vert = it->second.line;
	g_dot_vert = it->second.dot;
	g_vert_size = s;
}

// One thick rounded segment. Caller is inside graphics, render target + ortho
// set, solid technique active, select_vertices() already called.
void draw_segment(float x0, float y0, float x1, float y1)
{
	float b = (float)g_vert_size;
	float dx = x1 - x0;
	float dy = y1 - y0;
	float len = sqrtf(dx * dx + dy * dy);
	float angle = atan2f(dy, dx);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate3f(x0, y0, 0);

	// start cap
	gs_matrix_push();
	gs_matrix_translate3f(-b, -b, 0);
	gs_load_vertexbuffer(g_dot_vert);
	gs_draw(GS_TRIS, 0, 0);
	gs_matrix_pop();

	// line body
	gs_matrix_rotaa4f(0, 0, 1, angle);
	gs_matrix_translate3f(0, -b, 0);
	if (len > 0) {
		gs_matrix_scale3f(len / b, 1.0f, 1.0f);
		gs_load_vertexbuffer(g_line_vert);
		gs_draw(GS_TRIS, 0, 0);
	}

	// end cap
	gs_matrix_identity();
	gs_matrix_translate3f(x1, y1, 0);
	gs_matrix_translate3f(-b, -b, 0);
	gs_load_vertexbuffer(g_dot_vert);
	gs_draw(GS_TRIS, 0, 0);

	gs_matrix_pop();
}

// ---------------------------------------------------------------------------
// Stroke geometry -> segment list
// ---------------------------------------------------------------------------
inline bool is_freehand(int t)
{
	return t == TOOL_PEN;
}

struct Seg {
	float x0, y0, x1, y1;
};

struct Tri {
	Vec2 a, b, c;
};

// A stroke's renderable geometry: stroked segments (round caps/joins), filled
// triangles at full ink alpha (solid arrowheads — the "broadcast" look, not
// stroked V-heads), and filled triangles at reduced alpha (the cone wedge).
struct StrokeGeom {
	std::vector<Seg> segs;
	std::vector<Tri> tris;      // full-alpha fills (arrowheads)
	std::vector<Tri> soft_tris; // translucent fills (cone interior)
};

// Cut `cut` length off the tail of a connected polyline so a solid arrowhead
// can sit on the end without the shaft's round cap poking past the tip.
void trim_polyline_tail(std::vector<Seg> &segs, float cut)
{
	while (!segs.empty() && cut > 0.0f) {
		Seg &sg = segs.back();
		float dx = sg.x1 - sg.x0, dy = sg.y1 - sg.y0;
		float len = sqrtf(dx * dx + dy * dy);
		if (len <= cut) {
			cut -= len;
			segs.pop_back();
			continue;
		}
		float t = (len - cut) / len;
		sg.x1 = sg.x0 + dx * t;
		sg.y1 = sg.y0 + dy * t;
		break;
	}
}

void trim_polyline_head(std::vector<Seg> &segs, float cut)
{
	while (!segs.empty() && cut > 0.0f) {
		Seg &sg = segs.front();
		float dx = sg.x1 - sg.x0, dy = sg.y1 - sg.y0;
		float len = sqrtf(dx * dx + dy * dy);
		if (len <= cut) {
			cut -= len;
			segs.erase(segs.begin());
			continue;
		}
		float t = cut / len;
		sg.x0 = sg.x0 + dx * t;
		sg.y0 = sg.y0 + dy * t;
		break;
	}
}

float arrow_head_len(const Stroke &s, float shaft_len)
{
	return fminf(shaft_len, fmaxf(22.0f, (float)s.size * 7.0f));
}

// Solid triangular arrowhead with the tip at (tx,ty), pointing along `ang`.
// Returns the length to trim off the shaft (tip -> base-center distance).
float add_arrowhead(StrokeGeom &g, float tx, float ty, float ang, float head)
{
	float lx = tx - head * cosf(ang - ARROW_SPREAD);
	float ly = ty - head * sinf(ang - ARROW_SPREAD);
	float rx = tx - head * cosf(ang + ARROW_SPREAD);
	float ry = ty - head * sinf(ang + ARROW_SPREAD);
	g.tris.push_back({{tx, ty}, {lx, ly}, {rx, ry}});
	// trim to just short of the base so shaft and head overlap slightly (no seam)
	return head * cosf(ARROW_SPREAD) * 0.85f;
}

StrokeGeom stroke_geometry(const Stroke &s)
{
	StrokeGeom geom;
	std::vector<Seg> &segs = geom.segs;
	int t = s.tool;
	if (is_freehand(t)) {
		const auto &pts = s.points;
		if (pts.size() <= 1) {
			float x = pts.empty() ? s.x0 : pts[0].x;
			float y = pts.empty() ? s.y0 : pts[0].y;
			segs.push_back({x, y, x, y});
			return geom;
		}
		if (pts.size() == 2) {
			segs.push_back({pts[0].x, pts[0].y, pts[1].x, pts[1].y});
			return geom;
		}
		// Catmull-Rom spline through the raw points → smooth curve. Endpoints
		// clamped so the curve starts/ends exactly on the first/last points.
		int n = (int)pts.size();
		auto pt = [&](int i) -> const Vec2 & {
			if (i < 0)
				i = 0;
			else if (i > n - 1)
				i = n - 1;
			return pts[i];
		};
		float prevx = pts[0].x, prevy = pts[0].y;
		for (int i = 0; i < n - 1; i++) {
			const Vec2 &p0 = pt(i - 1);
			const Vec2 &p1 = pt(i);
			const Vec2 &p2 = pt(i + 1);
			const Vec2 &p3 = pt(i + 2);
			for (int j = 1; j <= FREEHAND_SUB; j++) {
				float u = (float)j / (float)FREEHAND_SUB;
				float u2 = u * u;
				float u3 = u2 * u;
				float x = 0.5f * ((2 * p1.x) + (-p0.x + p2.x) * u +
						  (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * u2 +
						  (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * u3);
				float y = 0.5f * ((2 * p1.y) + (-p0.y + p2.y) * u +
						  (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * u2 +
						  (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * u3);
				segs.push_back({prevx, prevy, x, y});
				prevx = x;
				prevy = y;
			}
		}
		return geom;
	} else if (t == TOOL_LINE) {
		segs.push_back({s.x0, s.y0, s.x1, s.y1});
	} else if (t == TOOL_ARROW) {
		float a = atan2f(s.y1 - s.y0, s.x1 - s.x0);
		float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
		float len = sqrtf(dx * dx + dy * dy);
		float head = arrow_head_len(s, len);
		segs.push_back({s.x0, s.y0, s.x1, s.y1});
		trim_polyline_tail(segs, add_arrowhead(geom, s.x1, s.y1, a, head));
	} else if (t == TOOL_DBLARROW) {
		float a = atan2f(s.y1 - s.y0, s.x1 - s.x0);
		float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
		float len = sqrtf(dx * dx + dy * dy);
		float head = fminf(len * 0.5f, fmaxf(22.0f, (float)s.size * 7.0f));
		segs.push_back({s.x0, s.y0, s.x1, s.y1});
		trim_polyline_tail(segs, add_arrowhead(geom, s.x1, s.y1, a, head));
		trim_polyline_head(segs, add_arrowhead(geom, s.x0, s.y0, a + PI_F, head));
	} else if (t == TOOL_CURVEDARROW) {
		float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
		float len = sqrtf(dx * dx + dy * dy);
		// Control point FOLLOWS THE DRAG PATH: sweep the cursor in an arc and
		// the arrow bows that way (either side, any depth). The recorded path
		// point farthest off the chord anchors the curve; a straight drag falls
		// back to a gentle default bow.
		float cx = 0.0f, cy = 0.0f;
		bool from_path = false;
		if (len > 1.0f && s.points.size() >= 3) {
			float ux = dx / len, uy = dy / len;
			float best = 0.0f;
			Vec2 bp{0.0f, 0.0f};
			for (const Vec2 &p : s.points) {
				float rx = p.x - s.x0, ry = p.y - s.y0;
				float d = rx * -uy + ry * ux; // signed distance off the chord
				if (fabsf(d) > fabsf(best)) {
					best = d;
					bp = p;
				}
			}
			if (fabsf(best) > len * 0.02f) {
				// quadratic bezier passing through bp at t=0.5
				cx = 2.0f * bp.x - (s.x0 + s.x1) * 0.5f;
				cy = 2.0f * bp.y - (s.y0 + s.y1) * 0.5f;
				from_path = true;
			}
		}
		if (!from_path) {
			float midx = (s.x0 + s.x1) * 0.5f, midy = (s.y0 + s.y1) * 0.5f;
			float perpx = -dy, perpy = dx;
			float plen = sqrtf(perpx * perpx + perpy * perpy);
			if (plen > 0.001f) {
				perpx /= plen;
				perpy /= plen;
			}
			cx = midx + perpx * len * 0.22f;
			cy = midy + perpy * len * 0.22f;
		}
		// quadratic bezier shaft
		float px = s.x0, py = s.y0;
		const int N = 24;
		for (int i = 1; i <= N; i++) {
			float u = (float)i / (float)N, iu = 1.0f - u;
			float bx = iu * iu * s.x0 + 2 * iu * u * cx + u * u * s.x1;
			float by = iu * iu * s.y0 + 2 * iu * u * cy + u * u * s.y1;
			segs.push_back({px, py, bx, by});
			px = bx;
			py = by;
		}
		// arrowhead at the end, aligned to the curve's tangent there
		float a = atan2f(s.y1 - cy, s.x1 - cx);
		float head = arrow_head_len(s, len);
		trim_polyline_tail(segs, add_arrowhead(geom, s.x1, s.y1, a, head));
	} else if (t == TOOL_RECT) {
		segs.push_back({s.x0, s.y0, s.x1, s.y0});
		segs.push_back({s.x1, s.y0, s.x1, s.y1});
		segs.push_back({s.x1, s.y1, s.x0, s.y1});
		segs.push_back({s.x0, s.y1, s.x0, s.y0});
	} else if (t == TOOL_ELLIPSE) {
		float cx = (s.x0 + s.x1) / 2;
		float cy = (s.y0 + s.y1) / 2;
		float rx = fabsf(s.x1 - s.x0) / 2;
		float ry = fabsf(s.y1 - s.y0) / 2;
		float px = 0, py = 0;
		for (int i = 0; i <= ELLIPSE_SEGS; i++) {
			float th = ((float)i / (float)ELLIPSE_SEGS) * 2.0f * PI_F;
			float x = cx + rx * cosf(th);
			float y = cy + ry * sinf(th);
			if (i > 0)
				segs.push_back({px, py, x, y});
			px = x;
			py = y;
		}
	} else if (t == TOOL_FIRSTDOWN) {
		// full-width horizontal guide line at the drawn y (clipped to the canvas)
		segs.push_back({-20000.0f, s.y0, 20000.0f, s.y0});
	} else if (t == TOOL_VERTICAL) {
		// full-height vertical guide line at the drawn x (clipped to the canvas)
		segs.push_back({s.x0, -20000.0f, s.x0, 20000.0f});
	} else if (t == TOOL_CONE) {
		// vision/passing cone: a wedge from the apex (start) opening toward the
		// end — translucent filled interior + crisp edges (broadcast style)
		float a = atan2f(s.y1 - s.y0, s.x1 - s.x0);
		float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
		float len = sqrtf(dx * dx + dy * dy);
		float spread = 0.42f; // ~24 deg half-angle
		float lx = s.x0 + len * cosf(a - spread), ly = s.y0 + len * sinf(a - spread);
		float rx = s.x0 + len * cosf(a + spread), ry = s.y0 + len * sinf(a + spread);
		segs.push_back({s.x0, s.y0, lx, ly});
		segs.push_back({s.x0, s.y0, rx, ry});
		segs.push_back({lx, ly, rx, ry});
		geom.soft_tris.push_back({{s.x0, s.y0}, {lx, ly}, {rx, ry}});
	}
	return geom;
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------
// Run fn() with a render target bound, viewport + ortho for the canvas, and a
// fresh blend state. Restores everything afterwards. The viewport covers the
// SS-supersampled texture while the ortho stays in logical canvas coords, so
// all stroke geometry rasterizes at SS x resolution transparently.
template<typename Fn> void with_target(TelSource *d, gs_texture_t *tex, bool clear_first, Fn fn)
{
	obs_enter_graphics();
	gs_texture_t *prev_rt = gs_get_render_target();
	gs_zstencil_t *prev_zs = gs_get_zstencil_target();

	gs_set_render_target(tex, nullptr);
	gs_viewport_push();
	gs_set_viewport(0, 0, (int)(d->width * SS), (int)(d->height * SS));
	gs_projection_push();
	gs_ortho(0.0f, (float)d->width, 0.0f, (float)d->height, 0.0f, 1.0f);
	gs_blend_state_push();
	gs_reset_blend_state();

	if (clear_first) {
		struct vec4 zero;
		vec4_zero(&zero);
		gs_clear(GS_CLEAR_COLOR, &zero, 1.0f, 0);
	}

	fn();

	gs_blend_state_pop();
	gs_projection_pop();
	gs_viewport_pop();
	gs_set_render_target(prev_rt, prev_zs);
	obs_leave_graphics();
}

// Render a segment list. Caller must be inside with_target(). alpha (0..1)
// multiplies the ink alpha.
void render_segments(const Stroke &s, const std::vector<Seg> &segs, float alpha)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	if (s.eraser) {
		gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_SRCALPHA);
		gs_effect_set_vec4(color_param, &g_eraser_v4);
	} else {
		gs_reset_blend_state();
		struct vec4 c;
		vec4_from_rgba(&c, s.color);
		c.w = c.w * alpha;
		gs_effect_set_vec4(color_param, &c);
	}

	select_vertices(s.size);
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	for (const auto &seg : segs)
		draw_segment(seg.x0, seg.y0, seg.x1, seg.y1);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

// Render filled triangles in the stroke color (arrowheads, cone interior).
// Caller must be inside with_target().
void render_tris(const Stroke &s, const std::vector<Tri> &tris, float alpha)
{
	if (tris.empty())
		return;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
	gs_reset_blend_state();
	struct vec4 c;
	vec4_from_rgba(&c, s.color);
	c.w = c.w * alpha;
	gs_effect_set_vec4(color_param, &c);

	gs_render_start(true);
	for (const auto &t : tris) {
		gs_vertex2f(t.a.x, t.a.y);
		gs_vertex2f(t.b.x, t.b.y);
		gs_vertex2f(t.c.x, t.c.y);
	}
	gs_vertbuffer_t *vb = gs_render_save();
	gs_load_vertexbuffer(vb);
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw(GS_TRIS, 0, 0);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
	gs_load_vertexbuffer(nullptr);
	gs_vertexbuffer_destroy(vb);
}

// Expand a segment list into dashes. The dash phase carries ACROSS segments so
// multi-segment shapes (rects, ellipses, curves) dash continuously instead of
// restarting at every joint (which made dashed ellipses render solid). dash/gap
// are geometric lengths; draw_segment adds a round cap of radius `size` on each
// dash end, so callers pass a gap larger than the visual gap they want.
std::vector<Seg> dash_segments(const std::vector<Seg> &segs, float dash, float gap)
{
	std::vector<Seg> out;
	float period = dash + gap;
	float phase = 0.0f; // distance into the current period; ink is on while phase < dash
	for (const auto &sg : segs) {
		float dx = sg.x1 - sg.x0, dy = sg.y1 - sg.y0;
		float len = sqrtf(dx * dx + dy * dy);
		if (len < 0.001f)
			continue;
		float ux = dx / len, uy = dy / len;
		float pos = 0.0f;
		while (pos < len - 0.0001f) {
			bool on = phase < dash;
			float budget = on ? (dash - phase) : (period - phase);
			float step = fminf(budget, len - pos);
			if (on)
				out.push_back({sg.x0 + ux * pos, sg.y0 + uy * pos, sg.x0 + ux * (pos + step),
					       sg.y0 + uy * (pos + step)});
			pos += step;
			phase += step;
			if (phase >= period - 0.0001f)
				phase = 0.0f;
		}
	}
	return out;
}

// Filled interior for rect/ellipse (translucent). Caller is inside with_target().
void render_fill(const Stroke &s, float alpha)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
	gs_reset_blend_state();
	struct vec4 c;
	vec4_from_rgba(&c, s.color);
	c.w = c.w * alpha;
	gs_effect_set_vec4(color_param, &c);

	gs_render_start(true);
	if (s.tool == TOOL_RECT) {
		gs_vertex2f(s.x0, s.y0);
		gs_vertex2f(s.x1, s.y0);
		gs_vertex2f(s.x0, s.y1);
		gs_vertex2f(s.x0, s.y1);
		gs_vertex2f(s.x1, s.y0);
		gs_vertex2f(s.x1, s.y1);
	} else { // ellipse as a triangle fan emitted as a list
		float cx = (s.x0 + s.x1) * 0.5f, cy = (s.y0 + s.y1) * 0.5f;
		float rx = fabsf(s.x1 - s.x0) * 0.5f, ry = fabsf(s.y1 - s.y0) * 0.5f;
		for (int i = 0; i < ELLIPSE_SEGS; i++) {
			float a0 = (float)i / (float)ELLIPSE_SEGS * 2.0f * PI_F;
			float a1 = (float)(i + 1) / (float)ELLIPSE_SEGS * 2.0f * PI_F;
			gs_vertex2f(cx, cy);
			gs_vertex2f(cx + rx * cosf(a0), cy + ry * sinf(a0));
			gs_vertex2f(cx + rx * cosf(a1), cy + ry * sinf(a1));
		}
	}
	gs_vertbuffer_t *vb = gs_render_save();
	gs_load_vertexbuffer(vb);
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw(GS_TRIS, 0, 0);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
	gs_load_vertexbuffer(nullptr);
	gs_vertexbuffer_destroy(vb);
}

// Spotlight: darken the whole canvas except a transparent ellipse, so the
// underlying scene shows through only there (a "spotlight" on a player/region).
// Draw it FIRST (it's a background dimmer) then annotate inside the bright area.
void render_spotlight(const Stroke &s, float fade)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	// 1) dark fill over the whole canvas (viewport clips the oversized quad)
	gs_reset_blend_state();
	struct vec4 dark;
	vec4_set(&dark, 0.0f, 0.0f, 0.0f, 0.6f * fade);
	gs_effect_set_vec4(color_param, &dark);
	gs_render_start(true);
	gs_vertex2f(0, 0);
	gs_vertex2f(20000, 0);
	gs_vertex2f(0, 20000);
	gs_vertex2f(0, 20000);
	gs_vertex2f(20000, 0);
	gs_vertex2f(20000, 20000);
	gs_vertbuffer_t *vbg = gs_render_save();
	gs_load_vertexbuffer(vbg);
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw(GS_TRIS, 0, 0);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
	gs_vertexbuffer_destroy(vbg);

	// 2) erase the ellipse (clears alpha -> transparent hole = the spotlight)
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_SRCALPHA);
	gs_effect_set_vec4(color_param, &g_eraser_v4);
	float cx = (s.x0 + s.x1) * 0.5f, cy = (s.y0 + s.y1) * 0.5f;
	float rx = fabsf(s.x1 - s.x0) * 0.5f, ry = fabsf(s.y1 - s.y0) * 0.5f;
	gs_render_start(true);
	for (int i = 0; i < ELLIPSE_SEGS; i++) {
		float a0 = (float)i / (float)ELLIPSE_SEGS * 2.0f * PI_F;
		float a1 = (float)(i + 1) / (float)ELLIPSE_SEGS * 2.0f * PI_F;
		gs_vertex2f(cx, cy);
		gs_vertex2f(cx + rx * cosf(a0), cy + ry * sinf(a0));
		gs_vertex2f(cx + rx * cosf(a1), cy + ry * sinf(a1));
	}
	gs_vertbuffer_t *vbe = gs_render_save();
	gs_load_vertexbuffer(vbe);
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw(GS_TRIS, 0, 0);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
	gs_load_vertexbuffer(nullptr);
	gs_vertexbuffer_destroy(vbe);
}

// Draw a whole stroke's geometry into the CURRENTLY BOUND target at `alpha`.
void render_stroke_direct(const Stroke &s, float alpha)
{
	if (s.tool == TOOL_SPOTLIGHT) {
		render_spotlight(s, alpha);
		return;
	}
	if (s.filled)
		render_fill(s, alpha * 0.35f); // translucent interior
	StrokeGeom geom = stroke_geometry(s);
	if (s.dashed)
		// Geometric dash/gap chosen so the VISUAL rhythm (after round caps grow
		// each dash by the stroke width) is ~2.5w on / ~1.5w off. The old numbers
		// ignored the caps, which closed the gaps into a blobby near-solid line.
		geom.segs = dash_segments(geom.segs, (float)s.size * 3.0f, (float)s.size * 5.0f);
	render_tris(s, geom.soft_tris, alpha * 0.30f); // cone interior
	render_segments(s, geom.segs, alpha);
	render_tris(s, geom.tris, alpha); // solid arrowheads on top
}

// Alpha-composite effect: draws a texture with its alpha scaled by a uniform.
// Lets a stroke rendered OPAQUE into `scratch` land on the canvas as one
// uniform translucent layer — overlapping caps/joints no longer stack into
// darker blobs (the old MS-Paint highlighter look).
const char *FADE_EFFECT_SRC = "\
uniform float4x4 ViewProj;\n\
uniform texture2d image;\n\
uniform float opacity;\n\
sampler_state texSampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n\
struct VertInOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n\
VertInOut VSDefault(VertInOut vert_in)\n\
{\n\
	VertInOut vert_out;\n\
	vert_out.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);\n\
	vert_out.uv = vert_in.uv;\n\
	return vert_out;\n\
}\n\
float4 PSDrawFade(VertInOut vert_in) : TARGET\n\
{\n\
	float4 c = image.Sample(texSampler, vert_in.uv);\n\
	return float4(c.rgb, c.a * opacity);\n\
}\n\
technique Draw\n\
{\n\
	pass\n\
	{\n\
		vertex_shader = VSDefault(vert_in);\n\
		pixel_shader = PSDrawFade(vert_in);\n\
	}\n\
}\n";

gs_effect_t *g_fade_effect = nullptr;
bool g_fade_effect_tried = false;

gs_effect_t *fade_effect()
{
	if (!g_fade_effect && !g_fade_effect_tried) {
		g_fade_effect_tried = true;
		obs_enter_graphics();
		char *err = nullptr;
		g_fade_effect = gs_effect_create(FADE_EFFECT_SRC, "telestrator_fade", &err);
		if (!g_fade_effect)
			obs_log(LOG_ERROR, "telestrator: fade effect compile failed: %s", err ? err : "(no detail)");
		if (err)
			bfree(err);
		obs_leave_graphics();
	}
	return g_fade_effect;
}

// Lay `scratch` over `dest` with alpha scaled to `alpha`. Same-size textures and
// a full-viewport quad -> exact 1:1 texel copy, just with the alpha multiply.
void composite_scratch(TelSource *d, gs_texture_t *dest, float alpha)
{
	gs_effect_t *eff = fade_effect();
	if (!eff)
		return;
	with_target(d, dest, false, [&]() {
		gs_effect_set_texture(gs_effect_get_param_by_name(eff, "image"), d->scratch);
		gs_effect_set_float(gs_effect_get_param_by_name(eff, "opacity"), alpha);
		gs_reset_blend_state();
		while (gs_effect_loop(eff, "Draw"))
			gs_draw_sprite(d->scratch, 0, d->width, d->height);
	});
}

// Render one stroke into `dest`. Translucent ink (highlighter, opacity dial,
// laser/auto-fade mid-fade) goes through `scratch` opaque-first so it reads as
// one uniform layer; opaque ink and the eraser draw straight in.
void render_stroke_into(TelSource *d, gs_texture_t *dest, const Stroke &s, float fade)
{
	float alpha = fade * s.opacity;
	bool uniform = !s.eraser && s.tool != TOOL_SPOTLIGHT && alpha < 0.995f && d->scratch && fade_effect();
	if (!uniform) {
		with_target(d, dest, false, [&]() { render_stroke_direct(s, alpha); });
	} else {
		with_target(d, d->scratch, true, [&]() { render_stroke_direct(s, 1.0f); });
		composite_scratch(d, dest, alpha);
	}
}

void bake_stroke(TelSource *d, const Stroke &s)
{
	ensure_vertices(s.size);
	render_stroke_into(d, d->canvas, s, 1.0f);
}

void draw_live_segment(TelSource *d, const Stroke &s, float x0, float y0, float x1, float y1)
{
	ensure_vertices(s.size);
	std::vector<Seg> one = {{x0, y0, x1, y1}};
	with_target(d, d->canvas, false, [&]() { render_segments(s, one, 1.0f); });
}

void rebuild_texture(TelSource *d)
{
	for (const auto &s : g_strokes)
		ensure_vertices(s.size);
	with_target(d, d->canvas, true, []() {}); // clear, then replay stroke by stroke
	for (const auto &s : g_strokes)
		render_stroke_into(d, d->canvas, s, 1.0f);
}

// Transient overlay: fading temp strokes + in-progress preview + armed indicator.
void render_overlay(TelSource *d)
{
	for (const auto &s : g_temp)
		ensure_vertices(s.size);
	if (g_active && !g_active->eraser)
		ensure_vertices(g_active->size);
	if (g_drawing_enabled && g_show_indicator)
		ensure_vertices(INDICATOR_SIZE);

	// age temp strokes: cull the dead, remember each survivor's current fade
	std::vector<Stroke> survivors;
	std::vector<float> fades;
	for (auto &s : g_temp) {
		float age = g_clock - s.born;
		if (age >= s.life)
			continue;
		float fade = 1.0f;
		float hold = s.life - s.fade;
		if (age > hold) {
			float t = (s.life - age) / s.fade; // 1 -> 0 over the fade window
			fade = t * t * (3.0f - 2.0f * t);  // smoothstep ease-out: clean fade
		}
		fades.push_back(fade);
		survivors.push_back(std::move(s));
	}
	g_temp = std::move(survivors);

	with_target(d, d->preview, true, []() {}); // clear, then per-stroke passes
	for (size_t i = 0; i < g_temp.size(); i++)
		render_stroke_into(d, d->preview, g_temp[i], fades[i]);

	// in-progress stroke: pen/shapes/laser preview smoothly each frame.
	if (g_active && !g_active->eraser)
		render_stroke_into(d, d->preview, *g_active, 1.0f);

	// armed indicator dot (opt-in; composites into output)
	if (g_drawing_enabled && g_show_indicator) {
		Stroke ind;
		ind.tool = TOOL_PEN;
		ind.color = g_eraser ? 0xFFCCCCCC : g_color_array[g_color_index - 1];
		ind.size = INDICATOR_SIZE;
		ind.eraser = false;
		ind.points.push_back({(float)INDICATOR_X, (float)INDICATOR_Y});
		render_stroke_into(d, d->preview, ind, 1.0f);
	}
}

// ---------------------------------------------------------------------------
// Stroke lifecycle
// ---------------------------------------------------------------------------
void begin_stroke(TelSource *d, float mx, float my)
{
	bool temp = (g_laser_mode || (g_autofade_secs > 0)) && !g_eraser;
	int t = g_eraser ? TOOL_PEN : g_tool;
	g_active = std::make_unique<Stroke>();
	g_active->tool = t;
	g_active->color = g_color_array[g_color_index - 1];
	g_active->size = g_size;
	g_active->eraser = g_eraser;
	g_active->temporary = temp;
	g_active->dashed = g_dashed && !is_freehand(t) && !g_eraser;
	g_active->filled = g_filled && (t == TOOL_RECT || t == TOOL_ELLIPSE) && !g_eraser;
	// 0.5: highlighter ink is a single uniform layer now (scratch-composited),
	// so it no longer gets the artificial boost of overlapping stamp alpha.
	g_active->opacity = (g_highlighter && !g_eraser) ? 0.5f : g_opacity;
	g_active->points.push_back({mx, my});
	g_active->x0 = mx;
	g_active->y0 = my;
	g_active->x1 = mx;
	g_active->y1 = my;
	// Eraser paints destructively onto the canvas, so it draws live. The pen
	// previews each frame (smooth), so it doesn't.
	if (!temp && g_eraser)
		draw_live_segment(d, *g_active, mx, my, mx, my);
}

void commit_stroke(TelSource *d)
{
	if (!g_active)
		return;
	Stroke s = std::move(*g_active);
	g_active.reset();

	if (s.temporary) {
		s.born = g_clock;
		if (g_laser_mode) {
			s.life = LASER_LIFE;
			s.fade = LASER_FADE;
		} else {
			s.life = (float)g_autofade_secs;
			s.fade = fminf(0.6f, (float)g_autofade_secs / 2.0f);
		}
		g_temp.push_back(std::move(s));
	} else {
		bool was_eraser = s.eraser;
		g_strokes.push_back(std::move(s));
		g_redo.clear();
		// Eraser already painted live; pen/shapes were only previewed -> bake.
		if (!was_eraser)
			bake_stroke(d, g_strokes.back());
	}
}

void do_clear(TelSource *d)
{
	g_strokes.clear();
	g_redo.clear();
	g_temp.clear();
	g_active.reset();
	with_target(d, d->canvas, true, []() {});
	with_target(d, d->preview, true, []() {});
}

void do_undo_now(TelSource *d)
{
	if (g_strokes.empty())
		return;
	g_redo.push_back(std::move(g_strokes.back()));
	g_strokes.pop_back();
	rebuild_texture(d);
}

void do_redo_now(TelSource *d)
{
	if (g_redo.empty())
		return;
	g_strokes.push_back(std::move(g_redo.back()));
	g_redo.pop_back();
	bake_stroke(d, g_strokes.back());
}

// ---------------------------------------------------------------------------
// Projector close (Win32 WM_CLOSE) + window matching
// ---------------------------------------------------------------------------
bool window_match(const char *window_name)
{
	if (!window_name)
		return false;
	if (!g_projector_name.empty() && strstr(window_name, g_projector_name.c_str()))
		return true;
	if (!g_scene_name.empty() && strstr(window_name, g_scene_name.c_str()))
		return true;
	return false;
}

void close_projector_windows(const std::string &match_in)
{
#ifdef _WIN32
	std::string match = match_in.empty() ? g_projector_name : match_in;
	if (match.empty())
		return;
	HWND prev = nullptr;
	int closed = 0;
	for (int i = 0; i < 4000; i++) {
		HWND hwnd = FindWindowExA(nullptr, prev, nullptr, nullptr);
		if (!hwnd)
			break;
		char title[512];
		int len = GetWindowTextA(hwnd, title, sizeof(title));
		if (len > 0 && strstr(title, match.c_str())) {
			PostMessageA(hwnd, WM_CLOSE, 0, 0);
			closed++;
		}
		prev = hwnd;
	}
	obs_log(LOG_INFO, "closed %d projector window(s) matching '%s'", closed, match.c_str());
#else
	(void)match_in;
#endif
}

bool valid_position(float cx, float cy, float px, float py, float w, float h)
{
	if ((cx >= 0 && cx < w && cy >= 0 && cy < h) || (px >= 0 && px < w && py >= 0 && py < h))
		return true;
	return false;
}

// ---------------------------------------------------------------------------
// Per-frame command processing
// ---------------------------------------------------------------------------
void process_commands(TelSource *d)
{
	if (f_toggle) {
		g_drawing_enabled = !g_drawing_enabled;
		f_toggle = false;
	}
	if (f_arm_on) {
		g_drawing_enabled = true;
		f_arm_on = false;
	}
	if (f_arm_off) {
		g_drawing_enabled = false;
		f_arm_off = false;
	}
	if (f_close_proj) {
		close_projector_windows(g_pending_close_match);
		g_pending_close_match.clear();
		f_close_proj = false;
	}
	if (f_clear) {
		do_clear(d);
		f_clear = false;
	}
	if (f_colorswap) {
		g_color_index++;
		if (g_color_index > COLOR_COUNT)
			g_color_index = 1;
		f_colorswap = false;
	}
	if (f_sizetoggle) {
		if (g_size % 2 == 1)
			g_size += 1;
		else
			g_size += 2;
		if (g_size > BRUSH_MAX)
			g_size = 2;
		f_sizetoggle = false;
	}
	if (f_toolcycle) {
		g_tool++;
		if (g_tool > TOOL_COUNT)
			g_tool = 1;
		f_toolcycle = false;
	}
	if (f_laser) {
		g_laser_mode = !g_laser_mode;
		f_laser = false;
	}
	if (f_sizedown) {
		if (g_size % 2 == 1)
			g_size -= 1;
		else
			g_size -= 2;
		if (g_size < 2)
			g_size = BRUSH_MAX;
		f_sizedown = false;
	}
	if (f_undo) {
		do_undo_now(d);
		f_undo = false;
	}
	if (f_redo) {
		do_redo_now(d);
		f_redo = false;
	}
}

// ---------------------------------------------------------------------------
// Input core: feed one canvas-space mouse sample into the stroke path. Shared by
// the Win32 projector poll (handle_input) and the drawable dock (Qt events), so
// both surfaces drive the exact same begin/continue/commit logic.
// ---------------------------------------------------------------------------
void feed_draw(TelSource *d, float mx, float my, bool down)
{
	if (!down) {
		if (g_active)
			commit_stroke(d);
		d->has_mouse = false;
		return;
	}
	float px = d->has_mouse ? d->mouse_x : mx;
	float py = d->has_mouse ? d->mouse_y : my;
	if (!valid_position(mx, my, px, py, (float)d->width, (float)d->height)) {
		d->has_mouse = true;
		d->mouse_x = mx;
		d->mouse_y = my;
		return;
	}

	if (!g_active) {
		begin_stroke(d, mx, my);
	} else {
		Stroke &s = *g_active;
		if (is_freehand(s.tool)) {
			const Vec2 &last = s.points.back();
			if (s.eraser) {
				draw_live_segment(d, s, last.x, last.y, mx, my);
				s.points.push_back({mx, my});
			} else {
				// decimate jittery near-duplicate points -> cleaner curves
				float ddx = mx - last.x, ddy = my - last.y;
				if (ddx * ddx + ddy * ddy >= 6.0f)
					s.points.push_back({mx, my});
			}
		} else {
			// curved arrow: also record the swept path (coarse) — the curve's
			// bow direction/depth follows it (see stroke_geometry)
			if (s.tool == TOOL_CURVEDARROW) {
				const Vec2 &last = s.points.back();
				float ddx = mx - last.x, ddy = my - last.y;
				if (ddx * ddx + ddy * ddy >= 36.0f)
					s.points.push_back({mx, my});
			}
			s.x1 = mx;
			s.y1 = my;
		}
	}

	d->has_mouse = true;
	d->mouse_x = mx;
	d->mouse_y = my;
}

// OBS's MAIN PREVIEW insets the canvas by a fixed border before letterboxing it
// into the widget (PREVIEW_EDGE_SIZE, in *physical* px — GetPixelSize multiplies
// the widget size by devicePixelRatioF, then subtracts the raw edge). Windowed
// projectors fill edge to edge (edge 0). Match OBS exactly so ink lands precisely
// under the cursor at the corners, not drifted inward by the border.
static constexpr float PREVIEW_EDGE_SIZE = 10.0f; // == OBS UI PREVIEW_EDGE_SIZE

// Map a client-area-local point (area pixel w/h, minus any letterbox edge inset)
// to canvas coords and feed it. This mirrors OBS's own GetScaleAndCenterPos +
// preview edge inset EXACTLY — same aspect branch, same integer truncation of the
// fitted size and the center offset — so feed_client is the precise inverse of the
// projector / main-preview renderer and ink lands pixel-exact under the cursor (no
// sub-pixel drift from float-vs-int rounding). Pass everything in one consistent
// space: the Win32 paths use physical px on both ends.
void feed_client(TelSource *d, float local_x, float local_y, float cw, float ch, float edge = 0.0f)
{
	int e = (int)edge;
	int winCX = (int)cw - 2 * e;
	int winCY = (int)ch - 2 * e;
	int W = (int)d->width, H = (int)d->height;
	if (winCX <= 0 || winCY <= 0 || W <= 0 || H <= 0)
		return;
	double winAspect = (double)winCX / (double)winCY;
	double baseAspect = (double)W / (double)H;
	float scale;
	int newCX, newCY;
	if (winAspect > baseAspect) { // window relatively wider -> height-bound (pillarbox L/R)
		scale = (float)winCY / (float)H;
		newCX = (int)((double)W * (double)scale);
		newCY = winCY;
	} else { // window relatively taller -> width-bound (letterbox T/B)
		scale = (float)winCX / (float)W;
		newCX = winCX;
		newCY = (int)((double)H * (double)scale);
	}
	if (scale <= 0.0f)
		return;
	float ox = (float)(winCX / 2 - newCX / 2 + e);
	float oy = (float)(winCY / 2 - newCY / 2 + e);
	feed_draw(d, (local_x - ox) / scale, (local_y - oy) / scale, true);
}

// ---------------------------------------------------------------------------
// LEGACY Win32 cursor input (opt-in via Settings; the dock is canonical) — draws
// on the windowed projector (window-title match) OR the OBS main preview region
// ("preview mode"). Global cursor poll, same as the Lua engine. Only reached from
// video_tick when g_legacy_cursor_input is on.
// ---------------------------------------------------------------------------
void handle_input(TelSource *d)
{
#ifdef _WIN32
	bool mouse_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
	if (!mouse_down) {
		feed_draw(d, 0.0f, 0.0f, false);
		return;
	}

	POINT raw;
	if (!GetCursorPos(&raw))
		return;
	HWND window = GetForegroundWindow();
	char window_name[512];
	int wlen = window ? GetWindowTextA(window, window_name, sizeof(window_name)) : 0;
	if (wlen <= 0)
		window_name[0] = '\0';

	if (window_match(window_name)) {
		// Windowed projector: map within its client area.
		POINT origin = {0, 0};
		ClientToScreen(window, &origin);
		RECT rc;
		GetClientRect(window, &rc);
		feed_client(d, (float)(raw.x - origin.x), (float)(raw.y - origin.y),
			    (float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
		return;
	}

	// Preview mode: the OBS main window is foreground and the cursor is over the
	// (locked) main-preview rect. Lock Preview keeps clicks from dragging items.
	if (g_prev_valid.load() && strncmp(window_name, "OBS ", 4) == 0) {
		float pl = g_prev_l.load(), pt = g_prev_t.load();
		float pw = g_prev_w.load(), ph = g_prev_h.load();
		if ((float)raw.x >= pl && (float)raw.x < pl + pw && (float)raw.y >= pt &&
		    (float)raw.y < pt + ph) {
			feed_client(d, (float)raw.x - pl, (float)raw.y - pt, pw, ph, PREVIEW_EDGE_SIZE);
		}
	}
	// else: not over a drawing surface — ignore (keep any active stroke pending)
#else
	(void)d;
#endif
}

// ---------------------------------------------------------------------------
// Texture management
// ---------------------------------------------------------------------------
void make_textures(TelSource *d)
{
	obs_enter_graphics();
	if (d->canvas)
		gs_texture_destroy(d->canvas);
	if (d->preview)
		gs_texture_destroy(d->preview);
	if (d->scratch)
		gs_texture_destroy(d->scratch);
	// SS x supersampled render targets (see SS); drawn back at canvas size.
	d->canvas = gs_texture_create(d->width * SS, d->height * SS, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	d->preview = gs_texture_create(d->width * SS, d->height * SS, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	d->scratch = gs_texture_create(d->width * SS, d->height * SS, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	obs_leave_graphics();
}

// ---------------------------------------------------------------------------
// Source callbacks
// ---------------------------------------------------------------------------
const char *tel_get_name(void *)
{
	return "Telestrator";
}

void *tel_create(obs_data_t *, obs_source_t *source)
{
	auto *d = static_cast<TelSource *>(bzalloc(sizeof(TelSource)));
	new (d) TelSource();
	d->source = source;
	vec4_from_rgba(&g_eraser_v4, 0x00000000);

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		d->width = ovi.base_width;
		d->height = ovi.base_height;
	}
	make_textures(d);
	obs_log(LOG_INFO, "telestrator source created (%ux%u)", d->width, d->height);
	return d;
}

void tel_destroy(void *data)
{
	auto *d = static_cast<TelSource *>(data);
	obs_enter_graphics();
	if (d->canvas)
		gs_texture_destroy(d->canvas);
	if (d->preview)
		gs_texture_destroy(d->preview);
	if (d->scratch)
		gs_texture_destroy(d->scratch);
	obs_leave_graphics();
	d->~TelSource();
	bfree(d);
}

uint32_t tel_get_width(void *data)
{
	auto *d = static_cast<TelSource *>(data);
	return d ? d->width : 1920;
}

uint32_t tel_get_height(void *data)
{
	auto *d = static_cast<TelSource *>(data);
	return d ? d->height : 1080;
}

void tel_video_render(void *data, gs_effect_t *)
{
	auto *d = static_cast<TelSource *>(data);
	if (!d->canvas)
		return;
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_blend_state_push();
	gs_reset_blend_state();
	// Textures are SS x supersampled; drawing them at canvas size downsamples
	// through the linear sampler (2x2 box filter) = the anti-aliasing pass.
	while (gs_effect_loop(def, "Draw"))
		obs_source_draw(d->canvas, 0, 0, d->width, d->height, false);
	if (d->preview) {
		while (gs_effect_loop(def, "Draw"))
			obs_source_draw(d->preview, 0, 0, d->width, d->height, false);
	}
	gs_blend_state_pop();
}

void tel_video_tick(void *data, float seconds)
{
	auto *d = static_cast<TelSource *>(data);
	g_clock += seconds;

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi) && (ovi.base_width != d->width || ovi.base_height != d->height)) {
		d->width = ovi.base_width;
		d->height = ovi.base_height;
		make_textures(d);
	}
	if (!d->canvas)
		return;

	process_commands(d);

	if (!(d->active || g_drawing_enabled))
		return;

	if (g_drawing_enabled) {
		// Scripted stroke playback (vendor "sim_stroke"): advance along the
		// point list and drive the dock input vars; release on completion so the
		// normal commit path below finishes the stroke.
		if (g_sim_t >= 0.0f) {
			g_sim_t += seconds;
			const auto &pts = g_sim_active.pts;
			float f = fminf(1.0f, g_sim_t / g_sim_active.duration);
			float fi = f * (float)(pts.size() - 1);
			int i0 = (int)fi;
			int i1 = (i0 + 1 < (int)pts.size()) ? i0 + 1 : i0;
			float u = fi - (float)i0;
			g_dock_mx.store(pts[i0].x + (pts[i1].x - pts[i0].x) * u);
			g_dock_my.store(pts[i0].y + (pts[i1].y - pts[i0].y) * u);
			if (f >= 1.0f) {
				g_dock_down.store(false);
				g_sim_t = -1.0f;
			}
		} else if (!g_dock_down.load() && !g_active) {
			std::lock_guard<std::mutex> lk(g_sim_mtx);
			if (!g_sim_queue.empty()) {
				g_sim_active = std::move(g_sim_queue.front());
				g_sim_queue.erase(g_sim_queue.begin());
				if (!g_sim_active.pts.empty()) {
					g_sim_t = 0.0f;
					g_dock_mx.store(g_sim_active.pts[0].x);
					g_dock_my.store(g_sim_active.pts[0].y);
					g_dock_down.store(true);
				}
			}
		}

		if (g_dock_down.load()) {
			// Canonical input: a drag in the Telestrator Draw dock. Draw from its
			// native Qt coords (written on the UI thread).
			feed_draw(d, g_dock_mx.load(), g_dock_my.load(), true);
		} else if (g_legacy_cursor_input.load()) {
			// Opt-in legacy: windowed-projector / main-preview Win32 cursor poll
			// (handle_input commits on its own physical mouse-up).
			handle_input(d);
		} else {
			// Dock idle: a dock stroke just released (or nothing active) — commit
			// any finished stroke. This is the mouse-up the dock relies on, so the
			// canonical path no longer needs the Win32 button poll to end a stroke.
			feed_draw(d, 0.0f, 0.0f, false);
		}
	} else {
		g_active.reset();
		d->has_mouse = false;
	}

	render_overlay(d);
}

void tel_activate(void *data)
{
	auto *d = static_cast<TelSource *>(data);
	// NB: the scene-name match fallback (window_match) needs obs-frontend-api to
	// read the current scene; we link libobs core only for now, so we rely on the
	// "Projector" keyword match instead. Revisit when the native dock lands.
	d->active = true;
}

void tel_deactivate(void *data)
{
	auto *d = static_cast<TelSource *>(data);
	d->active = false;
}

obs_source_info telestrator_source_info = {};

// ---------------------------------------------------------------------------
// Hotkeys — one dispatcher, command code passed as the hotkey's data pointer.
// ---------------------------------------------------------------------------
enum Cmd {
	C_CLEAR, C_COLORSWAP, C_SIZETOGGLE, C_ERASERTOGGLE, C_TOGGLE, C_TOOLCYCLE,
	C_UNDO, C_REDO, C_LASER, C_SIZEDOWN, C_ARMON, C_ARMOFF,
	C_CLOSE_ALL, C_CLOSE_PROGRAM, C_CLOSE_PREVIEW, C_CLOSE_MULTIVIEW, C_OPEN_PROJECTOR,
	C_TOOL_PEN, C_TOOL_LINE, C_TOOL_ARROW, C_TOOL_RECT, C_TOOL_ELLIPSE,
	C_COLOR_YELLOW, C_COLOR_RED, C_COLOR_GREEN, C_COLOR_BLUE, C_COLOR_WHITE, C_COLOR_CUSTOM,
	C_REPLAY, C_REPLAYHIDE,
	C_SIZE_THIN, C_SIZE_MED, C_SIZE_THICK,
	C_TOOL_DBLARROW, C_DASH,
	C_TOOL_CURVEDARROW, C_COLOR_ORANGE, C_COLOR_CYAN,
	C_FILL, C_HIGHLIGHT, C_OPACITY, C_INDICATOR, C_AUTOFADE,
	C_TOOL_SPOTLIGHT, C_TOOL_FIRSTDOWN, C_TOOL_CONE, C_TOOL_VERTICAL,
	C_REPLAY_RESTART, C_BUFFER_TOGGLE,
};

struct HotkeySpec {
	const char *name;
	const char *label;
	int cmd;
};

const HotkeySpec g_hotkeys[] = {
	{"telestrator.clear", "Telestrator: Clear", C_CLEAR},
	{"telestrator.colorswap", "Telestrator: Cycle Color", C_COLORSWAP},
	{"telestrator.sizetoggle", "Telestrator: Cycle Brush Size", C_SIZETOGGLE},
	{"telestrator.erasertoggle", "Telestrator: Toggle Eraser", C_ERASERTOGGLE},
	{"telestrator.toggle", "Telestrator: Toggle Drawing On/Off", C_TOGGLE},
	{"telestrator.toolcycle", "Telestrator: Cycle Tool", C_TOOLCYCLE},
	{"telestrator.undo", "Telestrator: Undo", C_UNDO},
	{"telestrator.redo", "Telestrator: Redo", C_REDO},
	{"telestrator.laser", "Telestrator: Toggle Laser (temporary ink)", C_LASER},
	{"telestrator.sizedown", "Telestrator: Brush Size Down", C_SIZEDOWN},
	{"telestrator.armon", "Telestrator: Arm (drawing ON)", C_ARMON},
	{"telestrator.armoff", "Telestrator: Disarm (drawing OFF)", C_ARMOFF},
	{"telestrator.closeprojectors", "Telestrator: Close ALL projector windows", C_CLOSE_ALL},
	{"telestrator.closeprojector.program", "Telestrator: Close Program projector", C_CLOSE_PROGRAM},
	{"telestrator.closeprojector.preview", "Telestrator: Close Preview projector", C_CLOSE_PREVIEW},
	{"telestrator.closeprojector.multiview", "Telestrator: Close Multiview projector", C_CLOSE_MULTIVIEW},
	{"telestrator.openprojector", "Telestrator: Open drawing surface (windowed Program projector)", C_OPEN_PROJECTOR},
	{"telestrator.tool.pen", "Telestrator: Tool -> Pen", C_TOOL_PEN},
	{"telestrator.tool.line", "Telestrator: Tool -> Line", C_TOOL_LINE},
	{"telestrator.tool.arrow", "Telestrator: Tool -> Arrow", C_TOOL_ARROW},
	{"telestrator.tool.rect", "Telestrator: Tool -> Rectangle", C_TOOL_RECT},
	{"telestrator.tool.ellipse", "Telestrator: Tool -> Ellipse", C_TOOL_ELLIPSE},
	{"telestrator.color.yellow", "Telestrator: Color -> Yellow", C_COLOR_YELLOW},
	{"telestrator.color.red", "Telestrator: Color -> Red", C_COLOR_RED},
	{"telestrator.color.green", "Telestrator: Color -> Green", C_COLOR_GREEN},
	{"telestrator.color.blue", "Telestrator: Color -> Blue", C_COLOR_BLUE},
	{"telestrator.color.white", "Telestrator: Color -> White", C_COLOR_WHITE},
	{"telestrator.color.custom", "Telestrator: Color -> Custom", C_COLOR_CUSTOM},
	{"telestrator.buffer", "Telestrator: Start/Stop Replay Buffer", C_BUFFER_TOGGLE},
	{"telestrator.replay", "Telestrator: Replay & Markup (save buffer, draw on it)", C_REPLAY},
	{"telestrator.replayhide", "Telestrator: Replay - Back to Live", C_REPLAYHIDE},
	{"telestrator.size.thin", "Telestrator: Size -> Thin", C_SIZE_THIN},
	{"telestrator.size.med", "Telestrator: Size -> Medium", C_SIZE_MED},
	{"telestrator.size.thick", "Telestrator: Size -> Thick", C_SIZE_THICK},
	{"telestrator.tool.dblarrow", "Telestrator: Tool -> Double Arrow", C_TOOL_DBLARROW},
	{"telestrator.dash", "Telestrator: Toggle Dashed Style", C_DASH},
	{"telestrator.tool.curvedarrow", "Telestrator: Tool -> Curved Arrow", C_TOOL_CURVEDARROW},
	{"telestrator.color.orange", "Telestrator: Color -> Orange", C_COLOR_ORANGE},
	{"telestrator.color.cyan", "Telestrator: Color -> Cyan", C_COLOR_CYAN},
	{"telestrator.fill", "Telestrator: Toggle Filled Shapes", C_FILL},
	{"telestrator.highlight", "Telestrator: Toggle Highlighter (translucent)", C_HIGHLIGHT},
	{"telestrator.opacity", "Telestrator: Cycle stroke opacity (100/66/33%)", C_OPACITY},
	{"telestrator.indicator", "Telestrator: Toggle armed indicator dot", C_INDICATOR},
	{"telestrator.autofade", "Telestrator: Cycle auto-fade ink (off/5s/10s)", C_AUTOFADE},
	{"telestrator.tool.spotlight", "Telestrator: Tool -> Spotlight (dim except ellipse)", C_TOOL_SPOTLIGHT},
	{"telestrator.tool.firstdown", "Telestrator: Tool -> Horizontal line (full-width)", C_TOOL_FIRSTDOWN},
	{"telestrator.tool.cone", "Telestrator: Tool -> Cone (vision/passing wedge)", C_TOOL_CONE},
	{"telestrator.tool.vertical", "Telestrator: Tool -> Vertical line (full-height)", C_TOOL_VERTICAL},
};

// ---------------------------------------------------------------------------
// Replay markup: save the replay buffer, then show the saved clip as an overlay
// in the current scene with the telestrator raised on top, so you can draw on
// the replay. Runs on the UI thread (frontend + scene edits): the dock buttons
// are already on the UI thread; the hotkey path marshals via obs_queue_task.
// ---------------------------------------------------------------------------
const char *REPLAY_SOURCE = "Telestrator Replay";
bool g_proj_open = false;      // whether we opened the drawing-surface projector (toggle)
bool g_replay_pending = false;

// Raise the scene's telestrator source item to the top (ink above the replay).
void raise_telestrator(obs_scene_t *scene)
{
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *) -> bool {
			obs_source_t *src = obs_sceneitem_get_source(item);
			if (src && obs_source_get_id(src) && strcmp(obs_source_get_id(src), "telestrator") == 0) {
				obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP);
				return false; // found it; stop
			}
			return true;
		},
		nullptr);
}

void replay_show_overlay(const char *file)
{
	obs_source_t *scene_src = obs_frontend_get_current_scene();
	if (!scene_src)
		return;
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (!scene) {
		obs_source_release(scene_src);
		return;
	}

	obs_source_t *replay = obs_get_source_by_name(REPLAY_SOURCE);
	if (!replay) {
		obs_data_t *s = obs_data_create();
		obs_data_set_bool(s, "is_local_file", true);
		obs_data_set_bool(s, "looping", false); // no loop — operator controls playback (play/restart/scrub)
		obs_data_set_bool(s, "restart_on_activate", true);
		obs_data_set_bool(s, "clear_on_media_end", false); // hold the last frame instead of going blank
		replay = obs_source_create("ffmpeg_source", REPLAY_SOURCE, s, nullptr);
		obs_data_release(s);
	}
	if (file && *file) {
		obs_data_t *s = obs_data_create();
		obs_data_set_bool(s, "is_local_file", true);
		obs_data_set_string(s, "local_file", file);
		obs_data_set_bool(s, "looping", false);
		obs_data_set_bool(s, "clear_on_media_end", false);
		obs_source_update(replay, s);
		obs_data_release(s);
	}

	obs_sceneitem_t *item = obs_scene_find_source(scene, REPLAY_SOURCE);
	if (!item)
		item = obs_scene_add(scene, replay);
	if (item) {
		obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP); // above live content...
		obs_sceneitem_set_visible(item, true);

		// Fill the whole canvas every time. A freshly-added media item defaults
		// to scale 1.0 at (0,0), so without this the clip lands at its native
		// pixel size in the top-left corner instead of full-screen — and the
		// telestration would no longer line up with what's on the replay.
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			struct vec2 pos, bounds;
			vec2_set(&pos, 0.0f, 0.0f);
			vec2_set(&bounds, (float)ovi.base_width, (float)ovi.base_height);
			obs_sceneitem_set_alignment(item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
			obs_sceneitem_set_pos(item, &pos);
			obs_sceneitem_set_rot(item, 0.0f);
			obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
			obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
			obs_sceneitem_set_bounds(item, &bounds);
		}
	}
	raise_telestrator(scene); // ...but keep the ink above the replay

	obs_source_set_muted(replay, true); // mute the clip's own audio (no duplicate crowd; live commentary carries)
	obs_source_media_restart(replay);   // play once from the start; operator scrubs/restarts
	obs_source_release(replay);
	obs_source_release(scene_src);
	obs_log(LOG_INFO, "telestrator: replay overlay shown (%s)", file ? file : "?");
}

void do_replay_hide()
{
	obs_source_t *scene_src = obs_frontend_get_current_scene();
	if (!scene_src)
		return;
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (scene) {
		obs_sceneitem_t *item = obs_scene_find_source(scene, REPLAY_SOURCE);
		if (item)
			obs_sceneitem_set_visible(item, false);
	}
	obs_source_release(scene_src);
	obs_log(LOG_INFO, "telestrator: replay overlay hidden (back to live)");
}

// Hide the replay overlay in EVERY scene. The replay buffer records the program
// continuously, so if a scene is saved with the replay item left visible, the
// buffer bakes that stale clip into the next capture (a replay-inside-a-replay
// at the wrong size). Called once on load so the overlay always starts hidden;
// markup makes it visible on demand and Resume Live hides it again.
void hide_replay_everywhere()
{
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_scene_t *sc = obs_scene_from_source(scenes.sources.array[i]);
		if (!sc)
			continue;
		obs_sceneitem_t *item = obs_scene_find_source(sc, REPLAY_SOURCE);
		if (item)
			obs_sceneitem_set_visible(item, false);
	}
	obs_frontend_source_list_free(&scenes);
}

void do_replay_markup()
{
	if (!obs_frontend_replay_buffer_active()) {
		obs_log(LOG_WARNING, "telestrator: replay buffer not active — start it first");
		return;
	}
	g_replay_pending = true;
	obs_frontend_replay_buffer_save(); // async; completion arrives via tel_replay_event
}

// Restart the replay clip from the top (dock "Replay Again").
void do_replay_restart()
{
	obs_source_t *s = obs_get_source_by_name(REPLAY_SOURCE);
	if (!s)
		return;
	obs_source_media_restart(s);
	obs_source_release(s);
}

// Frontend event: when our save finishes, load the clip + show the overlay.
void tel_replay_event(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED || !g_replay_pending)
		return;
	g_replay_pending = false;
	char *path = obs_frontend_get_last_replay();
	if (path && *path)
		replay_show_overlay(path);
	bfree(path);
}

void dispatch_cmd(int cmd)
{
	switch (cmd) {
	// Flag-based (processed in video_tick on the graphics thread):
	case C_CLEAR: f_clear = true; break;
	case C_COLORSWAP: f_colorswap = true; break;
	case C_SIZETOGGLE: f_sizetoggle = true; break;
	case C_TOGGLE: f_toggle = true; break;
	case C_TOOLCYCLE: f_toolcycle = true; break;
	case C_UNDO: f_undo = true; break;
	case C_REDO: f_redo = true; break;
	case C_LASER: f_laser = true; break;
	case C_SIZEDOWN: f_sizedown = true; break;
	case C_ARMON: f_arm_on = true; break;
	case C_ARMOFF: f_arm_off = true; break;
	// Real OBS 32 windowed-projector titles are the DASH form ("Projector -
	// Program/Preview/Multiview"), confirmed by live window enumeration. (The Lua
	// engine's parens form was wrong for this OBS version.)
	case C_CLOSE_ALL: g_pending_close_match.clear(); f_close_proj = true; break;
	case C_CLOSE_PROGRAM: g_pending_close_match = "Projector - Program"; f_close_proj = true; break;
	case C_CLOSE_PREVIEW: g_pending_close_match = "Projector - Preview"; f_close_proj = true; break;
	case C_CLOSE_MULTIVIEW: g_pending_close_match = "Multiview"; f_close_proj = true; break;
	case C_OPEN_PROJECTOR:
		if (g_proj_open) {
			g_pending_close_match = "Projector - Program";
			f_close_proj = true;
			g_proj_open = false;
		} else {
			obs_queue_task(
				OBS_TASK_UI,
				[](void *) { obs_frontend_open_projector("Program", -1, nullptr, nullptr); }, nullptr,
				false);
			g_proj_open = true;
		}
		break;
	// Direct (set immediately, like the Lua handlers). Picking a tool/color
	// implies intent to draw, so it clears the eraser.
	case C_ERASERTOGGLE: g_eraser = !g_eraser; break;
	case C_TOOL_PEN: g_tool = TOOL_PEN; g_eraser = false; break;
	case C_TOOL_LINE: g_tool = TOOL_LINE; g_eraser = false; break;
	case C_TOOL_ARROW: g_tool = TOOL_ARROW; g_eraser = false; break;
	case C_TOOL_RECT: g_tool = TOOL_RECT; g_eraser = false; break;
	case C_TOOL_ELLIPSE: g_tool = TOOL_ELLIPSE; g_eraser = false; break;
	case C_COLOR_YELLOW: g_color_index = 1; g_eraser = false; break;
	case C_COLOR_RED: g_color_index = 2; g_eraser = false; break;
	case C_COLOR_GREEN: g_color_index = 3; g_eraser = false; break;
	case C_COLOR_BLUE: g_color_index = 4; g_eraser = false; break;
	case C_COLOR_WHITE: g_color_index = 5; g_eraser = false; break;
	case C_COLOR_ORANGE: g_color_index = 6; g_eraser = false; break;
	case C_COLOR_CYAN: g_color_index = 7; g_eraser = false; break;
	case C_COLOR_CUSTOM: g_color_index = 8; g_eraser = false; break;
	// Replay touches frontend + scene APIs -> run on the UI thread. (Safe from
	// the dock too, which is already on the UI thread.)
	case C_REPLAY: obs_queue_task(OBS_TASK_UI, [](void *) { do_replay_markup(); }, nullptr, false); break;
	case C_REPLAYHIDE: obs_queue_task(OBS_TASK_UI, [](void *) { do_replay_hide(); }, nullptr, false); break;
	case C_REPLAY_RESTART: obs_queue_task(OBS_TASK_UI, [](void *) { do_replay_restart(); }, nullptr, false); break;
	case C_BUFFER_TOGGLE:
		obs_queue_task(
			OBS_TASK_UI,
			[](void *) {
				if (obs_frontend_replay_buffer_active())
					obs_frontend_replay_buffer_stop();
				else
					obs_frontend_replay_buffer_start();
			},
			nullptr, false);
		break;
	case C_SIZE_THIN: g_size = 2; break;
	case C_SIZE_MED: g_size = 6; break;
	case C_SIZE_THICK: g_size = 12; break;
	case C_TOOL_DBLARROW: g_tool = TOOL_DBLARROW; g_eraser = false; break;
	case C_TOOL_CURVEDARROW: g_tool = TOOL_CURVEDARROW; g_eraser = false; break;
	case C_DASH: g_dashed = !g_dashed; break;
	case C_FILL: g_filled = !g_filled; break;
	case C_HIGHLIGHT: g_highlighter = !g_highlighter; break;
	case C_OPACITY: g_opacity = (g_opacity > 0.8f) ? 0.66f : (g_opacity > 0.5f ? 0.33f : 1.0f); break;
	case C_INDICATOR: g_show_indicator = !g_show_indicator; break;
	case C_AUTOFADE: g_autofade_secs = (g_autofade_secs == 0) ? 5 : (g_autofade_secs == 5 ? 10 : 0); break;
	case C_TOOL_SPOTLIGHT: g_tool = TOOL_SPOTLIGHT; g_eraser = false; break;
	case C_TOOL_FIRSTDOWN: g_tool = TOOL_FIRSTDOWN; g_eraser = false; break;
	case C_TOOL_CONE: g_tool = TOOL_CONE; g_eraser = false; break;
	case C_TOOL_VERTICAL: g_tool = TOOL_VERTICAL; g_eraser = false; break;
	default: break;
	}
}

void tel_hotkey_cb(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	dispatch_cmd((int)(intptr_t)data);
}

// Load a dock button icon, embedded in the DLL as a Qt resource (no data dir
// install needed). See src/telestrator.qrc.
static QIcon dock_icon(const char *name)
{
	if (!name)
		return QIcon();
	return QIcon(QString(":/telestrator/") + name + ".png");
}

// Apply an arbitrary palette swatch as the ink color (custom slot + select it).
static void set_custom_color(uint32_t rgba)
{
	g_color_array[COLOR_COUNT - 1] = rgba;
	dispatch_cmd(C_COLOR_CUSTOM);
}

// obs-websocket vendor: lets the dock / Stream Deck set an arbitrary RGB ink
// color over the wire (the hotkeys only reach the 8 presets). Reuses the same
// custom-slot path as the Qt color picker above. Registered in post_load.
static obs_websocket_vendor g_vendor = nullptr;

// CallVendorRequest{ vendorName:"telestrator", requestType:"set_color",
//   requestData:{ rgb: 0xRRGGBB } }   (or { r, g, b } as separate 0-255 ints)
static void ws_set_color(obs_data_t *req, obs_data_t *resp, void *)
{
	uint32_t rgb = (uint32_t)obs_data_get_int(req, "rgb");
	if (obs_data_has_user_value(req, "r")) {
		rgb = ((uint32_t)obs_data_get_int(req, "r") << 16) | ((uint32_t)obs_data_get_int(req, "g") << 8) |
		      (uint32_t)obs_data_get_int(req, "b");
	}
	uint8_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
	set_custom_color(0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r); // engine stores ABGR
	if (resp)
		obs_data_set_bool(resp, "ok", true);
}

// CallVendorRequest{ vendorName:"telestrator", requestType:"sim_stroke",
//   requestData:{ points:[{x,y},...], duration_ms:400 } }   (canvas px coords)
// Queues a scripted stroke through the canonical dock input path — the test /
// demo / integration hook (works with drawing armed, no window focus needed).
static void ws_sim_stroke(obs_data_t *req, obs_data_t *resp, void *)
{
	SimStroke ss;
	long long ms = obs_data_get_int(req, "duration_ms");
	ss.duration = (ms > 0 ? (float)ms : 400.0f) / 1000.0f;
	obs_data_array_t *arr = obs_data_get_array(req, "points");
	size_t n = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *p = obs_data_array_item(arr, i);
		ss.pts.push_back({(float)obs_data_get_double(p, "x"), (float)obs_data_get_double(p, "y")});
		obs_data_release(p);
	}
	if (arr)
		obs_data_array_release(arr);
	bool ok = !ss.pts.empty();
	if (ok) {
		std::lock_guard<std::mutex> lk(g_sim_mtx);
		g_sim_queue.push_back(std::move(ss));
	}
	if (resp)
		obs_data_set_bool(resp, "ok", ok);
}

// ---------------------------------------------------------------------------
// Scene management: one-click add/remove of the Telestrator source on the
// CURRENT scene, so attaching the overlay is as obvious as adding any source.
// UI-thread only (dock buttons + the dock sync timer).
// ---------------------------------------------------------------------------
obs_sceneitem_t *find_telestrator_item(obs_scene_t *scene)
{
	obs_sceneitem_t *hit = nullptr;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *p) -> bool {
			obs_source_t *src = obs_sceneitem_get_source(item);
			if (src && obs_source_get_id(src) && strcmp(obs_source_get_id(src), "telestrator") == 0) {
				*static_cast<obs_sceneitem_t **>(p) = item;
				return false;
			}
			return true;
		},
		&hit);
	return hit;
}

// The one logical telestrator source: reuse it if it exists anywhere (drawing
// state is engine-wide), otherwise create it.
obs_source_t *get_or_create_telestrator()
{
	obs_source_t *found = nullptr;
	obs_enum_sources(
		[](void *p, obs_source_t *src) -> bool {
			if (obs_source_get_id(src) && strcmp(obs_source_get_id(src), "telestrator") == 0) {
				*static_cast<obs_source_t **>(p) = obs_source_get_ref(src);
				return false;
			}
			return true;
		},
		&found);
	if (!found)
		found = obs_source_create("telestrator", "Telestrator", nullptr, nullptr);
	return found;
}

// True if the current scene has a telestrator item (drives the button label).
bool telestrator_in_current_scene()
{
	obs_source_t *scene_src = obs_frontend_get_current_scene();
	if (!scene_src)
		return false;
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	bool in = scene && find_telestrator_item(scene) != nullptr;
	obs_source_release(scene_src);
	return in;
}

void toggle_telestrator_in_scene()
{
	obs_source_t *scene_src = obs_frontend_get_current_scene();
	if (!scene_src)
		return;
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (scene) {
		obs_sceneitem_t *item = find_telestrator_item(scene);
		if (item) {
			obs_sceneitem_remove(item);
			obs_log(LOG_INFO, "telestrator: removed from scene '%s'", obs_source_get_name(scene_src));
		} else {
			obs_source_t *tel = get_or_create_telestrator();
			if (tel) {
				obs_sceneitem_t *ni = obs_scene_add(scene, tel);
				if (ni) {
					obs_sceneitem_set_order(ni, OBS_ORDER_MOVE_TOP);
					obs_sceneitem_set_locked(ni, true); // overlay: don't let clicks drag it
				}
				obs_source_release(tel);
				obs_log(LOG_INFO, "telestrator: added to scene '%s'", obs_source_get_name(scene_src));
			}
		}
	}
	obs_source_release(scene_src);
}

// Advanced settings that don't belong in the always-visible Tools dock.
static void show_settings_dialog()
{
	QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	QDialog *dlg = new QDialog(parent);
	dlg->setWindowTitle("Telestrator Settings");
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	QFormLayout *form = new QFormLayout(dlg);

	QSpinBox *fade = new QSpinBox(dlg);
	fade->setRange(0, 60);
	fade->setValue(g_autofade_secs);
	fade->setSuffix(" s");
	QObject::connect(fade, QOverload<int>::of(&QSpinBox::valueChanged), [](int v) { g_autofade_secs = v; });
	form->addRow("Auto-fade ink after (0 = off):", fade);

	QCheckBox *ind = new QCheckBox(dlg);
	ind->setChecked(g_show_indicator);
	QObject::connect(ind, &QCheckBox::toggled, [](bool v) { g_show_indicator = v; });
	form->addRow("Show armed indicator dot:", ind);

	QSpinBox *op = new QSpinBox(dlg);
	op->setRange(10, 100);
	op->setValue((int)(g_opacity * 100.0f + 0.5f));
	op->setSuffix(" %");
	QObject::connect(op, QOverload<int>::of(&QSpinBox::valueChanged), [](int v) { g_opacity = (float)v / 100.0f; });
	form->addRow("Default ink opacity:", op);

	QCheckBox *legacy = new QCheckBox(dlg);
	legacy->setChecked(g_legacy_cursor_input.load());
	legacy->setToolTip("Legacy/experimental. The Telestrator Draw dock is the native way to draw. "
			   "This enables drawing on a windowed projector or the OBS main preview via global "
			   "Windows cursor polling + window-title matching (Windows-only, must keep the "
			   "projector/preview focused, and Lock Preview on for preview mode).");
	QObject::connect(legacy, &QCheckBox::toggled, [](bool v) { g_legacy_cursor_input.store(v); });
	form->addRow("Legacy projector / preview input (Win32):", legacy);

	QLabel *replayHelp = new QLabel("Replay markup needs the OBS Replay Buffer ON: "
					"Settings -> Output -> Replay Buffer, then "
					"Controls -> Start Replay Buffer.",
					dlg);
	replayHelp->setWordWrap(true);
	form->addRow("Replay:", replayHelp);

	QLabel *about = new QLabel("Telestrator for OBS Studio — MIT licensed.", dlg);
	about->setWordWrap(true);
	form->addRow(about);

	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
	QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::close);
	QObject::connect(bb, &QDialogButtonBox::accepted, dlg, &QDialog::close);
	form->addRow(bb);

	dlg->show();
}

// ---------------------------------------------------------------------------
// Drawable preview dock — an OBS display that renders the program output and that
// you draw on directly. Native Qt mouse events -> canvas coords -> the shared
// stroke path (feed_draw). Lives inside the OBS main window: one-window workflow,
// no projector, no cursor-polling. The Win32 projector path stays as a fallback.
// ---------------------------------------------------------------------------
class DrawSurface : public QWidget {
	obs_display_t *m_display = nullptr;

public:
	explicit DrawSurface(QWidget *parent = nullptr) : QWidget(parent)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_NativeWindow);
		// 16:9-shaped floor: the pad is a canvas viewport, so never let the
		// layout squeeze it into a skinny letterboxed sliver.
		setMinimumSize(480, 270);
		setCursor(Qt::CrossCursor);
	}
	~DrawSurface() override { destroyDisplay(); }
	QPaintEngine *paintEngine() const override { return nullptr; }

	// A re-dock / float / close+reopen swaps this widget's native HWND
	// (WinIdChange). The obs_display is bound to the OLD hwnd, so after a
	// remove+add the surface renders nowhere ("it breaks"). Rebind it.
	bool event(QEvent *e) override
	{
		if (e->type() == QEvent::WinIdChange && isVisible()) {
			destroyDisplay();
			createDisplay();
		}
		return QWidget::event(e);
	}

protected:
	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		createDisplay();
	}
	void hideEvent(QHideEvent *e) override
	{
		QWidget::hideEvent(e);
		// Closing/undocking can recreate the native HWND; drop the display so the
		// next show rebinds it to the live window instead of the dead one.
		destroyDisplay();
	}
	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		if (!m_display)
			createDisplay();
		else
			obs_display_resize(m_display, physW(), physH());
	}
	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton) {
			feed(e);
			g_dock_down.store(true);
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override
	{
		if (e->buttons() & Qt::LeftButton)
			feed(e);
	}
	void mouseReleaseEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton)
			g_dock_down.store(false); // next tick commits the stroke (feed_draw down=false)
	}
	void contextMenuEvent(QContextMenuEvent *e) override
	{
		QMenu menu(this);
		QAction *fit = menu.addAction("Fit to Window");
		QAction *fill = menu.addAction("Fill Window (crop)");
		fit->setCheckable(true);
		fill->setCheckable(true);
		fit->setChecked(!g_drawpad_fill.load());
		fill->setChecked(g_drawpad_fill.load());
		QObject::connect(fit, &QAction::triggered, []() { g_drawpad_fill.store(false); });
		QObject::connect(fill, &QAction::triggered, []() { g_drawpad_fill.store(true); });
		menu.exec(e->globalPos());
	}

private:
	uint32_t physW() const { return (uint32_t)((float)width() * (float)devicePixelRatioF()); }
	uint32_t physH() const { return (uint32_t)((float)height() * (float)devicePixelRatioF()); }

	void destroyDisplay()
	{
		if (m_display) {
			obs_display_remove_draw_callback(m_display, &DrawSurface::renderCb, this);
			obs_display_destroy(m_display);
			m_display = nullptr;
		}
	}
	void createDisplay()
	{
		if (m_display || !isVisible() || width() < 2 || height() < 2)
			return;
		gs_init_data info = {};
		info.cx = physW();
		info.cy = physH();
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
#ifdef _WIN32
		info.window.hwnd = (HWND)winId();
#endif
		m_display = obs_display_create(&info, 0);
		if (m_display)
			obs_display_add_draw_callback(m_display, &DrawSurface::renderCb, this);
	}

	// Qt mouse position -> canvas coords. Computed as the EXACT inverse of renderCb
	// below: same physical pixel size (physW/physH), same fminf scale, same integer
	// letterbox offsets. The mouse is converted logical->physical (x devicePixelRatio)
	// so it lands in the same space the video is drawn in — pixel-perfect, HiDPI and
	// letterbox included, with no dependency on OBS UI internals.
	void feed(QMouseEvent *e)
	{
		struct obs_video_info ovi;
		uint32_t bw = 1920, bh = 1080;
		if (obs_get_video_info(&ovi)) {
			bw = ovi.base_width;
			bh = ovi.base_height;
		}
		uint32_t cx = physW(), cy = physH(); // the pixel size renderCb letterboxes into
		if (cx < 1 || cy < 1 || bw < 1 || bh < 1)
			return;
		float scale = g_drawpad_fill.load() ? fmaxf((float)cx / (float)bw, (float)cy / (float)bh)
						    : fminf((float)cx / (float)bw, (float)cy / (float)bh);
		if (scale <= 0.0f)
			return;
		int dw = (int)((float)bw * scale), dh = (int)((float)bh * scale);
		int ox = ((int)cx - dw) / 2, oy = ((int)cy - dh) / 2; // identical to renderCb
		float dpr = (float)devicePixelRatioF();
		float mx = (float)e->position().x() * dpr, my = (float)e->position().y() * dpr;
		g_dock_mx.store((mx - (float)ox) / scale);
		g_dock_my.store((my - (float)oy) / scale);
	}

	static void renderCb(void *, uint32_t cx, uint32_t cy)
	{
		struct obs_video_info ovi;
		uint32_t bw = 1920, bh = 1080;
		if (obs_get_video_info(&ovi)) {
			bw = ovi.base_width;
			bh = ovi.base_height;
		}
		float scale = g_drawpad_fill.load() ? fmaxf((float)cx / (float)bw, (float)cy / (float)bh)
						    : fminf((float)cx / (float)bw, (float)cy / (float)bh);
		uint32_t dw = (uint32_t)((float)bw * scale);
		uint32_t dh = (uint32_t)((float)bh * scale);
		int ox = ((int)cx - (int)dw) / 2;
		int oy = ((int)cy - (int)dh) / 2;

		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		gs_set_viewport(0, 0, (int)cx, (int)cy);
		struct vec4 bg;
		vec4_set(&bg, 0.05f, 0.05f, 0.06f, 1.0f);
		gs_clear(GS_CLEAR_COLOR, &bg, 0.0f, 0);

		gs_ortho(0.0f, (float)bw, 0.0f, (float)bh, -100.0f, 100.0f);
		gs_set_viewport(ox, oy, (int)dw, (int)dh);
		obs_render_main_texture();

		gs_projection_pop();
		gs_viewport_pop();
	}
};

void add_dock()
{
	// THREE stacked control docks — Tools / Color / Replay — so the right-hand
	// column reads balanced, like a native OBS side panel (each dock stays
	// compact and resizable on its own). Every widget is a plain Qt control so
	// the OBS theme styles it; tool buttons reuse the Stream Deck key glyphs
	// (src/telestrator.qrc); a poll timer reflects engine state back into the
	// docks so hotkey / Stream Deck / websocket changes always show.
	QWidget *tools = new QWidget();
	QVBoxLayout *tv = new QVBoxLayout(tools);
	tv->setContentsMargins(8, 8, 8, 8);
	tv->setSpacing(10);
	QWidget *colors = new QWidget();
	QVBoxLayout *cv = new QVBoxLayout(colors);
	cv->setContentsMargins(8, 8, 8, 8);
	cv->setSpacing(10);
	// Replay/control dock mirrors OBS's own Controls dock metrics exactly
	// (button height, spacing, margins), so the two read as siblings.
	QWidget *replay = new QWidget();
	QVBoxLayout *rv = new QVBoxLayout(replay);
	rv->setContentsMargins(6, 6, 6, 6);
	rv->setSpacing(0); // exact match to OBS's Controls dock (~4 px visual gap from the button style)

	struct ToolRef {
		QToolButton *btn;
		int tool; // engine Tool id; -1 == eraser
	};
	auto *toolRefs = new std::vector<ToolRef>();
	QButtonGroup *toolGroup = new QButtonGroup(tools);
	toolGroup->setExclusive(true);

	// Icon cell (Sources-toolbar feel): flat/autoRaise QToolButton, themed by OBS.
	auto iconBtn = [&](const char *icon, const char *tip, int cmd) {
		QToolButton *b = new QToolButton();
		b->setAutoRaise(true);
		b->setCheckable(true);
		b->setIcon(dock_icon(icon));
		b->setIconSize(QSize(20, 20));
		b->setMinimumSize(30, 30);
		b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		b->setToolTip(tip);
		b->setFocusPolicy(Qt::NoFocus);
		QObject::connect(b, &QToolButton::clicked, [cmd]() { dispatch_cmd(cmd); });
		return b;
	};
	auto toolBtn = [&](const char *icon, const char *tip, int cmd, int tool) {
		QToolButton *b = iconBtn(icon, tip, cmd);
		toolGroup->addButton(b);
		toolRefs->push_back({b, tool});
		return b;
	};
	// Full-width native control button — identical to OBS's Controls-dock
	// buttons (28 px min height, expanding width, theme-styled).
	auto ctrlBtn = [](const char *text, int cmd) {
		QPushButton *b = new QPushButton(text);
		b->setMinimumHeight(28);
		b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		b->setFocusPolicy(Qt::NoFocus);
		if (cmd >= 0)
			QObject::connect(b, &QPushButton::clicked, [cmd]() { dispatch_cmd(cmd); });
		return b;
	};

	// ---- Replay/control dock, ordered the way you actually click it:
	//      Add to scene -> Arm -> Markup Replay -> Replay Again -> Resume Live.

	// 1. Put the overlay on (or take it off) the current scene. Label flips from
	//    the sync timer below.
	QPushButton *sceneBtn = ctrlBtn("Add to Current Scene", -1);
	sceneBtn->setToolTip("Add or remove the Telestrator overlay on the current scene");
	QObject::connect(sceneBtn, &QPushButton::clicked, []() { toggle_telestrator_in_scene(); });
	rv->addWidget(sceneBtn);

	// 2. Arm drawing: the Start/Stop slot; the label flips like OBS's buttons.
	QPushButton *armBtn = ctrlBtn("Arm Drawing", C_TOGGLE);
	armBtn->setToolTip("Toggle drawing on/off (telestrator.toggle)");
	rv->addWidget(armBtn);

	// ---- Tool palette: 4-wide grid, ordered by reach frequency; eraser lives
	//      in the grid as a peer mode. Exclusive + state-synced (radio look). ----
	QGridLayout *tg = new QGridLayout();
	tg->setContentsMargins(0, 0, 0, 0);
	tg->setSpacing(2);
	struct ToolDef {
		const char *icon, *tip;
		int cmd, tool;
	};
	static const ToolDef TOOL_DEFS[] = {
		{"pen", "Pen", C_TOOL_PEN, TOOL_PEN},
		{"line", "Line", C_TOOL_LINE, TOOL_LINE},
		{"arrow", "Arrow", C_TOOL_ARROW, TOOL_ARROW},
		{"dblarrow", "Double arrow", C_TOOL_DBLARROW, TOOL_DBLARROW},
		{"curvedarrow", "Curved arrow", C_TOOL_CURVEDARROW, TOOL_CURVEDARROW},
		{"rect", "Rectangle", C_TOOL_RECT, TOOL_RECT},
		{"ellipse", "Ellipse", C_TOOL_ELLIPSE, TOOL_ELLIPSE},
		{"cone", "Cone (vision wedge)", C_TOOL_CONE, TOOL_CONE},
		{"firstdown", "Horizontal line (full width)", C_TOOL_FIRSTDOWN, TOOL_FIRSTDOWN},
		{"vertical", "Vertical line (full height)", C_TOOL_VERTICAL, TOOL_VERTICAL},
		{"spotlight", "Spotlight", C_TOOL_SPOTLIGHT, TOOL_SPOTLIGHT},
		{"eraser", "Eraser", C_ERASERTOGGLE, -1},
	};
	const int TOOL_COLS = 4;
	int ti = 0;
	for (const ToolDef &d : TOOL_DEFS) {
		tg->addWidget(toolBtn(d.icon, d.tip, d.cmd, d.tool), ti / TOOL_COLS, ti % TOOL_COLS);
		ti++;
	}
	for (int c = 0; c < TOOL_COLS; c++)
		tg->setColumnStretch(c, 1);
	tv->addLayout(tg);

	// ---- Styles: independent toggles, same cell rhythm as the tool grid ----
	QGridLayout *stg = new QGridLayout();
	stg->setContentsMargins(0, 0, 0, 0);
	stg->setSpacing(2);
	QToolButton *dashBtn = iconBtn("dash", "Dashed style", C_DASH);
	QToolButton *fillBtn = iconBtn("fill", "Filled shapes", C_FILL);
	QToolButton *hlBtn = iconBtn("highlighter", "Highlighter (translucent ink)", C_HIGHLIGHT);
	QToolButton *laserBtn = iconBtn("laser", "Laser (fading ink)", C_LASER);
	stg->addWidget(dashBtn, 0, 0);
	stg->addWidget(fillBtn, 0, 1);
	stg->addWidget(hlBtn, 0, 2);
	stg->addWidget(laserBtn, 0, 3);
	for (int c = 0; c < TOOL_COLS; c++)
		stg->setColumnStretch(c, 1);
	tv->addLayout(stg);

	// ---- Brush size: one slider (small dot -> big dot), like the Mixer's ----
	QHBoxLayout *szRow = new QHBoxLayout();
	szRow->setContentsMargins(2, 0, 2, 0);
	szRow->setSpacing(8);
	QLabel *szSmall = new QLabel();
	szSmall->setPixmap(dock_icon("sizedown").pixmap(12, 12));
	QLabel *szBig = new QLabel();
	szBig->setPixmap(dock_icon("sizeup").pixmap(18, 18));
	QSlider *szSlider = new QSlider(Qt::Horizontal);
	szSlider->setRange(2, BRUSH_MAX);
	szSlider->setValue(g_size);
	szSlider->setToolTip("Brush size");
	szSlider->setFocusPolicy(Qt::NoFocus);
	QObject::connect(szSlider, &QSlider::valueChanged, [](int val) { g_size = val; });
	szRow->addWidget(szSmall, 0);
	szRow->addWidget(szSlider, 1);
	szRow->addWidget(szBig, 0);
	tv->addLayout(szRow);

	// ---- Draw dock: live program video you draw on directly (native Qt input) ----
	// Docked at the bottom by default ("a drawing pad below the frame"); no scroll
	// wrap so the surface fills. Drawing here needs the telestrator source in the
	// scene + Arm on, same as the projector.
	DrawSurface *drawSurface = new DrawSurface();
	// Fresh dock id so OBS places it at the bottom (a drawing pad below the frame)
	// instead of restoring an old tabbed-with-Tools layout.
	obs_frontend_add_dock_by_id("telestrator_dock_drawpad", "Telestrator Draw", drawSurface);
	if (QMainWindow *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window())) {
		if (QDockWidget *dw = mw->findChild<QDockWidget *>("telestrator_dock_drawpad")) {
			dw->setFloating(false);
			mw->addDockWidget(Qt::BottomDockWidgetArea, dw);
			dw->setVisible(true);
			dw->raise();
		}
	}

	// LEGACY "preview mode": publish the OBS main-preview screen rect to handle_input
	// on a UI-thread timer. This reaches into OBS's private widget tree, so it ONLY
	// runs when the legacy cursor-input path is opted in (default off) — otherwise the
	// timer is inert and never touches OBS internals.
	if (QMainWindow *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window())) {
		QTimer *prevTimer = new QTimer(mw);
		QObject::connect(prevTimer, &QTimer::timeout, [mw]() {
			if (!g_legacy_cursor_input.load()) {
				g_prev_valid.store(false);
				return;
			}
			QWidget *pv = mw->findChild<QWidget *>("preview");
			if (pv && pv->isVisible()) {
#ifdef _WIN32
				RECT r;
				if (GetWindowRect((HWND)pv->winId(), &r)) {
					g_prev_l.store((float)r.left);
					g_prev_t.store((float)r.top);
					g_prev_w.store((float)(r.right - r.left));
					g_prev_h.store((float)(r.bottom - r.top));
					g_prev_valid.store(true);
					return;
				}
#endif
			}
			g_prev_valid.store(false);
		});
		prevTimer->start(120);
	}

	// ---- Colors: OBS's "Select Color" basic grid 1:1 (QColorDialog standard
	//      colors, 8 cols x 6 rows column-major) + the full picker below. The
	//      active ink swatch gets a light ring, synced from engine state. ----
	auto swatchCss = [](const QColor &qc, bool selected) {
		char css[220];
		snprintf(css, sizeof(css),
			 "QPushButton{background:#%02X%02X%02X;border:%s;border-radius:2px;margin:0px;padding:0px;}"
			 "QPushButton:hover{border:1px solid #e6e6e6;}",
			 qc.red(), qc.green(), qc.blue(), selected ? "2px solid #f0f0f0" : "1px solid #0e0e0e");
		return QString(css);
	};
	auto *swatches = new std::vector<std::pair<QPushButton *, uint32_t>>();
	QWidget *swGrid = new QWidget();
	QGridLayout *sg = new QGridLayout(swGrid);
	sg->setContentsMargins(0, 0, 0, 0);
	sg->setSpacing(3);
	for (int i = 0; i < 48; i++) {
		QColor qc = QColorDialog::standardColor(i);
		QPushButton *btn = new QPushButton();
		btn->setFixedSize(20, 20);
		btn->setFocusPolicy(Qt::NoFocus);
		char tip[16];
		snprintf(tip, sizeof(tip), "#%02X%02X%02X", qc.red(), qc.green(), qc.blue());
		btn->setToolTip(tip);
		btn->setStyleSheet(swatchCss(qc, false));
		uint32_t rgba = 0xFF000000u | ((uint32_t)qc.blue() << 16) | ((uint32_t)qc.green() << 8) |
				(uint32_t)qc.red();
		QObject::connect(btn, &QPushButton::clicked, [rgba]() { set_custom_color(rgba); });
		sg->addWidget(btn, i % 6, i / 6); // 6 rows x 8 cols, column-major (matches QColorDialog)
		swatches->push_back({btn, rgba});
	}
	QHBoxLayout *swCenter = new QHBoxLayout();
	swCenter->addStretch(1);
	swCenter->addWidget(swGrid);
	swCenter->addStretch(1);
	cv->addLayout(swCenter);

	QPushButton *wheel = new QPushButton("Select Color…");
	wheel->setMinimumHeight(28);
	wheel->setFocusPolicy(Qt::NoFocus);
	wheel->setToolTip("Pick any ink color");
	QObject::connect(wheel, &QPushButton::clicked, []() {
		QColor c = QColorDialog::getColor(QColor(255, 255, 255), nullptr, "Select Color");
		if (c.isValid())
			set_custom_color(0xFF000000u | ((uint32_t)c.blue() << 16) | ((uint32_t)c.green() << 8) |
					 (uint32_t)c.red());
	});
	cv->addWidget(wheel);
	cv->addStretch(1);

	// ---- Edit (Tools dock): undo / redo side by side, clear under them ----
	QHBoxLayout *editRow = new QHBoxLayout();
	editRow->setSpacing(6);
	editRow->addWidget(ctrlBtn("Undo", C_UNDO), 1);
	editRow->addWidget(ctrlBtn("Redo", C_REDO), 1);
	tv->addLayout(editRow);
	tv->addWidget(ctrlBtn("Clear", C_CLEAR));
	tv->addStretch(1);

	// ---- Replay dock: markup flow + the Settings gear beside "Resume Live"
	//      (exactly like OBS's Virtual Camera gear) ----
	rv->addWidget(ctrlBtn("Markup Replay", C_REPLAY));
	rv->addWidget(ctrlBtn("Replay Again", C_REPLAY_RESTART));
	// Resume Live stays full-width so its label centers exactly like the other
	// buttons; the settings gear floats flat over the right end (no stolen width,
	// so nothing shifts off-center). Same grid cell => the gear stacks on top.
	QGridLayout *gearRow = new QGridLayout();
	gearRow->setContentsMargins(0, 0, 0, 0);
	gearRow->setSpacing(0);
	gearRow->addWidget(ctrlBtn("Resume Live", C_REPLAYHIDE), 0, 0);
	QToolButton *gear = new QToolButton();
	gear->setAutoRaise(true); // flat until hover, so the button shows through
	gear->setIcon(dock_icon("settings"));
	gear->setIconSize(QSize(16, 16));
	gear->setFixedSize(30, 26);
	gear->setToolTip("Telestrator settings");
	gear->setFocusPolicy(Qt::NoFocus);
	QObject::connect(gear, &QToolButton::clicked, []() { show_settings_dialog(); });
	gearRow->addWidget(gear, 0, 0, Qt::AlignRight | Qt::AlignVCenter);
	rv->addLayout(gearRow);
	rv->addStretch(1);

	// Poll timer: reflect engine state into the docks (checked tool/styles, arm
	// text, slider, active swatch) so hotkey / Stream Deck / websocket changes
	// always show. 250 ms matches OBS's own status refresh cadence.
	auto *lastSwatch = new int(-1);
	QTimer *sync = new QTimer(tools);
	QObject::connect(sync, &QTimer::timeout, [=]() {
		armBtn->setText(g_drawing_enabled ? "Disarm Drawing" : "Arm Drawing");
		sceneBtn->setText(telestrator_in_current_scene() ? "Remove from Scene" : "Add to Current Scene");
		for (const ToolRef &r : *toolRefs) {
			bool want = (r.tool == -1) ? g_eraser : (!g_eraser && g_tool == r.tool);
			if (r.btn->isChecked() != want)
				r.btn->setChecked(want);
		}
		dashBtn->setChecked(g_dashed);
		fillBtn->setChecked(g_filled);
		hlBtn->setChecked(g_highlighter);
		laserBtn->setChecked(g_laser_mode);
		if (!szSlider->isSliderDown() && szSlider->value() != g_size) {
			szSlider->blockSignals(true);
			szSlider->setValue(g_size);
			szSlider->blockSignals(false);
		}
		uint32_t cur = g_color_array[g_color_index - 1];
		int sel = -1;
		for (int i = 0; i < (int)swatches->size(); i++) {
			if ((*swatches)[i].second == cur) {
				sel = i;
				break;
			}
		}
		if (sel != *lastSwatch) {
			if (*lastSwatch >= 0)
				(*swatches)[*lastSwatch].first->setStyleSheet(
					swatchCss(QColorDialog::standardColor(*lastSwatch), false));
			if (sel >= 0)
				(*swatches)[sel].first->setStyleSheet(swatchCss(QColorDialog::standardColor(sel), true));
			*lastSwatch = sel;
		}
	});
	sync->start(250);

	// Wrap each panel in a frameless scroll area so short OBS windows never
	// clip controls, with a minimum width that keeps the palette laid out as
	// designed (no wrapped grids, no truncated labels).
	auto wrapScroll = [](QWidget *w) {
		QScrollArea *s = new QScrollArea();
		s->setWidget(w);
		s->setWidgetResizable(true);
		s->setFrameShape(QFrame::NoFrame);
		s->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		s->setMinimumWidth(290);
		return s;
	};
	obs_frontend_add_dock_by_id("telestrator_dock_tools", "Telestrator Tools", wrapScroll(tools));
	obs_frontend_add_dock_by_id("telestrator_dock_color", "Telestrator Color", wrapScroll(colors));
	obs_frontend_add_dock_by_id("telestrator_dock_replay", "Telestrator Replay", wrapScroll(replay));

	// Initial placement: stacked on the right, top -> bottom (NOT tabbed), so
	// the column reads Tools / Color / Replay. place_docks_once re-asserts this
	// after OBS finishes restoring its own layout.
	if (QMainWindow *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window())) {
		QDockWidget *prev = nullptr;
		for (const char *id : {"telestrator_dock_tools", "telestrator_dock_color", "telestrator_dock_replay"}) {
			QDockWidget *dw = mw->findChild<QDockWidget *>(id);
			if (!dw)
				continue;
			dw->setFloating(false);
			if (prev)
				mw->splitDockWidget(prev, dw, Qt::Vertical);
			else
				mw->addDockWidget(Qt::RightDockWidgetArea, dw);
			dw->setVisible(true);
			prev = dw;
		}
	}

	obs_log(LOG_INFO, "telestrator: native docks registered (Tools / Color / Replay + Draw)");
}

// One-time canonical dock placement: control panel docked on the RIGHT (a side
// column, like Scenes/Sources), draw pad along the BOTTOM. Runs after OBS
// finishes restoring its own layout (which otherwise dumps unplaced docks into
// the bottom row), and only once ever — a marker in the module config dir keeps
// us from stomping wherever the user drags the docks afterwards.
void place_docks_once()
{
	char *marker = obs_module_config_path("layout-placed");
	if (!marker)
		return;
	if (os_file_exists(marker)) {
		bfree(marker);
		return;
	}
	QMainWindow *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mw) {
		bfree(marker);
		return;
	}
	QDockWidget *toolsD = mw->findChild<QDockWidget *>("telestrator_dock_tools");
	QDockWidget *colorD = mw->findChild<QDockWidget *>("telestrator_dock_color");
	QDockWidget *replayD = mw->findChild<QDockWidget *>("telestrator_dock_replay");
	QDockWidget *draw = mw->findChild<QDockWidget *>("telestrator_dock_drawpad");
	QDockWidget *prev = nullptr;
	for (QDockWidget *dw : {toolsD, colorD, replayD}) {
		if (!dw)
			continue;
		dw->setFloating(false);
		if (prev)
			mw->splitDockWidget(prev, dw, Qt::Vertical);
		else
			mw->addDockWidget(Qt::RightDockWidgetArea, dw);
		dw->setVisible(true);
		prev = dw;
	}
	if (draw) {
		draw->setFloating(false);
		mw->addDockWidget(Qt::BottomDockWidgetArea, draw);
		draw->setVisible(true);
	}
	if (toolsD)
		mw->resizeDocks({toolsD}, {330}, Qt::Horizontal);
	// Balance the column: Tools and Color get their content heights; Replay
	// (whose buttons are top-aligned) absorbs whatever is left at the BOTTOM of
	// the column, so no blank void opens up between the docks.
	if (toolsD && colorD && replayD)
		mw->resizeDocks({toolsD, colorD, replayD}, {340, 260, 520}, Qt::Vertical);
	// Shape the draw pad ~16:9 so the program feed fills it edge to edge
	// instead of floating between letterbox bars.
	if (draw) {
		mw->resizeDocks({draw}, {380}, Qt::Vertical);
		mw->resizeDocks({draw}, {660}, Qt::Horizontal);
	}

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	FILE *f = fopen(marker, "w");
	if (f) {
		fputs("placed\n", f);
		fclose(f);
	}
	bfree(marker);
	obs_log(LOG_INFO, "telestrator: docks placed (one-time canonical layout)");
}

// Frontend event hook: assert the canonical dock layout once the frontend has
// fully loaded (a beat later, so OBS's own dock restore has settled).
void tel_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		hide_replay_everywhere(); // never let a saved-visible replay overlay pollute the buffer
		if (QMainWindow *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window()))
			QTimer::singleShot(600, mw, []() { place_docks_once(); });
	}
}

} // namespace

bool obs_module_load(void)
{
	telestrator_source_info.id = "telestrator";
	telestrator_source_info.type = OBS_SOURCE_TYPE_INPUT;
	telestrator_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	telestrator_source_info.get_name = tel_get_name;
	telestrator_source_info.create = tel_create;
	telestrator_source_info.destroy = tel_destroy;
	telestrator_source_info.get_width = tel_get_width;
	telestrator_source_info.get_height = tel_get_height;
	telestrator_source_info.video_render = tel_video_render;
	telestrator_source_info.video_tick = tel_video_tick;
	telestrator_source_info.activate = tel_activate;
	telestrator_source_info.deactivate = tel_deactivate;
	obs_register_source(&telestrator_source_info);

	for (const auto &hk : g_hotkeys)
		obs_hotkey_register_frontend(hk.name, hk.label, tel_hotkey_cb, (void *)(intptr_t)hk.cmd);

	obs_log(LOG_INFO, "telestrator plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

// Add the dock once the frontend is up (the main window exists by post-load).
extern "C" void obs_module_post_load(void)
{
	add_dock();
	obs_frontend_add_event_callback(tel_replay_event, nullptr);
	obs_frontend_add_event_callback(tel_frontend_event, nullptr);

	// obs-websocket vendor request (API mandates registering here, not in load()).
	g_vendor = obs_websocket_register_vendor("telestrator");
	if (g_vendor) {
		obs_websocket_vendor_register_request(g_vendor, "set_color", ws_set_color, nullptr);
		obs_websocket_vendor_register_request(g_vendor, "sim_stroke", ws_sim_stroke, nullptr);
	}
	obs_log(LOG_INFO, "telestrator: obs-websocket vendor 'telestrator' %s",
		g_vendor ? "registered (set_color, sim_stroke)" : "unavailable (obs-websocket not loaded)");
}

void obs_module_unload(void)
{
	// Drop the frontend callbacks so a late event can't call into this
	// now-unloaded module (dangling function pointer).
	obs_frontend_remove_event_callback(tel_replay_event, nullptr);
	obs_frontend_remove_event_callback(tel_frontend_event, nullptr);

	obs_enter_graphics();
	if (g_fade_effect) {
		gs_effect_destroy(g_fade_effect);
		g_fade_effect = nullptr;
	}
	// Release the cached per-size vertex buffers (line + dot) built lazily in
	// ensure_vertices; otherwise they leak on GPU teardown.
	for (auto &kv : g_vbuf) {
		if (kv.second.line)
			gs_vertexbuffer_destroy(kv.second.line);
		if (kv.second.dot)
			gs_vertexbuffer_destroy(kv.second.dot);
	}
	g_vbuf.clear();
	obs_leave_graphics();

	obs_log(LOG_INFO, "telestrator plugin unloaded");
}
