import 'dart:typed_data';

/// Slices an image into MTU-sized pieces, optionally resuming from a byte offset.
///
/// Kept a pure function so the slicing and resume arithmetic are testable without a BLE stack --
/// an off-by-one here corrupts a firmware image, and the device would only report it as a hash
/// mismatch after a minute of transfer.
Iterable<Uint8List> chunksFor(Uint8List image, int chunkSize, {int from = 0}) sync* {
  if (chunkSize <= 0) return;
  final length = image.length;
  var offset = from;
  while (offset < length) {
    var end = (offset + chunkSize < length) ? offset + chunkSize : length;

    // The device's ack-detection branch runs BEFORE its OTA-byte intercept and treats ANY
    // 2-byte write beginning with the chunk-ack marker as an ack, never as image data
    // (RxCallbacks::onWrite checks BLE_CHUNK_ACK_MARKER first and returns). That ordering has to
    // exist -- the device's own progress notifications go out through the same chunked
    // notify-and-wait-ack path, so the app must be able to ack during binary mode too -- but it
    // means a genuine final image chunk of exactly 2 bytes is indistinguishable on the wire from
    // an ack, and would be silently swallowed instead of written to flash. That happens whenever
    // the image length leaves a remainder of exactly 2 (1 in ~130,000 for a random size), and it
    // would fail as a hash mismatch after a full minute of transfer with nothing pointing at the
    // cause. Never emit that 2-byte chunk: if it would be the tail after this one, borrow a byte
    // so the tail becomes 3; if THIS chunk is itself the final 2 bytes, shrink it to 1 and let
    // the last byte fall out as its own 1-byte chunk on the next loop iteration. Either way, no
    // chunk this function yields is ever exactly 2 bytes long.
    final wouldLeaveTwoByteTail = end < length && (length - end) == 2;
    final isFinalTwoByteChunk = end == length && (end - offset) == 2;
    if (wouldLeaveTwoByteTail || isFinalTwoByteChunk) {
      end -= 1;
    }

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
