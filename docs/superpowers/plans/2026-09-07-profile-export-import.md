# Profile Export / Import Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a user write their Draupnir configuration to a file and read it back — for backup/restore, for sharing a profile, and for moving one between the two boards.

**Architecture:** One optional field (`include_icons`) on the existing `get_profiles` command makes the firmware stop stripping icon bitmaps, implemented differently on each board because they stripped differently. Everything else is app-side: a new pure-Dart module owns the file envelope and its validation, the state layer gains export/import/undo, and the dashboard gains the UI.

**Tech Stack:** Arduino ESP32 (m5stack:esp32 3.3.8 NimBLE / esp32:esp32 3.3.11), ArduinoJson 7.4.3; Flutter + provider + `file_selector` + `shared_preferences`, tested with `flutter_test`.

**Spec:** `docs/superpowers/specs/2026-09-07-profile-export-import-design.md` — read it first. It carries the reasoning behind every choice below, including why `schema` lives in the envelope and why the cross-device warning is not conditioned on the board.

## Global Constraints

1. **Two test regimes, and they are not the same.** The Dart side has `flutter_test` and a green baseline — write real failing tests first (Tasks 3, 5, 6). The **firmware has no host test framework and no CI**; its gate is `arduino-cli compile` plus hardware verification in Task 8. Do not invent a firmware test framework or write firmware tests that assert nothing.
2. **`include_icons` absent or false must behave exactly as today, byte for byte.** It is the common path; only export pays.
3. **The two boards need different edits.** Waveshare *composes* `IconXbmFilterSink` around `BleChunkSink` → bypass the filter. M5Dial *folds* stripping into `BleChunkSink` → add a flag. A single shared edit is wrong and will break one board.
4. **Never renumber `pos`.** It is a stable identifier and the ring-order key (spec v3, `CLAUDE.md`). Import preserves it exactly, on both verbs.
5. **The device is the authority on validity.** The app validates the envelope and basic shape only. Do **not** require every macro to have a `pos` — the firmware tolerates macros without one, and the owner's live Gaming profile contains several.
6. **No new Flutter dependency.** `file_selector` and `shared_preferences` are already in `pubspec.yaml`.
7. **Export writes the source document's own `version` into `schema`**, not the app's maximum. The owner's live config declares `version: 2` and that is legal.
8. **Threading rules (firmware).** BLE callbacks hand off via queues/flags drained in `loop()`; the macro engine is `loop()`-task only; stop macros before reloading profiles.
9. **Don't claim hardware verification that was not performed.** Task 8 records what was actually observed, and names anything skipped.

**Gates used throughout:**

```bash
# Firmware
arduino-cli compile --fqbn m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB firmware/M5_M6_config
arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled firmware/Waveshare_LVGL_Test
# App
cd companion_app && flutter test && flutter analyze
```

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `firmware/Waveshare_LVGL_Test/ble_engine.cpp` | Modify | Bypass `IconXbmFilterSink` when icons are requested. |
| `firmware/M5_M6_config/M5_M6_config.ino` | Modify | `BleChunkSink` gains a strip-or-not flag. |
| `companion_app/lib/services/profile_transfer.dart` | **Create** | Pure logic: build an envelope, parse and validate one, count `pos > 15`, suffix colliding names. No BLE, no UI, no I/O — which is exactly why it is testable and gets its own task. |
| `companion_app/test/profile_transfer_test.dart` | **Create** | Unit tests for the above. |
| `companion_app/lib/state/draupnir_state.dart` | Modify | `fetchProfilesForExport()`, `importConfig()`, `importProfile()`, snapshot + `undoImport()`. |
| `companion_app/lib/screens/dashboard_screen.dart` | Modify | Export/import menu, the `pos > 15` warning dialog, the Undo affordance. |

`profile_transfer.dart` is a new file rather than more methods on `DraupnirState` because the envelope rules are the part most worth testing in isolation and the part a reviewer most needs to read in one piece. `DraupnirState` is already ~700 lines and owns transport plus connection state; adding file-format parsing to it would tangle two responsibilities and make the format untestable without a BLE stack.

---

### Task 1: Waveshare — bypass the icon filter

**Files:**
- Modify: `firmware/Waveshare_LVGL_Test/ble_engine.cpp` (the `get_profiles` branch, ~line 235)

**Interfaces:**
- Consumes: nothing.
- Produces: `get_profiles` accepts an optional `include_icons` boolean. Task 4 sends it.

- [ ] **Step 1: Read the existing branch**

It currently reads:

```c
  if (cmd == "get_profiles") {
    bleSendPreamble();
    BleChunkSink sink;
    IconXbmFilterSink filtered(sink);
    filtered.print("{\"status\":\"ok\",\"profiles\":");
    profiles_serialize(filtered);
    filtered.print("}\n");
    // filtered.flushRemainder() forwards any still-withheld marker-prefix bytes into sink's
    // buffer; sink.flushRemainder() then sends whatever chunk that leaves (and is also what
    // surfaces a failure sink.write() hit earlier, since it checks `failed` itself).
    if (filtered.flushRemainder() && sink.flushRemainder()) {
      Serial.printf("[ble] get_profiles: streamed %u bytes\n", (unsigned)sink.totalSent);
    } else {
```

- [ ] **Step 2: Replace it with the two-path version**

```c
  if (cmd == "get_profiles") {
    // Export needs the icon bitmaps the normal path strips (see
    // docs/superpowers/specs/2026-09-07-profile-export-import-design.md). Absent or false is the
    // common path and must stay byte-identical to before.
    bool includeIcons = req["include_icons"] | false;
    bleSendPreamble();
    BleChunkSink sink;
    bool ok;
    if (includeIcons) {
      // Serialize STRAIGHT into the chunk sink -- IconXbmFilterSink exists only to strip, so
      // there is nothing to configure, only to skip. Note there is correspondingly no filter to
      // flush here: only sink.flushRemainder() applies. Calling a filter's flush on this path
      // (or omitting sink's) truncates the tail of the response, which presents at the app as
      // malformed JSON rather than as a missing flush.
      sink.print("{\"status\":\"ok\",\"profiles\":");
      profiles_serialize(sink);
      sink.print("}\n");
      ok = sink.flushRemainder();
    } else {
      IconXbmFilterSink filtered(sink);
      filtered.print("{\"status\":\"ok\",\"profiles\":");
      profiles_serialize(filtered);
      filtered.print("}\n");
      // filtered.flushRemainder() forwards any still-withheld marker-prefix bytes into sink's
      // buffer; sink.flushRemainder() then sends whatever chunk that leaves (and is also what
      // surfaces a failure sink.write() hit earlier, since it checks `failed` itself).
      ok = filtered.flushRemainder() && sink.flushRemainder();
    }
    if (ok) {
      Serial.printf("[ble] get_profiles: streamed %u bytes (icons=%d)\n",
                    (unsigned)sink.totalSent, includeIcons ? 1 : 0);
    } else {
```

Leave the existing `else` body (the send-aborted diagnostic) exactly as it is.

- [ ] **Step 3: Compile**

Run:
```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled firmware/Waveshare_LVGL_Test
```
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add firmware/Waveshare_LVGL_Test/ble_engine.cpp
git commit -m "feat: get_profiles include_icons on the Waveshare

Export needs the icon bitmaps the normal path strips. IconXbmFilterSink
exists only to strip, so the icon-bearing path bypasses it entirely
rather than gaining a passthrough mode.

Note the flush asymmetry: the filtered path needs both flushes (the
filter forwards withheld marker-prefix bytes into the sink's buffer
first), the bypass path only sink's. Getting that wrong truncates the
response tail and looks like malformed JSON at the app."
```

---

### Task 2: M5Dial — a strip-or-not flag on BleChunkSink

**Files:**
- Modify: `firmware/M5_M6_config/M5_M6_config.ino` (`BleChunkSink` ~line 813, `get_profiles` ~line 979)

**Interfaces:**
- Consumes: nothing.
- Produces: `BleChunkSink(bool stripIcons = true)`. Same `include_icons` wire contract as Task 1.

**Why this differs from Task 1:** on this board the stripping state machine is *inside* `BleChunkSink` (`_matched`, `_skipping`, `emit()`), so there is no wrapper to bypass.

- [ ] **Step 1: Add the constructor and flag**

In `class BleChunkSink : public Print {`, immediately after the `public:` line and before `bool failed`:

```c
  // Stripping is the default because it is the common path: the app only needs icon *names*,
  // and the 18x18 bitmaps are display-side data it never renders. Export is the exception --
  // it needs the bitmaps to travel, or a profile shared to another device silently loses every
  // custom icon. See the export/import design doc.
  explicit BleChunkSink(bool stripIcons = true) : _stripIcons(stripIcons) {}
```

Then in the `private:` section, next to `bool _skipping = false;`:

```c
  bool _stripIcons = true; // false => forward icon_xbm through untouched (export path)
```

- [ ] **Step 2: Short-circuit the marker matching**

In `size_t write(uint8_t c) override`, the body currently begins:

```c
  size_t write(uint8_t c) override {
    if (failed) return 0;
    if (_skipping) {
```

Insert the bypass between those two lines:

```c
  size_t write(uint8_t c) override {
    if (failed) return 0;
    if (!_stripIcons) return emit(c) ? 1 : 0; // export path: no marker matching at all
    if (_skipping) {
```

`flushRemainder()` needs no change: it emits any withheld marker prefix from `_matched`, which stays 0 on this path.

- [ ] **Step 3: Read the flag in get_profiles**

The branch currently reads:

```c
    bleSendPreamble();
    BleChunkSink sink;
    sink.print("{\"status\":\"ok\",\"profiles\":");
    serializeJson(profilesDoc, sink);           // icon_xbm stripped on the fly by the sink
    sink.print("}\n");                           // '\n' = end-of-message delimiter for the app
    if (sink.flushRemainder()) {
      Serial.print("get_profiles: streamed bytes = ");
      Serial.println(sink.totalSent);
```

Replace those lines with:

```c
    bool includeIcons = req["include_icons"] | false;
    bleSendPreamble();
    BleChunkSink sink(!includeIcons);
    sink.print("{\"status\":\"ok\",\"profiles\":");
    serializeJson(profilesDoc, sink);           // icon_xbm stripped by the sink unless exporting
    sink.print("}\n");                           // '\n' = end-of-message delimiter for the app
    if (sink.flushRemainder()) {
      Serial.printf("get_profiles: streamed bytes = %u (icons=%d)\n",
                    (unsigned)sink.totalSent, includeIcons ? 1 : 0);
```

Delete the now-redundant `Serial.println(sink.totalSent);` line that followed the old `Serial.print`.

- [ ] **Step 4: Compile**

Run:
```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB firmware/M5_M6_config
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add firmware/M5_M6_config/M5_M6_config.ino
git commit -m "feat: get_profiles include_icons on the M5Dial

Same wire contract as the Waveshare, different edit: this board folds the
icon_xbm stripping into BleChunkSink itself rather than wrapping it, so
there is no filter to bypass -- the sink takes a flag that skips marker
matching entirely.

flushRemainder needs no change: it emits withheld marker-prefix bytes
from _matched, which stays 0 when stripping is off."
```

---

### Task 3: The transfer module (pure logic, real TDD)

**Files:**
- Create: `companion_app/lib/services/profile_transfer.dart`
- Create: `companion_app/test/profile_transfer_test.dart`

**Interfaces:**
- Consumes: nothing.
- Produces, for Tasks 4-6:
  - `const int kMaxSchema = 3;`
  - `Map<String, dynamic> buildConfigEnvelope(Map<String, dynamic> document, {DateTime? now})`
  - `Map<String, dynamic> buildProfileEnvelope(Map<String, dynamic> profile, Map<String, dynamic> sourceDocument, {DateTime? now})`
  - `class TransferException implements Exception { final String message; }`
  - `class ParsedTransfer { final String kind; final int schema; final Map<String, dynamic> payload; }` — `kind` is `'config'` or `'profile'`
  - `ParsedTransfer parseEnvelope(String jsonText)` — throws `TransferException` with a user-facing message
  - `int countMacrosAbovePos15(ParsedTransfer t)`
  - `String uniqueProfileName(String desired, List<dynamic> existingProfiles)`

- [ ] **Step 1: Write the failing tests**

Create `companion_app/test/profile_transfer_test.dart`:

```dart
import 'dart:convert';
import 'package:flutter_test/flutter_test.dart';
import 'package:companion_app/services/profile_transfer.dart';

void main() {
  group('buildConfigEnvelope', () {
    test('carries the document\'s own version into schema, not the app maximum', () {
      final doc = {'version': 2, 'profiles': [], 'settings': {}};
      final env = buildConfigEnvelope(doc, now: DateTime.utc(2026, 9, 7, 14, 22));
      expect(env['draupnir'], 'config');
      expect(env['schema'], 2);
      expect(env['exported'], '2026-09-07T14:22:00.000Z');
      expect(env['payload'], doc);
    });

    test('a version-less document exports as schema 3', () {
      final env = buildConfigEnvelope({'profiles': []});
      expect(env['schema'], 3);
    });
  });

  group('buildProfileEnvelope', () {
    test('takes schema from the source document, since a profile has no version', () {
      final profile = {'name': 'Gaming', 'macros': []};
      final env = buildProfileEnvelope(profile, {'version': 2, 'profiles': []});
      expect(env['draupnir'], 'profile');
      expect(env['schema'], 2);
      expect(env['payload'], profile);
    });
  });

  group('parseEnvelope', () {
    String enc(Object o) => jsonEncode(o);

    test('round-trips a config envelope', () {
      final doc = {'version': 3, 'profiles': [], 'settings': {}};
      final t = parseEnvelope(enc(buildConfigEnvelope(doc)));
      expect(t.kind, 'config');
      expect(t.schema, 3);
      expect(t.payload, doc);
    });

    test('rejects a file with no draupnir key', () {
      expect(() => parseEnvelope(enc({'profiles': []})),
          throwsA(isA<TransferException>()));
    });

    test('rejects malformed JSON', () {
      expect(() => parseEnvelope('not json at all'),
          throwsA(isA<TransferException>()));
    });

    test('rejects a schema newer than this app understands', () {
      expect(
          () => parseEnvelope(enc({
                'draupnir': 'config',
                'schema': 99,
                'payload': {'profiles': []}
              })),
          throwsA(isA<TransferException>()));
    });

    test('rejects an unknown kind', () {
      expect(
          () => parseEnvelope(enc({
                'draupnir': 'sandwich',
                'schema': 3,
                'payload': {'profiles': []}
              })),
          throwsA(isA<TransferException>()));
    });

    test('rejects a config payload with no profiles array', () {
      expect(
          () => parseEnvelope(
              enc({'draupnir': 'config', 'schema': 3, 'payload': {}})),
          throwsA(isA<TransferException>()));
    });

    test('rejects a profile payload missing macros', () {
      expect(
          () => parseEnvelope(enc({
                'draupnir': 'profile',
                'schema': 3,
                'payload': {'name': 'X'}
              })),
          throwsA(isA<TransferException>()));
    });

    test('accepts macros with no pos — the firmware tolerates them', () {
      final t = parseEnvelope(enc({
        'draupnir': 'profile',
        'schema': 3,
        'payload': {
          'name': 'Gaming',
          'macros': [
            {'name': 'GG', 'actions': []}
          ]
        }
      }));
      expect(t.kind, 'profile');
    });
  });

  group('countMacrosAbovePos15', () {
    test('counts across every profile of a config', () {
      final t = parseEnvelope(jsonEncode({
        'draupnir': 'config',
        'schema': 3,
        'payload': {
          'profiles': [
            {
              'name': 'A',
              'macros': [
                {'pos': 15},
                {'pos': 16}
              ]
            },
            {
              'name': 'B',
              'macros': [
                {'pos': 20},
                {'name': 'no pos'}
              ]
            }
          ]
        }
      }));
      expect(countMacrosAbovePos15(t), 2);
    });

    test('is zero for a profile that fits the ring', () {
      final t = parseEnvelope(jsonEncode({
        'draupnir': 'profile',
        'schema': 3,
        'payload': {
          'name': 'A',
          'macros': [
            {'pos': 0},
            {'pos': 15}
          ]
        }
      }));
      expect(countMacrosAbovePos15(t), 0);
    });
  });

  group('uniqueProfileName', () {
    test('returns the name unchanged when free', () {
      expect(uniqueProfileName('Gaming', [
        {'name': 'Windows'}
      ]), 'Gaming');
    });

    test('suffixes on collision', () {
      expect(uniqueProfileName('Gaming', [
        {'name': 'Gaming'}
      ]), 'Gaming (imported)');
    });

    test('numbers subsequent collisions', () {
      expect(
          uniqueProfileName('Gaming', [
            {'name': 'Gaming'},
            {'name': 'Gaming (imported)'}
          ]),
          'Gaming (imported 2)');
    });
  });
}
```

**Note on the import path:** the package name is `companion_app` (verified in
`companion_app/pubspec.yaml:1`), which is why the import reads
`package:companion_app/services/profile_transfer.dart` rather than anything resembling the product
name.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd companion_app && flutter test test/profile_transfer_test.dart`
Expected: FAIL — the file `lib/services/profile_transfer.dart` does not exist yet.

- [ ] **Step 3: Write the implementation**

Create `companion_app/lib/services/profile_transfer.dart`:

```dart
import 'dart:convert';

/// The newest profile schema this app understands. Mirrors SCHEMA_VERSION in the firmware;
/// the device enforces the same rule (`ver <= SCHEMA_VERSION`) and remains the authority.
const int kMaxSchema = 3;

/// Thrown for anything wrong with a transfer file. The message is shown to the user verbatim,
/// so it must read as an explanation, not as a stack trace.
class TransferException implements Exception {
  final String message;
  TransferException(this.message);
  @override
  String toString() => message;
}

/// A validated transfer file. [kind] is 'config' or 'profile'.
class ParsedTransfer {
  final String kind;
  final int schema;
  final Map<String, dynamic> payload;
  ParsedTransfer(this.kind, this.schema, this.payload);

  bool get isConfig => kind == 'config';
}

/// The schema a document should be exported as: its own declared version, never this app's
/// maximum. Labelling a v2 document as v3 would be a lie the importer cannot detect. A document
/// with no version exports as [kMaxSchema], matching the firmware, whose schemaVersionOk()
/// returns true for a null version — i.e. it treats a version-less document as current.
int _schemaOf(Map<String, dynamic> document) {
  final v = document['version'];
  return v is int ? v : kMaxSchema;
}

String _stamp(DateTime? now) =>
    (now ?? DateTime.now().toUtc()).toUtc().toIso8601String();

Map<String, dynamic> buildConfigEnvelope(Map<String, dynamic> document,
    {DateTime? now}) {
  return {
    'draupnir': 'config',
    'schema': _schemaOf(document),
    'exported': _stamp(now),
    'payload': document,
  };
}

Map<String, dynamic> buildProfileEnvelope(
    Map<String, dynamic> profile, Map<String, dynamic> sourceDocument,
    {DateTime? now}) {
  return {
    'draupnir': 'profile',
    // A profile object carries no version of its own — version lives at the document root —
    // so it is taken from the document this profile was fetched from.
    'schema': _schemaOf(sourceDocument),
    'exported': _stamp(now),
    'payload': profile,
  };
}

ParsedTransfer parseEnvelope(String jsonText) {
  Object? decoded;
  try {
    decoded = jsonDecode(jsonText);
  } catch (_) {
    throw TransferException(
        'That file isn\'t valid JSON, so it can\'t be a Draupnir export.');
  }

  if (decoded is! Map<String, dynamic>) {
    throw TransferException(
        'That file isn\'t a Draupnir export — expected a JSON object.');
  }

  final kind = decoded['draupnir'];
  if (kind == null) {
    throw TransferException(
        'That file isn\'t a Draupnir export (no "draupnir" marker).');
  }
  if (kind != 'config' && kind != 'profile') {
    throw TransferException(
        'Unrecognised export kind "$kind" — expected "config" or "profile".');
  }

  final schema = decoded['schema'];
  if (schema is! int) {
    throw TransferException('That export is missing a valid "schema" number.');
  }
  if (schema > kMaxSchema) {
    throw TransferException(
        'That export uses schema $schema; this app understands up to $kMaxSchema. '
        'Update the app.');
  }

  final payload = decoded['payload'];
  if (payload is! Map<String, dynamic>) {
    throw TransferException('That export has no readable "payload".');
  }

  // Shape checks only. The device is the authority on validity, and over-validating here would
  // reject documents the firmware happily accepts — macros with no "pos" field, for instance,
  // which exist in real profiles today.
  if (kind == 'config') {
    if (payload['profiles'] is! List) {
      throw TransferException(
          'That config export has no "profiles" list — it may be truncated.');
    }
  } else {
    if (payload['name'] == null || payload['macros'] is! List) {
      throw TransferException(
          'That profile export is missing its name or macro list.');
    }
  }

  return ParsedTransfer(kind as String, schema, payload);
}

Iterable<dynamic> _allMacros(ParsedTransfer t) sync* {
  if (t.isConfig) {
    for (final p in (t.payload['profiles'] as List)) {
      final macros = (p is Map) ? p['macros'] : null;
      if (macros is List) yield* macros;
    }
  } else {
    yield* (t.payload['macros'] as List);
  }
}

/// How many macros sit above the M5Dial's 16-dot ring. They transfer and fire normally; they
/// just do not render on that board (Draupnir_Spec.md §6). Macros with no `pos` are not counted
/// — absent is not "above 15".
int countMacrosAbovePos15(ParsedTransfer t) {
  var n = 0;
  for (final m in _allMacros(t)) {
    final pos = (m is Map) ? m['pos'] : null;
    if (pos is int && pos > 15) n++;
  }
  return n;
}

/// Profile import appends and never overwrites, so a colliding name is suffixed rather than
/// replacing the existing profile.
String uniqueProfileName(String desired, List<dynamic> existingProfiles) {
  final taken = existingProfiles
      .map((p) => (p is Map) ? p['name']?.toString() : null)
      .whereType<String>()
      .toSet();
  if (!taken.contains(desired)) return desired;
  final first = '$desired (imported)';
  if (!taken.contains(first)) return first;
  for (var i = 2;; i++) {
    final candidate = '$desired (imported $i)';
    if (!taken.contains(candidate)) return candidate;
  }
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd companion_app && flutter test test/profile_transfer_test.dart`
Expected: PASS, all cases.

- [ ] **Step 5: Run the whole suite and the analyzer**

Run: `cd companion_app && flutter test && flutter analyze`
Expected: all tests pass; no new analyzer issues. Pre-existing warnings (an unused `dart:io`
import in `editor_panel.dart`, ~22 deprecation infos) are acceptable — report them, do not fix
them.

- [ ] **Step 6: Commit**

```bash
git add companion_app/lib/services/profile_transfer.dart companion_app/test/profile_transfer_test.dart
git commit -m "feat: profile transfer envelope, with tests

Pure logic in its own module: build an envelope, parse and validate one,
count macros above pos 15, suffix colliding names. No BLE, no UI, no
I/O, which is what makes it testable without a device.

schema carries the source document's own version rather than the app's
maximum -- the owner's live config declares version 2, and labelling it 3
would be a lie the importer cannot detect.

Validation stops at envelope and shape. The device is the authority, and
requiring every macro to have a pos would reject real profiles."
```

---

### Task 4: Fetch-for-export in the state layer

**Files:**
- Modify: `companion_app/lib/state/draupnir_state.dart`

**Interfaces:**
- Consumes: `buildConfigEnvelope`, `buildProfileEnvelope` from Task 3.
- Produces: `Future<Map<String, dynamic>?> fetchProfilesForExport()` — returns the icon-bearing document, or null on failure with `error` set.

- [ ] **Step 1: Import the module**

At the top of `draupnir_state.dart`, alongside the existing imports:

```dart
import '../services/profile_transfer.dart';
```

- [ ] **Step 2: Add the fetch**

Immediately after the existing `fetchProfiles()` method, add:

```dart
  /// Fetches a document WITH icon bitmaps, for export only.
  ///
  /// Deliberately does not touch [profilesData]. The on-screen document was fetched without
  /// icons and the UI has no use for them; more importantly, exporting from the in-memory copy
  /// would silently produce an icon-less file, which is the exact failure this whole feature
  /// exists to prevent. Export always asks the device fresh.
  Future<Map<String, dynamic>?> fetchProfilesForExport() async {
    isLoading = true;
    error = null;
    notifyListeners();

    Map<String, dynamic>? result;
    try {
      final response =
          await _sendBleRequest({'cmd': 'get_profiles', 'include_icons': true});
      if (response['status'] == 'ok') {
        result = Map<String, dynamic>.from(response['profiles'] as Map);
        needsPairing = false;
        needsConfigMode = false;
      } else if (_looksLikeConfigModeRefusal(response['message'])) {
        needsConfigMode = true;
        error = configModeRequiredMessage;
      } else {
        error = 'Failed to read profiles for export: ${response['message']}';
      }
    } catch (e) {
      if (_looksLikeAuthFailure(e)) {
        needsPairing = true;
        error = pairingRequiredMessage;
      } else {
        error = 'Bluetooth request failed: $e';
      }
    }

    isLoading = false;
    notifyListeners();
    return result;
  }
```

- [ ] **Step 3: Analyze**

Run: `cd companion_app && flutter analyze`
Expected: no new issues. (`profile_transfer.dart` is imported but not yet used here; if the
analyzer flags the unused import, leave it — Task 5 uses it — or move the import to Task 5. Do
not add an ignore comment.)

- [ ] **Step 4: Commit**

```bash
git add companion_app/lib/state/draupnir_state.dart
git commit -m "feat: fetch profiles with icons, for export only

Export must not serialize the in-memory document: it was fetched without
icon bitmaps, so exporting it would silently produce the icon-less file
this feature exists to prevent. fetchProfilesForExport asks the device
fresh with include_icons and deliberately does not touch profilesData."
```

---

### Task 5: Import, snapshot, and undo in the state layer

**Files:**
- Modify: `companion_app/lib/state/draupnir_state.dart`

**Interfaces:**
- Consumes: `ParsedTransfer`, `uniqueProfileName`, `TransferException` from Task 3; `fetchProfilesForExport()` from Task 4.
- Produces:
  - `Future<bool> importTransfer(ParsedTransfer t)` — applies and saves; true on success
  - `Future<bool> undoImport()` — restores the pre-import snapshot
  - `bool get hasImportSnapshot`
  - `DateTime? get importSnapshotTakenAt`

- [ ] **Step 1: Add the shared_preferences import and the snapshot key**

At the top of the file, with the other imports:

```dart
import 'package:shared_preferences/shared_preferences.dart';
```

As a static member of `DraupnirState`, next to the other constants:

```dart
  // One key, overwritten on every import. Survives an app restart, which an in-memory undo
  // would not — and a wrong import is the only irreversible action in this app.
  static const String _snapshotKey = 'pre_import_config';
  static const String _snapshotAtKey = 'pre_import_config_at';
```

- [ ] **Step 2: Add the snapshot accessors and import logic**

Add these methods to `DraupnirState`:

```dart
  bool _hasSnapshot = false;
  DateTime? _snapshotAt;

  bool get hasImportSnapshot => _hasSnapshot;
  DateTime? get importSnapshotTakenAt => _snapshotAt;

  /// Call once at startup so the Undo affordance survives an app restart.
  Future<void> loadImportSnapshotState() async {
    final prefs = await SharedPreferences.getInstance();
    _hasSnapshot = prefs.getString(_snapshotKey) != null;
    final at = prefs.getString(_snapshotAtKey);
    _snapshotAt = at == null ? null : DateTime.tryParse(at);
    notifyListeners();
  }

  Future<void> _takeImportSnapshot() async {
    if (profilesData == null) return;
    final prefs = await SharedPreferences.getInstance();
    final now = DateTime.now().toUtc();
    await prefs.setString(_snapshotKey, jsonEncode(profilesData));
    await prefs.setString(_snapshotAtKey, now.toIso8601String());
    _hasSnapshot = true;
    _snapshotAt = now;
  }

  /// Applies a parsed transfer to the device.
  ///
  /// A snapshot is taken before EVERY import, not only the destructive one. An append is not
  /// destructive, but undoing one is just as useful, and a rule that fires on every import
  /// cannot be got wrong about which case it covers.
  Future<bool> importTransfer(ParsedTransfer t) async {
    if (profilesData == null) {
      error = 'Connect to a Draupnir before importing.';
      notifyListeners();
      return false;
    }

    await _takeImportSnapshot();

    if (t.isConfig) {
      // Replaces everything. This is restore.
      profilesData = Map<String, dynamic>.from(t.payload);
    } else {
      // Appends. Never overwrites, and never renumbers pos — pos is a stable identifier and the
      // ring-order key, unique only WITHIN a profile, so an appended profile cannot collide.
      final profiles = profilesData!['profiles'] as List;
      final incoming = Map<String, dynamic>.from(t.payload);
      incoming['name'] =
          uniqueProfileName(incoming['name'].toString(), profiles);
      profiles.add(incoming);
    }

    notifyListeners();
    await saveProfiles();
    if (error != null) return false;

    // Re-read so the UI reflects what the device actually stored, including any icon merge it
    // performed on the way in.
    await fetchProfiles();
    return error == null;
  }

  /// Re-sends the config captured immediately before the last import.
  Future<bool> undoImport() async {
    final prefs = await SharedPreferences.getInstance();
    final saved = prefs.getString(_snapshotKey);
    if (saved == null) {
      error = 'There is no pre-import snapshot to restore.';
      notifyListeners();
      return false;
    }
    profilesData = Map<String, dynamic>.from(jsonDecode(saved) as Map);
    notifyListeners();
    await saveProfiles();
    if (error != null) return false;
    await fetchProfiles();
    return error == null;
  }
```

`jsonEncode`/`jsonDecode` need `dart:convert`, which this file already imports at line 2. No import
change beyond `shared_preferences` in Step 1.

- [ ] **Step 3: Analyze**

Run: `cd companion_app && flutter analyze`
Expected: no new issues.

- [ ] **Step 4: Run the full suite**

Run: `cd companion_app && flutter test`
Expected: PASS — Task 3's tests still green, nothing regressed.

- [ ] **Step 5: Commit**

```bash
git add companion_app/lib/state/draupnir_state.dart
git commit -m "feat: import, pre-import snapshot, and undo

Config import replaces everything; profile import appends and never
overwrites, with a suffixed name on collision. pos is preserved exactly
on both paths -- renumbering to fit a board's ring would silently reorder
a profile and break the round trip back.

A snapshot goes to shared_preferences before EVERY import, not just the
destructive one: an append is not destructive but undoing one is just as
useful, and one rule cannot be got wrong about which case it covers. It
survives an app restart, which an in-memory undo would not."
```

---

### Task 6: The UI — export, import, warning, undo

**Files:**
- Modify: `companion_app/lib/screens/dashboard_screen.dart`

**Interfaces:**
- Consumes: everything from Tasks 3-5.
- Produces: no API; UI only.

- [ ] **Step 1: Add imports**

```dart
import 'dart:convert';
import 'package:file_selector/file_selector.dart';
import '../services/profile_transfer.dart';
```

`file_selector` is already a dependency and is already used for the Synapse XML import in
`editor_panel.dart` — `openFile()` there, `getSaveLocation()` here.

- [ ] **Step 2: Add the export handlers**

Add these methods to the dashboard's `State` class:

```dart
  Future<void> _exportConfig(DraupnirState state) async {
    final doc = await state.fetchProfilesForExport();
    if (doc == null) return; // state.error is set and already rendered
    await _writeEnvelope(buildConfigEnvelope(doc), 'draupnir-config');
  }

  Future<void> _exportProfile(DraupnirState state, int profileIdx) async {
    final doc = await state.fetchProfilesForExport();
    if (doc == null) return;
    final profiles = doc['profiles'] as List;
    if (profileIdx < 0 || profileIdx >= profiles.length) return;
    final profile = Map<String, dynamic>.from(profiles[profileIdx] as Map);
    final safeName = profile['name']
        .toString()
        .replaceAll(RegExp(r'[^A-Za-z0-9._-]+'), '-')
        .toLowerCase();
    await _writeEnvelope(
        buildProfileEnvelope(profile, doc), 'draupnir-profile-$safeName');
  }

  Future<void> _writeEnvelope(
      Map<String, dynamic> envelope, String suggestedName) async {
    final location = await getSaveLocation(
      suggestedName: '$suggestedName.json',
      acceptedTypeGroups: const [
        XTypeGroup(label: 'Draupnir export', extensions: ['json'])
      ],
    );
    if (location == null) return; // user cancelled

    final bytes = utf8.encode(const JsonEncoder.withIndent('  ').convert(envelope));
    final file = XFile.fromData(
      Uint8List.fromList(bytes),
      mimeType: 'application/json',
      name: '$suggestedName.json',
    );
    await file.saveTo(location.path);

    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('Exported to ${location.path}')),
    );
  }
```

`Uint8List` needs `import 'dart:typed_data';` — add it with the other imports.

- [ ] **Step 3: Add the import handler with the warning**

```dart
  Future<void> _importFile(DraupnirState state) async {
    final XFile? file = await openFile(
      acceptedTypeGroups: const [
        XTypeGroup(label: 'Draupnir export', extensions: ['json'])
      ],
    );
    if (file == null) return;

    ParsedTransfer parsed;
    try {
      parsed = parseEnvelope(utf8.decode(await file.readAsBytes()));
    } on TransferException catch (e) {
      if (!mounted) return;
      await _alert('Can\'t import that file', e.message);
      return;
    }

    final above = countMacrosAbovePos15(parsed);
    final isConfig = parsed.isConfig;
    final buffer = StringBuffer();
    if (isConfig) {
      final n = (parsed.payload['profiles'] as List).length;
      buffer.writeln(
          'This replaces everything on your Draupnir with $n profile(s) from this file.');
    } else {
      buffer.writeln(
          'This adds "${parsed.payload['name']}" as a new profile. Nothing is overwritten.');
    }
    if (above > 0) {
      buffer.writeln();
      // Phrased as a property of the profile, not of the device: the app can only identify a
      // board by its advertised name, and the spec is explicit that the name is a label for
      // humans, not a protocol constant.
      buffer.writeln(
          '$above macro(s) sit above ring position 15. They transfer and fire normally, '
          'but will not appear on the M5Dial\'s 16-dot ring.');
    }
    buffer.writeln();
    buffer.write('You can undo this straight afterwards.');

    if (!mounted) return;
    final go = await _confirm(
        isConfig ? 'Replace everything?' : 'Add this profile?', buffer.toString());
    if (go != true) return;

    final ok = await state.importTransfer(parsed);
    if (!mounted) return;
    if (ok) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: const Text('Import complete.'),
          action: SnackBarAction(
            label: 'UNDO',
            onPressed: () => state.undoImport(),
          ),
          duration: const Duration(seconds: 8),
        ),
      );
    }
  }

  Future<void> _alert(String title, String body) => showDialog<void>(
        context: context,
        builder: (c) => AlertDialog(
          title: Text(title),
          content: Text(body),
          actions: [
            TextButton(
                onPressed: () => Navigator.pop(c), child: const Text('OK'))
          ],
        ),
      );

  Future<bool?> _confirm(String title, String body) => showDialog<bool>(
        context: context,
        builder: (c) => AlertDialog(
          title: Text(title),
          content: Text(body),
          actions: [
            TextButton(
                onPressed: () => Navigator.pop(c, false),
                child: const Text('CANCEL')),
            TextButton(
                onPressed: () => Navigator.pop(c, true),
                child: const Text('CONTINUE')),
          ],
        ),
      );
```

- [ ] **Step 4: Extend the EXISTING menu**

**Do not add a second `PopupMenuButton`.** The `AppBar` already has one (`dashboard_screen.dart`
~line 86, the `more_vert` with the debug-log `Badge`) carrying `refresh` / `settings` / `debug`.
A second would render two `⋮` icons side by side.

In that button's existing `onSelected` switch, add four cases alongside the current three:

```dart
                case 'export_config':
                  _exportConfig(state);
                  break;
                case 'export_profile':
                  _exportProfile(state, state.activeProfileIdx);
                  break;
                case 'import':
                  _importFile(state);
                  break;
                case 'undo_import':
                  state.undoImport();
                  break;
```

And in its existing `itemBuilder` list, after the `debug` item, add — matching the surrounding
`ListTile` style rather than bare `Text`, which the existing items use:

```dart
              const PopupMenuDivider(),
              if (state.profilesData != null && state.isBluetooth)
                const PopupMenuItem(
                  value: 'export_config',
                  child: ListTile(
                      leading: Icon(Icons.backup_outlined),
                      title: Text('Export All Profiles')),
                ),
              if (state.profilesData != null && state.isBluetooth)
                const PopupMenuItem(
                  value: 'export_profile',
                  child: ListTile(
                      leading: Icon(Icons.ios_share),
                      title: Text('Export This Profile')),
                ),
              if (state.profilesData != null && state.isBluetooth)
                const PopupMenuItem(
                  value: 'import',
                  child: ListTile(
                      leading: Icon(Icons.file_open_outlined),
                      title: Text('Import From File')),
                ),
              if (state.hasImportSnapshot)
                const PopupMenuItem(
                  value: 'undo_import',
                  child: ListTile(
                      leading: Icon(Icons.undo),
                      title: Text('Undo Last Import')),
                ),
```

The export/import items are gated on a live BLE connection because export performs a fresh fetch
and import ends in a save; both are meaningless offline. `undo_import` is gated only on a snapshot
existing, so it stays visible after a reconnect.

- [ ] **Step 5: Load the snapshot state at startup**

This widget has **no `initState`** — verified — and there is a deliberate comment above `build()`
explaining that launch-time auto-connect was rejected. Adding one for a local
`shared_preferences` read does not contradict that decision: it touches no radio and starts no
scan. Say so, so the next reader does not think the comment was overlooked.

Add to the dashboard's `State` class, immediately above `Widget build(...)`:

```dart
  @override
  void initState() {
    super.initState();
    // Local prefs read only — deliberately NOT a connection attempt. The comment below about
    // not auto-connecting in initState still stands; this starts no scan and touches no radio.
    // It only decides whether "Undo Last Import" appears in the menu after an app restart.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      context.read<DraupnirState>().loadImportSnapshotState();
    });
  }
```

- [ ] **Step 6: Analyze and test**

Run: `cd companion_app && flutter test && flutter analyze`
Expected: tests pass; no new analyzer issues.

- [ ] **Step 7: Commit**

```bash
git add companion_app/lib/screens/dashboard_screen.dart
git commit -m "feat: export/import UI with cross-device warning and undo

Two export verbs (whole config for backup, single profile for sharing)
and one import that reads the file's own kind, so restore can never
silently mean append and sharing can never silently mean wipe.

The pos>15 warning is phrased as a property of the profile rather than
the device: the app can only identify a board by advertised name, and the
spec is explicit that the name is a label for humans, not a protocol
constant. Warning on file content needs no capability negotiation and is
never wrong."
```

---

### Task 7: Build the APK

**Files:** none — build only.

**Interfaces:** Consumes Tasks 3-6.

**Why its own task:** `flutter install` does **not** rebuild. A suspiciously fast "Installing…"
with no Gradle line means it silently reused a stale APK, and the hardware round then tests old
code. This has cost this project time before (`HANDOFF.md` §5).

- [ ] **Step 1: Build**

Run:
```bash
cd companion_app && flutter build apk --debug
```
Expected: PASS, and the output must contain a `Running Gradle task 'assembleDebug'... <N>s` line.
If that line is absent, the build was skipped — investigate rather than proceeding.

- [ ] **Step 2: Install**

Ask the owner to connect the phone, then:
```bash
cd companion_app && flutter install
```

- [ ] **Step 3: No commit** — build artifacts are not tracked.

---

### Task 8: Hardware verification

**Files:**
- Modify: `docs/HANDOFF.md`
- Modify: `docs/Draupnir_Spec.md`

**This task requires the human and BOTH boards.** Criterion 5 is a two-board test and is the one
that proves the milestone; 1-4 pass trivially on a single device. Do not mark any criterion passed
on inference — see Global Constraint 9.

- [ ] **Step 1: Flash both boards**

M5Dial (hold BOOT, replug; COM5 in download mode, COM7 running):
```bash
arduino-cli upload -p COM5 --fqbn m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB firmware/M5_M6_config
```

Waveshare (**green-marked** cable side — the wrong orientation reaches the other MCU entirely;
COM10 download, COM11 running):
```bash
arduino-cli upload -p COM10 --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled firmware/Waveshare_LVGL_Test
```

Both need a physical replug afterwards; esptool's closing RTS reset is a no-op on these boards.

- [ ] **Step 2: Capture serial**

`arduino-cli monitor` cannot be used — it treats non-interactive stdin as an immediate quit. Use a
direct `.NET SerialPort` PowerShell script: open the port with `DtrEnable`/`RtsEnable` true, poll
`ReadLine()` with a short `ReadTimeout`, append each line to a log immediately.

- [ ] **Step 3: Walk the criteria and collect the owner's observations verbatim**

1. **No regression, run first on BOTH boards.** A normal fetch (no `include_icons`) returns a
   response with **no** `icon_xbm`, and the ring renders as before. This is the gate — the change
   touches the serialization path every fetch uses.
2. `include_icons: true` → the response **does** carry `icon_xbm` and the document arrives intact.
   Check the **tail** specifically: a flush error truncates the end and presents as malformed JSON.
   Serial shows `(icons=1)`.
3. **Export whole config** → the file contains the envelope and icon hex. Open it and look.
4. **Import it back** → profiles restored and **custom icons still render on the ring**. This is
   the round-trip where a stripping bug shows up as silently blank icons.
5. **Export a profile from one board, import it to the other** → appended, icons render on the
   receiving board.
6. `schema: 99` (hand-edit a file) → refused with a clear message; device config untouched after.
7. A JSON file with no `draupnir` key → clear error, nothing sent.
8. A profile with a macro above `pos` 15 → the warning appears, and the macro still fires after
   import.
9. **Undo last import** restores the previous config, including after an app restart.

If any criterion fails, stop and report it. Do not record a failure as a pass.

- [ ] **Step 4: Record the result**

Add to `docs/HANDOFF.md` §4 a block headed
`Verified on hardware — profile export/import, <date>`, one bullet per criterion that actually
passed, in the owner's terms. **Name anything skipped and why**, in the NOT-verified list rather
than as a footnote to a passing claim.

Update the `M10` row of the milestone table in `docs/Draupnir_Spec.md` §10 to record export/import
as done with its date, and note that the buzzer/haptic half remains tabled pending the I²C scan
(`HANDOFF.md` §6).

- [ ] **Step 5: Commit**

```bash
git add docs/HANDOFF.md docs/Draupnir_Spec.md
git commit -m "docs: profile export/import verified on hardware

Records the criteria that passed, including the two-board transfer that
is the point of the feature, and names anything skipped."
```

---

## Self-Review

**1. Spec coverage.** §1 protocol change → Tasks 1 (Waveshare bypass) and 2 (M5Dial flag), kept
separate exactly as the spec demands. §2 file format → Task 3 (`buildConfigEnvelope`,
`buildProfileEnvelope`, `parseEnvelope`), including the `schema`-derivation rule, tested. §3 two
verbs → Task 5 (`importTransfer` branching on `isConfig`) and Task 6 (two export menu items);
fresh-fetch-for-export → Task 4; `pos` preservation → Task 5, asserted in the commit message and
enforced by simply never touching `pos`. §4 import safety → Task 3 (validation), Task 5
(snapshot/undo), plus the device's existing atomic write. §5 cross-device warning → Task 6, with
`countMacrosAbovePos15` tested in Task 3. §6 verification → Task 8, and the APK-rebuild trap gets
Task 7 to itself. No gaps.

**2. Placeholder scan.** No TBD/TODO, and no deferred lookups: every value the implementer needs is
resolved in the plan. The package is `companion_app` (`pubspec.yaml:1`), so the test import is
exact rather than "check and correct". `dart:convert` is already imported in
`draupnir_state.dart:2`, so Task 5 states it instead of asking. The dashboard's `AppBar` already
owns a `PopupMenuButton`, so Task 6 extends it rather than adding a second `⋮`. That widget has no
`initState`, so Task 6 adds one — and says why that does not contradict the existing
"no auto-connect in initState" comment sitting a few lines away.

**3. Type consistency.** `ParsedTransfer` / `TransferException` / `kMaxSchema` are defined once in
Task 3 and used with those exact names in Tasks 5 and 6. `parsed.isConfig` is a getter on
`ParsedTransfer`, defined in Task 3 and used in both later tasks. `fetchProfilesForExport()`
returns `Map<String, dynamic>?` in Task 4 and is consumed as nullable in Task 6.
`importTransfer(ParsedTransfer)` and `undoImport()` return `Future<bool>` in Task 5 and are awaited
as such in Task 6. `hasImportSnapshot` gates the Undo menu item. The firmware's `include_icons`
wire name is identical in Tasks 1, 2 and 4.

**One deviation from the skill's default, stated plainly:** the two firmware tasks have no
failing-test step, because this repo has no host test framework for Arduino code and no CI. Their
gate is `arduino-cli compile` plus Task 8's hardware round. The Dart tasks do **not** get that
exemption — `flutter_test` exists and the baseline is green, so Task 3 is real TDD with tests
written first. Tasks 4-6 are wiring against a device and a file picker; they are covered by the
analyzer, by Task 3's tests continuing to pass, and by Task 8.
