# Notcurses renderer

Enable the separate `teder::notcurses` target as described in [build.md](build.md#optional-renderer).
The core playback library needs no terminal. The renderer uses the supplied pinned
notcurses runtime; it never initializes/stops a context, owns a plane, or presents
with `notcurses_render`. The host keeps that runtime loaded and serializes drawing,
plane changes, renderer configuration and destruction on its notcurses thread.

## Configuration and ownership

Pass a prefixed `teder_renderer_config` to `teder_renderer_create`. Mode zero
selects SPECTRUM; floor zero selects -90 dB. Other modes are SPECTRUM and LEVELS;
an explicit floor must be finite and negative. Colors are literal 0x00RRGGBB,
including black at zero; set a visible palette explicitly. Reserved/high color
bits and invalid modes/floors return INVALID. Prefix mismatch returns ABI_MISMATCH.
Creation checks the linked runtime's numeric version, returns NULL output on
failure, and allocates one small arena for handle/configuration. Configure copies
validated settings without allocation; failure preserves the old configuration.
Destroy releases the arena; destroy(NULL) is harmless. There is no engine owner.
Keep the renderer across frames, then destroy it after the final draw.

Draw borrows the snapshot and plane until return; it retains neither. A held
snapshot remains drawable after engine destruction. Stale snapshots draw exactly
the supplied values: the host decides whether to replace them with NULL. NULL
snapshot, or SPECTRUM with a meters-only snapshot, paints the background.

## Cells and clipping

Rendering uses opaque RGB backgrounds on single-column space cells, with style
zero and the configured foreground channel. Filled cells use foreground_rgb as
their background; peak markers use peak_rgb. No glyph-width or UTF-8 assumption
is required for the generated cells. Plane drawing channels/styles are restored
even after drawing errors. Base cell, geometry, scrolling/autogrow flags and
ownership are unchanged. The cursor advances as normal for the last cell written;
its previous position is not restored.

Rectangles use signed origins and unsigned sizes. Geometry is calculated in
64-bit integers and clipped against the current plane on every call. Zero area
or an empty intersection succeeds without changes. Mapping uses the original
rectangle, so clipping crops the plot instead of rescaling it. A boundary cutting
an existing wide glyph returns UNSUPPORTED before any cell write; include the
whole glyph or clear it first. Errors reading/writing native cells return IO.
A later drawing error can leave partial updates; repeat the full draw to recover.
Invalid renderer/plane/rectangle inputs return their validation error. A freed or
foreign-runtime plane is a caller-contract violation, not a safely probeable value.

## Mapping

Both modes map floor_db to zero height and 0 dB to full height, clamping outside
that range. Heights round to the nearest whole cell; an exact half may round
either way. Bars grow up from the original rectangle bottom. There are no labels,
axes or separators.

SPECTRUM maps column x to bands starting at floor(x*B/columns), ending before
floor((x+1)*B/columns), using at least one band. It takes their maximum dB value;
wide views repeat bands, and narrow views preserve peaks by grouping them.
LEVELS divides columns between left and right channels using floor(2*x/columns).
A one-column view takes each metric's channel maximum. RMS supplies the bar and
peak supplies a one-cell marker when its quantized height is positive. Linear
amplitude converts with 20*log10(value); zero maps to the floor. Smoothing/decay
already belongs to the snapshot and never depends on repaint frequency.

## Allocation boundary

Teder allocates nothing during configure/draw and uses constant stack storage,
independent of rectangle area or band count. It writes literal ASCII through
`ncplane_putegc_yx`; notcurses' `putc` path would duplicate a string per cell.
Boundary inspection uses temporary native cell copies, released on every path;
notcurses can allocate within its plane glyph pool when copying existing complex
graphemes. This is not a promise that arbitrary host-plane contents cause zero
native allocations. No render allocation or cleanup runs on the audio callback.
