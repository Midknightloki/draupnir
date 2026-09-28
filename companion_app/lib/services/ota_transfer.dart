import 'dart:typed_data';

/// Slices an image into MTU-sized pieces, optionally resuming from a byte offset.
///
/// Kept a pure function so the slicing and resume arithmetic are testable without a BLE stack --
/// an off-by-one here corrupts a firmware image, and the device would only report it as a hash
/// mismatch after a minute of transfer.
Iterable<Uint8List> chunksFor(Uint8List image, int chunkSize, {int from = 0}) sync* {
  if (chunkSize <= 0) return;
  var offset = from;
  while (offset < image.length) {
    final end = (offset + chunkSize < image.length) ? offset + chunkSize : image.length;
    yield Uint8List.sublistView(image, offset, end);
    offset = end;
  }
}

/// Whether to offer the bundled image to a device reporting [deviceVersion].
///
/// Offers anything that is not an exact match, downgrades included. Refusing a downgrade would
/// make a bad release recoverable only over USB, which is the situation OTA exists to avoid; the
/// UI warns instead.
bool shouldOffer({required String? deviceVersion, required String bundleVersion}) {
  if (deviceVersion == null || deviceVersion.isEmpty) return true;
  return deviceVersion != bundleVersion;
}
