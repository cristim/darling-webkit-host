# Known issues and open work: darling-webkit-host

Status as of 2026-09-27. Verified by running, not by inspection.

## Working, verified

- Backend interface + priority-ordered registry with runtime auto-selection.
  `--list-backends` reports both installed engines. `DWB_BACKEND` or
  `--backend` overrides selection; an unknown name fails loudly.
- `webkitgtk` backend renders real pages: correct colours, antialiased text,
  via `webkit_web_view_get_snapshot`.
- `chromium` backend completes a CDP handshake, navigates, and receives page
  lifecycle events (`frameStartedNavigating` -> `frameStoppedLoading`).

## Verified working: H.264 playback

The media stack is confirmed good. A bare `<video autoplay muted>` pointed at a
known H.264+AAC file reports:

```
t=2.77 dur=10.0 paused=false readyState=4 w=640x360 err=none buffered=10.0
```

`currentTime` advanced, `paused=false`, `readyState=4` (HAVE_ENOUGH_DATA),
correct 640x360 geometry, no error. So the host engine decodes and plays H.264,
and the autoplay gate is open (`media_playback_requires_user_gesture` disabled).
This is the answer to the question the whole proxy rests on, and it is positive.

## Open bugs

1. **Window resize does not take effect, so frames are far larger than
   requested.** `--size 640x480` yields a 1242x1500 view allocation (7.4MB).
   `gtk_window_resize` after `show_all` is not shrinking the web view, and
   `GDK_SCALE=1` does not help. Capture is now off the GdkWindow so it faithfully
   reports the real allocation - which means this bug is now the only thing
   standing between us and correctly-sized frames. Needs the window constrained
   before the view is realised.

2. **Captured frames do not visibly change during playback.** With the same
   H.264 page, `eval` shows `currentTime=2.77` and `readyState=4`, but
   `distinct_frames=1` over 8s. Either the video region is not being captured,
   or `gdk_pixbuf_get_from_window` returns a stale surface. Playback is proven;
   *capture fidelity during playback* is not. This must be settled before any
   frame is sent to a guest, since a static frame is indistinguishable from a
   frozen video.

3. **`chromium` receives no screencast frames.** `Page.startScreencast` is
   accepted and `Page.screencastVisibilityChanged` arrives, but zero
   `Page.screencastFrame` messages follow, so `render()` has nothing to return.
   Suspect the conflicting `--ozone-platform` flags the launcher injects
   (`wayland` from the system default plus `headless` from ours) producing a
   zero-size surface. Needs `--screenshot`-based capture or an explicit
   `Emulation.setDeviceMetricsOverride` before starting the cast.

4. **YouTube itself still does not start.** Metadata resolves (duration 213.1s,
   854x480, no error) and buffering works (31.6s then 42.3s buffered), but
   `currentTime` stays 0.00 with `paused=true` and `readyState=1`. Given that a
   bare `<video>` plays fine, this is YouTube's own gate rejecting a synthetic
   `video.play()` as an untrusted event, not a decode failure. Not yet
   circumvented, so "YouTube plays" remains unproven end to end.

5. **First eval on a media page can time out.** A page whose first eval lands
   while the media subsystem is still starting returns "javascript evaluation
   timed out" even though a later eval on the same page succeeds. Bounded
   correctly, but it makes single-shot probes unreliable on media pages; use
   `--settle` generously.

## Bugs already found and fixed

- `header[10]` in the WebSocket sender could not hold header+payload; the
  payload was written past the end of the array. This was the real cause of the
  CDP `-32700` parse errors, not the JSON.
- `"params":}` was emitted for parameterless commands, which every command
  rejects. Now `{}`.
- `devtools_http_get` read until EOF; Chromium ignores `Connection: close`, so
  it blocked forever. Now `SO_RCVTIMEO` plus read-until-`]`.
- `/json/list` needed read-until-complete (the key lands in a later segment than
  the first read), and its first entry is a `background_page` from an omarchy
  extension, not a page. Now prefers `"type": "page"`.
- `/usr/bin/chromium` is a launcher stub that rewrites argv and drops the value
  of a split `--remote-debugging-port`. The `=` form is required.
- The spawned browser inherited stdout, so any caller reading our output never
  saw EOF. Child stdio now goes to `/dev/null`.
- `pump()` used blocking `g_main_context_iteration(NULL, TRUE)`, which sits
  until an event arrives, so every deadline in the backend was unreachable. Now
  non-blocking with a short sleep. This was a real hang risk for the guest's
  navigation call, independent of bug 1.
- WebKitGTK 2.52 API: `WebKitLoadEvent` has no `FAILED` member;
  `get_snapshot` is 6-arg async; `evaluate_javascript_finish` returns
  `JSCValue*`; the setting is `media_playback_requires_user_gesture`.

## Not attempted

Backends for `webkit2gtk-5.0`, WPE, QtWebEngine, CEF and Playwright WebKit. None
are installed on this host, so they would be untestable code. The interface
takes a new engine as one file plus one line in `registry.c`; CMake already
gates compilation on the dependency being present.

`--serve` (the guest protocol) is deliberately unimplemented and fails loudly.
It is unreachable until the guest-side spikes pass: AF_UNIX reachability,
shared-memory frame transport, and guest-side compositing.

## The blocker this work does not touch

`-[NSWindow setContentViewController:]` is unimplemented in
`src/external/cocotron/AppKit/NSWindow.m`. YouLearn v0.3.1 throws there
(`MainWindowController..cfyc + 652` <- `AppDelegate.openMainWindow`) when it
builds its main window, so it still cannot launch no matter how good the host
engine is. That is an AppKit gap, not a WebKit one, and it is a much smaller
job than anything in this repository.
