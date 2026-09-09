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
