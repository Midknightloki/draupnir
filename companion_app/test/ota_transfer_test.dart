import 'dart:typed_data';
import 'package:flutter_test/flutter_test.dart';
import 'package:companion_app/services/ota_transfer.dart';

void main() {
  group('chunksFor', () {
    test('splits an image into MTU-sized chunks', () {
      final img = Uint8List(1200);
      final chunks = chunksFor(img, 509, from: 0).toList();
      expect(chunks.length, 3);           // 509 + 509 + 182
      expect(chunks[0].length, 509);
      expect(chunks[2].length, 182);
    });

    test('resumes from an offset', () {
      final img = Uint8List(1200);
      final chunks = chunksFor(img, 509, from: 509).toList();
      expect(chunks.fold<int>(0, (a, c) => a + c.length), 1200 - 509);
    });

    test('an offset at or past the end yields nothing', () {
      expect(chunksFor(Uint8List(100), 509, from: 100).toList(), isEmpty);
      expect(chunksFor(Uint8List(100), 509, from: 200).toList(), isEmpty);
    });
  });

  group('shouldOffer', () {
    test('offers a newer version', () {
      expect(shouldOffer(deviceVersion: '1.0.0', bundleVersion: '1.0.1'), isTrue);
    });

    test('does not offer the same version', () {
      expect(shouldOffer(deviceVersion: '1.0.1', bundleVersion: '1.0.1'), isFalse);
    });

    test('offers a downgrade, which the UI warns about', () {
      // Refusing would make a bad release recoverable only over USB -- the exact situation OTA
      // exists to avoid. See the M13 design 4.
      expect(shouldOffer(deviceVersion: '1.0.2', bundleVersion: '1.0.1'), isTrue);
    });

    test('offers when the device version is unknown', () {
      expect(shouldOffer(deviceVersion: null, bundleVersion: '1.0.1'), isTrue);
    });
  });
}
