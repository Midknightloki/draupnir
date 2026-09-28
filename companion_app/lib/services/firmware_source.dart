import 'dart:convert';
import 'dart:typed_data';
import 'package:flutter/services.dart';

/// A firmware image and everything needed to prove it is genuine.
class FirmwareBundle {
  const FirmwareBundle({
    required this.version,
    required this.bytes,
    required this.sha256,
    required this.signature,
  });

  final String version;
  final Uint8List bytes;
  final String sha256;    // hex
  final String signature; // hex, DER ECDSA P-256
}

/// Where a firmware image comes from.
///
/// The seam that keeps a future download path from touching the device side at all: the knob is
/// never told where the bytes originated, so swapping this out is an app-side change only. See
/// the M13 design 2.1 -- and note that a URL implementation needs the INTERNET permission, which
/// the release manifest deliberately does not declare.
abstract class FirmwareSource {
  Future<FirmwareBundle?> load();
}

/// The image shipped inside the APK.
class BundledFirmwareSource implements FirmwareSource {
  const BundledFirmwareSource();

  @override
  Future<FirmwareBundle?> load() async {
    try {
      final manifest = jsonDecode(
          await rootBundle.loadString('assets/firmware/manifest.json')) as Map<String, dynamic>;
      final data = await rootBundle.load('assets/firmware/firmware.bin');
      final bytes = data.buffer.asUint8List();

      // The manifest's size is what the device is told to expect, so a mismatch here would make
      // every transfer fail at the far end with a confusing byte-count error.
      final declared = manifest['size'] as int?;
      if (declared != null && declared != bytes.length) return null;

      return FirmwareBundle(
        version: manifest['version'] as String,
        bytes: bytes,
        sha256: manifest['sha256'] as String,
        signature: manifest['sig'] as String,
      );
    } catch (_) {
      // No bundled firmware is a normal state for a development build, not an error.
      return null;
    }
  }
}
