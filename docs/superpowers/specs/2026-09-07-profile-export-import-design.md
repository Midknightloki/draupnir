# Profile Export / Import — Design

*Written 2026-09-07. Targets: `companion_app/`, `firmware/Waveshare_LVGL_Test/ble_engine.cpp`,
`firmware/M5_M6_config/M5_M6_config.ino`.*

## Goal

Let a user write their Draupnir configuration to a file and read it back — for backup and restore,
for sharing a profile with someone else, and for moving a profile between the two boards. All three
uses are in scope.

This is one half of what the roadmap calls M10. The other half — buzzer/haptic feedback — is
**tabled**, and for a reason narrower than "it's blocked": nobody has confirmed the Waveshare has a
haptic driver on the bus at all. See `docs/HANDOFF.md` §6.

## The constraint that shapes everything

**The companion app has never possessed the icon bitmaps.** Both boards strip `,"icon_xbm":"<hex>"`
out of `get_profiles` responses on the fly, because the app only needs the icon *name* — the 18×18
1bpp bitmap is display-side data it never renders. The device merges the stored bitmaps back in on
`save_profiles`, matched by `pos` within a profile.

So an export taken from the app as it stands today would contain **no custom icon bitmaps**.
Re-importing to the same device would mostly survive via that `pos` merge; importing to a different
device would lose every custom icon, silently. Silent absence is this project's most expensive
recurring failure (`HANDOFF.md` §4, findings 1–5), and shipping export/import without addressing it
would add a sixth instance.

Hence the one protocol change below. Everything else is app-side.

## Non-goals

- **No new BLE command.** One optional field on an existing command, not a second request/response
  shape to keep in sync across two firmwares and an app.
- **No device-capability negotiation.** Deliberately rejected in §5.
- **No cloud, account, or sync.** A file is the transport; the OS owns where it goes.
- **No re-implementation of firmware validation in the app.** The device stays the authority.
- **No new Flutter dependency.** `file_selector` and `shared_preferences` are already in
  `pubspec.yaml`.

---

## 1. Protocol change — `include_icons`

`get_profiles` gains one optional boolean:

```json
{"cmd": "get_profiles", "include_icons": true}
```

Absent or false → today's behaviour exactly, byte for byte. True → the response carries
`icon_xbm` fields through instead of swallowing them.

Normal fetches keep today's bandwidth and latency; only an export pays the extra bytes. The
stripping stays the default precisely because it is the common path.

`save_profiles` is **not** touched. Its merge-on-save already handles an icon-bearing document
correctly — the code reads *"If the incoming macro HAS an `icon_xbm`, it wins -- never overwrite a
bitmap the client actually sent"* — so a document fetched with icons and saved back composes with
no special-casing.

### The two boards need different edits

They solved stripping differently, and the spec must not pretend otherwise.

**Waveshare** (`ble_engine.cpp:235-241`) *composes* two sinks — `IconXbmFilterSink` wraps
`BleChunkSink`:

```c
    BleChunkSink sink;
    IconXbmFilterSink filtered(sink);
    filtered.print("{\"status\":\"ok\",\"profiles\":");
    profiles_serialize(filtered);
    filtered.print("}\n");
    if (filtered.flushRemainder() && sink.flushRemainder()) {
```

The natural change is to **bypass the filter** when icons are wanted, writing straight to
`BleChunkSink`, rather than adding a passthrough flag to a filter whose entire purpose is filtering.
Note the two-stage flush: `filtered.flushRemainder()` forwards still-withheld marker-prefix bytes
into `sink`'s buffer before `sink.flushRemainder()` sends the last chunk. On the bypass path there
is no filter to flush, so only `sink.flushRemainder()` is called — **getting this wrong truncates
the response's tail**, and the failure looks like malformed JSON at the app, not like a missing
flush.

**M5Dial** (`M5_M6_config.ino:979-983`) *folds* stripping into `BleChunkSink` itself (`_skipping`
is a member, line 880):

```c
    BleChunkSink sink;
    sink.print("{\"status\":\"ok\",\"profiles\":");
    serializeJson(profilesDoc, sink);           // icon_xbm stripped on the fly by the sink
    sink.print("}\n");
```

Here there is nothing to bypass, so `BleChunkSink` takes a constructor flag that disables the
marker matching. The stale comment on the `serializeJson` line must be updated with it.

Both edits are small. They are not the same edit, and a plan that treats them as one will produce a
broken board.

---

## 2. File format

JSON, in a small envelope:

```json
{
  "draupnir": "config",
  "schema": 3,
  "exported": "2026-09-07T14:22:00Z",
  "payload": { }
}
```

| Field | Meaning |
|---|---|
| `draupnir` | Kind — `"config"` or `"profile"` — and a magic marker. A file without this key is not ours. |
| `schema` | Profile-schema version, same semantics as `version` in `profiles.json`. **Authoritative.** |
| `exported` | ISO-8601 UTC timestamp. Informational; shown in the UI, never validated. |
| `payload` | For `"config"`, the whole `profiles.json` document. For `"profile"`, one profile object. |

**Why the envelope carries `schema`.** `version` lives at the **document root** of `profiles.json`,
so a single profile object — `{"name":"Gaming","macros":[…],"color":"#00CED1"}` — has no version
field anywhere inside it. Without an envelope a profile export would be unvalidatable. Putting
`schema` in the envelope makes both kinds validate the same way.

**What export writes into `schema`.** The source document's own `version` field — *not* the app's
maximum. If the fetched document declares `"version": 2`, the export says `"schema": 2`, because
labelling a v2 document as v3 would be a lie the importer cannot detect. This is the live case, not
a hypothetical: the owner's current `profiles.json` declares **version 2**, which predates the M8b
bump and is perfectly legal — `schemaVersionOk()` accepts `ver <= SCHEMA_VERSION`, and v3 is a
strict relaxation of v2 (`Draupnir_Spec.md` §7). If the source declares no `version` at all — also
legal, the check returns true for a null — export writes `3`, matching the firmware's own treatment
of a version-less document as current.

For a single-profile export the profile object has no version of its own, so `schema` is taken from
the `version` of the document it was fetched from, by the same rule.

If a hand-edited file's envelope `schema` disagrees with a config payload's internal `version`, the
**envelope wins**; export always writes them in agreement.

The `draupnir` key earns its place by turning "user picked the wrong file" into *"This isn't a
Draupnir export file"* rather than a JSON parse error or, worse, a structurally-plausible import.

---

## 3. The two verbs

**Both exports require a connected device and perform a fresh fetch.** The app's in-memory
`profilesData` was fetched without icons, so exporting from it would silently produce the
icon-less file this design exists to prevent. Export therefore issues its own
`get_profiles` with `include_icons: true` and writes *that* document — it never exports what is
already on screen. If no device is connected, export is unavailable rather than partial.

**Export whole config** → `payload` is the freshly fetched `profiles.json` document verbatim:
every profile plus `settings` (`brightness`, `ledBrightness`, `buzzer`, `orientation`).

**Export single profile** → `payload` is one profile object from that same fetch, icons included.

**Import a config** → **replaces everything**. This is restore.

**Import a profile** → **appends** as a new profile; never overwrites. This is sharing. A name
collision is suffixed — `Gaming` → `Gaming (imported)`, then `Gaming (imported 2)`.

**Macro `pos` values are preserved exactly, on both verbs.** Nothing is renumbered to fit a
board's ring, including on import to the M5Dial. `pos` is a stable identifier and the ring-order
key (spec v3, `CLAUDE.md`), and rewriting it would silently reorder a profile and break the
round-trip back to the originating board. `pos` is unique only *within* a profile, so an appended
profile cannot collide with an existing one. §5 covers what the user is told instead.

Keeping these as distinct verbs is the point. It means "restore my backup" can never silently mean
"append a duplicate", and "accept someone's profile" can never silently mean "wipe my setup". The
destructive semantics belong to exactly one verb, and the file's `draupnir` field says which one
applies before anything is sent.

Import always ends by sending the resulting document through the ordinary `save_profiles` path — no
second write path, and therefore no second set of bugs.

---

## 4. Import safety

Whole-config import is the only irreversible destructive action in the product. Three layers.

**Validate before sending.** The app checks: envelope present, `draupnir` is `"config"` or
`"profile"`, `schema` is an integer `<= 3`, and the payload has the right shape (`config` → a
`profiles` array; `profile` → `name` and `macros`). Failures produce a specific message and send
nothing.

It deliberately stops there. The app does **not** re-implement the firmware's semantics — the
device is the authority, and over-validating would reject documents the firmware happily accepts.
Concretely: the firmware tolerates macros with **no `pos` field at all**, and several such macros
exist in the owner's live Gaming profile today. An app-side "every macro must have a `pos`" check
would reject a real, working config.

**The device's existing defences carry the rest.** Import is just `save_profiles`, which already
holds the schema check (`ver <= SCHEMA_VERSION`) and the atomic temp → verify → rename. A malformed
or refused import cannot corrupt the device: on any failure before the rename the original file is
untouched. This is M6/H4 on the Waveshare and this year's security-gate Task 3 on the M5Dial,
already paid for.

**A pre-import snapshot.** Before **any** import — config or profile — the app writes the current
config JSON to `shared_preferences` under a single key with a timestamp, and offers **Undo import**,
which re-sends it. One rule rather than two: an append is not destructive, but undoing one is just
as useful, and a rule that fires on every import cannot be got wrong about which case it covers.
Survives an app restart. Uses a dependency already present.

That third layer is the one piece here that is arguably scope creep, and it is called out as such so
a reviewer can cut it deliberately rather than by omission. The argument for keeping it: without it,
one wrong tap in a file picker permanently destroys a configuration that took real time to assemble,
and the only recovery is a backup the user may not have taken yet. Roughly fifteen lines.

---

## 5. The cross-device warning

Shown at import whenever the payload contains macros with `pos > 15`, phrased as a property of the
**profile**, not of the device:

> *3 macros sit above ring position 15. They transfer and fire normally, but will not appear on the
> M5Dial's 16-dot ring.*

**Why not warn only when the target is an M5Dial.** The app can identify the board only by its
advertised name, and the spec is explicit that the name is *"a label for humans, not a protocol
constant: a new board picks a new name without an app change"* (§7, Advertised names). Branching
behaviour on that string would make a deliberately cosmetic value load-bearing, and would break the
moment a third board appears.

The principled alternative is a capabilities command — and that is exactly the protocol surface
this design rejected in §1. Warning on file content requires no negotiation, is never wrong, and
costs a Waveshare user one line they read once. This is M8b's documented divergence
(`Draupnir_Spec.md` §6: a macro above `pos` 15 fires on the M5Dial but does not appear on its
16-dot ring) surfaced at the moment it becomes relevant.

---

## 6. Verification

No host test framework and no CI in this repo (`CLAUDE.md`); the automated gate is the compile plus
`flutter analyze`, and correctness is established on hardware. **Both boards must be present** —
criterion 5 is a two-board test and is the one that proves the milestone.

Compile gate:

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB firmware/M5_M6_config
arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled firmware/Waveshare_LVGL_Test
cd companion_app && flutter analyze
```

Hardware criteria:

1. **No regression, run first on both boards.** A normal fetch (no `include_icons`) returns a
   response with **no** `icon_xbm`, and the ring renders as before. This is the gate: the change
   touches the serialization path every fetch uses.
2. `include_icons: true` → the response **does** carry `icon_xbm`, and the full document arrives
   intact. Watch the tail specifically — see the two-stage-flush warning in §1.
3. **Export whole config** → a file is written containing the envelope and the icon hex.
4. **Import it back** → profiles restored and **custom icons still render on the ring**. This is
   the round-trip where a stripping bug shows up as silently blank icons.
5. **Export a profile from one board, import it to the other** → appended, and its icons render on
   the receiving board. Criteria 1–4 all pass trivially on a single device; this one does not.
6. `schema: 99` → refused with a clear message, and the device's config is untouched afterwards.
7. A JSON file with no `draupnir` key → clear error, nothing sent.
8. A profile containing a macro above `pos` 15 → the warning appears, and the macro still fires
   after import.
9. **Undo import** restores the pre-import config.

### Recording the result

Criteria 1–9 are all positive-path or error-path tests against a cooperating app; none of them is a
security test, and none should be recorded as one. Log the outcome in `HANDOFF.md` §4 in the terms
actually observed, and if any criterion is skipped, say which and why — the M5Dial security gate's
negative test sat in the NOT-verified list for a day rather than being blurred into the passing
results, and that is the standard here too.
