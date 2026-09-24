import 'dart:ui' as ui;
import 'package:flutter/material.dart';

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
      fontSize: size * 14.0 / 18.0,
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
