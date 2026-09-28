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

    // The device's ack-detection branch treats ANY 2-byte write as a chunk ack, never as image
    // data -- so a real final chunk of exactly 2 bytes would be swallowed instead of flashed.
    // These pin chunksFor() to never producing one, and -- more importantly than the lengths --
    // that the rebalanced chunks still concatenate back to the exact original bytes. A
    // rebalancing bug that dropped or duplicated a byte would still look like a plausible chunk
    // sequence; only a content comparison catches it.
    test('a length whose remainder is 2 produces no 2-byte chunk, and round-trips exactly', () {
      // 509 * 2 + 2 = 1020 -- the unpatched algorithm would yield a final chunk of length 2.
      final img = _filled(1020);
      final chunks = chunksFor(img, 509, from: 0).toList();

      expect(chunks.any((c) => c.length == 2), isFalse);
      expect(chunks.fold<int>(0, (a, c) => a + c.length), img.length);
      expect(_concat(chunks), orderedEquals(img));
    });

    test('resuming into what would be a 2-byte tail also avoids it, and round-trips exactly', () {
      // Same 1020-byte image; resuming from 1018 leaves exactly 2 bytes with no earlier chunk in
      // this call to borrow a byte from -- the other branch of the fix.
      final img = _filled(1020);
      final chunks = chunksFor(img, 509, from: 1018).toList();

      expect(chunks.any((c) => c.length == 2), isFalse);
      expect(chunks.fold<int>(0, (a, c) => a + c.length), img.length - 1018);
      expect(_concat(chunks), orderedEquals(img.sublist(1018)));
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

/// A non-trivial fill (not all zero/same byte) so a dropped, duplicated, or reordered byte in a
/// rebalanced chunk sequence would show up as a content mismatch, not just a length mismatch.
Uint8List _filled(int length) => Uint8List.fromList(List.generate(length, (i) => i % 256));

Uint8List _concat(List<Uint8List> chunks) {
  final total = chunks.fold<int>(0, (a, c) => a + c.length);
  final out = Uint8List(total);
  var offset = 0;
  for (final c in chunks) {
    out.setRange(offset, offset + c.length, c);
    offset += c.length;
  }
  return out;
}
