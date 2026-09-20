# Experiments

This folder holds renderer code that isn't part of the compiled firmware
(not referenced anywhere in `main/CMakeLists.txt`). Each subfolder is a
past attempt at a feature that was never finished: the UI code is often
reasonably complete, but none of them are wired to a real data source,
a navigation entry point, or (for chat/news) a working backend.

They're kept here instead of deleted so a future session can pick them
back up without starting from scratch. If you want to revive one, expect
to also write the missing plumbing described below.

## chat/

`ChatRenderer` + `streaming/` (`TextChunker`, `TtsStreamer`,
`StreamPipeline`) — a full streaming STT → LLM → TTS chat UI (bubbles,
streaming append, status/listening states).

**Status:** `StreamPipeline` is never instantiated anywhere in
`application.cc`; no `Protocol`/WebSocket client is ever created either.
The whole pipeline is inert — there's no backend in `server/` for it to
talk to anymore (the old `llmserve.py`/port-9001 WebSocket service
referenced in historical docs doesn't exist in this repo).

**To revive:** stand up a real backend (WebSocket LLM/TTS service), wire
a `Protocol` implementation + `StreamPipeline` into `Application`, add a
`Chat` module back to the UI manager and its navigation entry.

## ebook/

`EbookRenderer` — TXT file list + paginated reader, with portrait-mode
rotation and word-wrap. The most complete-feeling of the experiments.

**Status:** No data source. `application.cc` never calls `SetFileList()`
or `OpenFile()`, and there's no way to get a `.txt` file onto the device
(AP-transfer only handles photos).

**To revive:** scan SPIFFS/a photo-transfer-like upload path for `.txt`
files, wire `SetFileList`/`OpenFile`, add the module + navigation entry.

## news/

`NewsRenderer` — list + preview modal, with a TTS "read aloud" hook
(`tts_request_cb_`).

**Status:** `SetItems()` is never called from anywhere; there is no news
feed backend. Also depends on the (currently dead) TTS pipeline to be
useful.

**To revive:** build a news-fetching bridge (similar in spirit to
`server/calendar_bridge.py`), wire it in, and reconnect the TTS callback
once chat/TTS is revived.

## calendar/

`CalendarRenderer` + `calendar_component.*` — full month grid, lunar
calendar conversion, date-selection mode, and a `GetVoiceQueryContext()`
helper clearly meant to feed a selected date into the (dead) voice/chat
pipeline.

**Status:** No `SwitchPage(Calendar)` call exists anywhere — unreachable
even as a debug page.

**To revive:** add a navigation entry; `GetVoiceQueryContext()` only
becomes useful once chat/voice is revived too.

---

None of the above are built by `idf.py build` — they're excluded from
`main/CMakeLists.txt` on purpose. If you revive one, move it back under
`main/ui/renderers/rawdraw/` (and `main/streaming/` for chat's pipeline),
re-add it to `CMakeLists.txt`, and give it a slot in
`RawDrawUiManager`/the module registry.
