import 'dart:ui' as ui;
import 'package:flutter/material.dart';

/// How much of the bitmap's edge the glyph should occupy, by bitmap size.
///
/// These are two different consumers and they want different answers, so this is deliberately
/// not one ratio.
///
/// 18px keeps the original hand-tuned 14-in-18 (78%). That bitmap is what the retired M5Dial
/// draws and what the Waveshare's tier 3 upscales, and both have shipped looking like this --
/// changing it would alter how every existing macro renders on a board that can never be
/// updated.
///
/// 48px targets 46-in-48 (96%) to match tier 1. The compiled font's glyphs measure up to 46px
/// inside their 48px box, so a tier-2 bitmap drawn at 78% would sit visibly smaller than the
/// font glyph beside it on the same ring -- the ratio was hand-tuned for 18px and carried to
/// 48px unexamined (M12 whole-branch review, Minor).
///
/// This changes rendered appearance only. The payload is stride * height and is unaffected:
/// 18px stays exactly 108 hex chars and 48px exactly 576, which the firmware's decoders enforce.
double _glyphRatioFor(int size) => size >= 48 ? 46.0 / 48.0 : 14.0 / 18.0;

/// Generates an XBM hex string for a square 1bpp icon.
///
/// `size` is the bitmap edge in pixels. 18 is the interchange size every device understands and
/// the retired M5Dial requires; 48 is the Waveshare's tier-2 fallback, drawn 1:1 so it is sharper
/// than an upscaled 18. The glyph is laid out at roughly 78% of the bitmap edge, which is what
/// the original hand-tuned 14-in-18 worked out to.
Future<String> generateXbmHex(IconData iconData, {int size = 18}) async {
  final int width = size;
  final int height = size;
  final int stride = (width + 7) ~/ 8;

  final pictureRecorder = ui.PictureRecorder();
  final canvas = ui.Canvas(pictureRecorder);

  // Background must be black, icon white
  canvas.drawRect(
    Rect.fromLTWH(0, 0, width.toDouble(), height.toDouble()),
    Paint()..color = Colors.black,
  );

  final textPainter = TextPainter(textDirection: TextDirection.ltr);
  textPainter.text = TextSpan(
    text: String.fromCharCode(iconData.codePoint),
    style: TextStyle(
      fontSize: size * _glyphRatioFor(size),
      fontFamily: iconData.fontFamily,
      package: iconData.fontPackage,
      color: Colors.white,
    ),
  );
  textPainter.layout();

  // Center it
  final dx = (width - textPainter.width) / 2;
  final dy = (height - textPainter.height) / 2;
  textPainter.paint(canvas, Offset(dx, dy));

  final picture = pictureRecorder.endRecording();
  final image = await picture.toImage(width, height);
  final byteData = await image.toByteData(format: ui.ImageByteFormat.rawRgba);

  if (byteData == null) return "";

  // Convert to XBM byte array (LSB first)
  final bytes = byteData.buffer.asUint8List();

  final xbmBytes = List.filled(stride * height, 0);

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      // index in rawRgba array
      final idx = (y * width + x) * 4;
      final r = bytes[idx];

      // threshold: if red > 127 it's white, so bit=1, else bit=0
      if (r > 127) {
        final byteIdx = y * stride + (x ~/ 8);
        final bitIdx = x % 8;
        xbmBytes[byteIdx] |= (1 << bitIdx);
      }
    }
  }

  // convert to hex string
  String hexStr = "";
  for (int b in xbmBytes) {
    hexStr += b.toRadixString(16).padLeft(2, '0');
  }

  return hexStr;
}

/// Back-compatible wrapper: the 18x18 interchange bitmap every device understands.
Future<String> generateXbmHexForIcon(IconData iconData) =>
    generateXbmHex(iconData, size: 18);

/// Whether this macro needs an app-supplied 48x48 for the connected device.
///
/// True only when we KNOW the device's glyph list and the name is absent from it. A null list
/// means older firmware or the retired M5Dial, which cannot use icon_bmp48 at all -- sending it
/// would spend payload against an 8 KB receive buffer for nothing.
bool needsBmp48(String? iconName, Set<String>? deviceGlyphs) {
  if (iconName == null || iconName.isEmpty) return false;
  if (deviceGlyphs == null) return false;
  return !deviceGlyphs.contains(iconName);
}

/// The device refuses a message larger than its BLE receive buffer. BLE_RX_BUFFER_SIZE is 8192
/// on the Waveshare; this leaves headroom for the command wrapper and chunk framing. This is the
/// SAFE default for any device we cannot positively identify as something roomier: it also
/// covers pre-M12 Waveshare firmware, which has the same 8192-byte buffer but does not answer
/// `get_glyphs`.
const int kMaxDocumentBytes = 7500;

/// The M5Dial's BLE_RX_BUFFER_SIZE (firmware/M5_M6_config/M5_M6_config.ino:95) is 16384 -- double
/// the Waveshare's. Same headroom logic as [kMaxDocumentBytes], applied to the bigger buffer, so
/// a board that can never be updated to work around a Waveshare-sized limit isn't refused at 54%
/// of its own capacity (M12 whole-branch review, Important finding 3).
const int kMaxDocumentBytesM5Dial = 16384 - (8192 - kMaxDocumentBytes);

/// A specific, actionable message when the serialised document is too large, or null when it
/// fits.
///
/// The device already refuses oversized messages safely (M6 RX bounds) and the refusal surfaces
/// through the save-retry path, so this is not a safety net -- it is the difference between "the
/// device refused the save" and knowing WHY, which for a document full of gap icons is a thing
/// the user can actually act on by updating the firmware.
///
/// [maxBytes] defaults to the conservative Waveshare-or-unknown limit; callers that know they are
/// talking to a roomier device (see [kMaxDocumentBytesM5Dial]) pass that instead.
///
/// [hasGlyphList] should be `deviceGlyphs != null` at the call site: true only for a device that
/// answered `get_glyphs`, i.e. an M12 Waveshare. It gates which explanation the gap-icon wording
/// gives, because the "newer than the device's firmware" story is only true for that device --
/// on an M5Dial (or any device that didn't answer) there is no glyph list at all, and any
/// icon_bmp48 present arrived via import from a different device, not from stale firmware (M12
/// whole-branch review, Important finding 4).
String? oversizeWarning(int documentBytes, int gapIconCount,
    {int maxBytes = kMaxDocumentBytes, bool hasGlyphList = true}) {
  if (documentBytes <= maxBytes) return null;
  final kb = (documentBytes / 1024).toStringAsFixed(1);
  if (gapIconCount > 0 && hasGlyphList) {
    return 'This profile is too large to send ($kb KB). '
        '$gapIconCount of its icons are newer than the device\'s firmware, so the app has to '
        'send each one as an image. Updating the firmware, or choosing icons the device already '
        'has, will fix both the size and how sharp they look.';
  }
  if (gapIconCount > 0) {
    return 'This profile is too large to send ($kb KB). '
        '$gapIconCount of its icons were sent as full images (this device has no built-in icon '
        'set), which takes more space than the app can fit. Try removing a few macros or '
        'shortening their text actions.';
  }
  return 'This profile is too large to send ($kb KB). '
      'Try removing a few macros or shortening their text actions.';
}
