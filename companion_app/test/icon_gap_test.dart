import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:companion_app/utils/icon_generator.dart';

void main() {
  group('needsBmp48', () {
    test('false when the device has no glyph list', () {
      // Older firmware and the retired M5Dial. Sending a bitmap they cannot use wastes payload
      // against an 8 KB buffer for no benefit.
      expect(needsBmp48('play', null), isFalse);
    });

    test('false when the device already knows the name', () {
      expect(needsBmp48('play', {'play', 'pause'}), isFalse);
    });

    test('true when the device does not know the name', () {
      expect(needsBmp48('gamepad2', {'play', 'pause'}), isTrue);
    });

    test('false for a macro with no icon', () {
      // Tier 4 -- the wedge draws the macro's name. There is nothing to rasterise.
      expect(needsBmp48(null, {'play'}), isFalse);
      expect(needsBmp48('', {'play'}), isFalse);
    });
  });

  // These pin a wire contract with the firmware, not an implementation detail. The firmware's
  // icon_bmp48_decode rejects any string whose length isn't exactly ICON_BMP48_BYTES * 2 = 576,
  // and wedge_icon_decode rejects anything not exactly 108 -- both silently, with no error and no
  // log, so a wrong stride produces a plausible-looking hex string the device just never draws.
  // The length depends only on stride * height * 2 (stride = (size + 7) ~/ 8), not on whether the
  // glyph actually renders, so this holds even where the test environment can't resolve the
  // Lucide font.
  group('generateXbmHex wire-length contract', () {
    final hexPattern = RegExp(r'^[0-9a-f]+$');

    setUpAll(() {
      TestWidgetsFlutterBinding.ensureInitialized();
    });

    test('size 18 (the M5Dial / universal interchange bitmap) is exactly 108 hex chars', () async {
      // stride = (18 + 7) ~/ 8 = 3; buffer = 3 * 18 = 54 bytes = 108 hex chars.
      final hex = await generateXbmHex(Icons.play_arrow, size: 18);
      expect(hex.length, 108);
      expect(hex, matches(hexPattern));
    });

    test('size 48 (the Waveshare tier-2 fallback) is exactly 576 hex chars', () async {
      // stride = (48 + 7) ~/ 8 = 6; buffer = 6 * 48 = 288 bytes = 576 hex chars -- the exact
      // ICON_BMP48_BYTES * 2 the firmware's icon_bmp48_decode requires.
      final hex = await generateXbmHex(Icons.play_arrow, size: 48);
      expect(hex.length, 576);
      expect(hex, matches(hexPattern));
    });

    test('generateXbmHexForIcon (the wrapper every pre-existing caller uses) is still 108', () async {
      final hex = await generateXbmHexForIcon(Icons.play_arrow);
      expect(hex.length, 108);
      expect(hex, matches(hexPattern));
    });
  });
}
