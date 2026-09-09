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
