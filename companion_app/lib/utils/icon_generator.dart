import 'dart:ui' as ui;
import 'package:flutter/material.dart';

/// Generates an XBM hex string for an 18x18 icon
Future<String> generateXbmHexForIcon(IconData iconData) async {
  final int width = 18;
  final int height = 18;

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
      fontSize: 14.0, // A bit smaller than 18 to fit
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
  // 18 pixels wide = 3 bytes per row
  // 18 rows = 54 bytes
  final bytes = byteData.buffer.asUint8List();
  
  List<int> xbmBytes = List.filled(54, 0);
  
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      // index in rawRgba array
      final idx = (y * width + x) * 4;
      final r = bytes[idx];
      
      // threshold: if red > 127 it's white, so bit=1, else bit=0
      if (r > 127) {
        final byteIdx = y * 3 + (x ~/ 8);
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
