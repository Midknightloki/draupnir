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
}
