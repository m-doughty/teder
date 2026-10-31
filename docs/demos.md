# Standalone C consumers

`examples/play.c` is the basic player. `examples/visualizer.c` adds a terminal
spectrum and stereo levels. Both use public headers. The visualizer and the
bounded exercise share `examples/demo.c`: one caller-owned state record, one
outstanding request, and no application worker or heap allocation.

Build with `TEDER_BUILD_EXAMPLES=ON`. The visualizer additionally requires
`TEDER_WITH_NOTCURSES=ON` and the exact fork installation described in
[build.md](build.md#optional-renderer). Core-only builds still provide the player
and lifecycle exercise. Runtime libraries must remain loaded through cleanup.

## Live terminal

Use absolute UTF-8 file paths. Windows accepts Unicode command-line paths through
`wmain`. Run in an interactive terminal at least 76 columns by 14 rows:

```sh
# Linux: use a real ALSA/Pulse output and an interactive terminal.
build-renderer/bin/teder_visualizer /home/me/Music/first.flac /home/me/Music/second.flac
# macOS: use CoreAudio and an interactive terminal.
build-renderer/bin/teder_visualizer $HOME/Music/first.flac $HOME/Music/second.flac
```

Windows PowerShell, with the UCRT64 runtime and pinned notcurses DLL available:

```powershell
$env:PATH = 'C:\notcurses\bin;C:\msys64\ucrt64\bin;' + $env:PATH
& .\build-renderer\bin\teder_visualizer.exe 'C:\Music\first.flac' 'C:\Music\second.flac'
```

The second path is optional. Startup selects the default device, enables 64-band
analysis, probes the first file, opens it, prepares its successor, then plays.
The frame thread polls commands, status and snapshots about 33 times per second.
File/device work runs in teder workers; native drawing stays on the frame thread.

| Key | Action |
| --- | --- |
| Space | Play/pause |
| Left / Right | Seek backward/forward five seconds |
| M | Toggle mute |
| D | Select the current default output |
| N / C | Prepare the supplied second path / clear the successor |
| S | Stop and rewind |
| Q / Escape / Ctrl-C | Close and restore the terminal |

Use lowercase letter keys. Commands are accepted when startup and the previous
request finish. Seek, stop and device selection invalidate a prepared successor;
press N to prepare it again. This demonstration has two paths, not a playlist.
N always refers to the supplied second path, including after a transition.

Status displays submitted source time, actual output format, underruns and analysis
drops. ENDED means final PCM submission; leave the window open for the device tail
before quitting. Frozen meters while paused are retained immutable snapshots.

Missing artwork is reported without stopping playback. Artwork bytes are available
through the probe result; the demo does not decode or display them. Metadata refers
to the initial file. Request failures show their numeric public error code. A late
successor is reported without automatic restart. Device loss requires D followed
by Space; it never silently changes output or starts playback elsewhere.

Q starts asynchronous close while retaining the last snapshot and metadata result.
After CLOSED, the demo destroys the renderer and notcurses context, then joins and
destroys the engine outside the frame loop. It checks both independent owners
again before releasing them. Failures use the same cleanup path. A close request
or expired deadline never permits freeing an object with outstanding users.

## Bounded checks

CTest registers `examples.lifecycle` and, with the renderer, `examples.visualizer`.
The latter runs `--check` against a real headless notcurses context, with no audio
device. Both use the committed 16-frame fixtures. Lifecycle checks include exact
metadata, preparation/clear, seek/stop positions, repeated acknowledged requests,
rejected and failed opens preserving the source, and close during pending probing.
The installed consumer project builds these same sources against installed public
headers/libraries; no Check or private include path is needed.

An explicit native smoke check uses two 100-millisecond, artwork-free FLAC files.
For example, with an optional local FFmpeg installation:

```sh
ffmpeg -f lavfi -i anullsrc=r=44100:cl=stereo -t 0.1 -c:a flac /absolute/silence.flac
build-renderer/bin/teder_exercise /absolute/silence.flac /absolute/silence.flac --native
```

It requires a working default output; unavailable devices fail. It verifies the
prepared successor's exact source/track identity at submitted EOF, rejects another
preparation after EOF, and holds a snapshot through engine destruction. Polling
deadlines bound normal exercises; CTest supplies the outer process timeout for
stalled native I/O. These short files fit together in the prefilled PCM pool,
so an unpaced ALSA null sink cannot outrun refill before the boundary. Longer
streaming tracks need a paced device. Null output is not physical audio evidence.

Device-loss/cutoff message assertions exercise client policy using copied status;
they do not simulate a physical unplug or reproduce the library's cutoff race.
