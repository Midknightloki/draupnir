import 'package:flutter_test/flutter_test.dart';
import 'package:companion_app/utils/macro_icons.dart';

// The icon name is stored inside a profile, on the device, and inside exported files. That
// makes this map a data contract, not a UI detail, and the tests below guard the two ways it
// has already been broken once each.
void main() {
  // Every name the app has ever offered in its picker. A macro saved with any of these may
  // still exist on someone's device or in an exported file, so dropping one gives that macro a
  // blank face -- the app renders a fallback, and the save path, which rasterises only names
  // it can resolve, sends icon_xbm: "". This list is append-only: add to it when icons are
  // added, never remove from it.
  const historicalNames = [
    'activity', 'airplay', 'alertCircle', 'aperture', 'archive', 'arrowUpCircle',
    'arrowRightCircle', 'arrowDownCircle', 'arrowLeftCircle', 'award', 'barChart', 'barChart2',
    'battery', 'batteryCharging', 'bell', 'bluetooth', 'book', 'bookmark', 'box', 'briefcase',
    'calendar', 'camera', 'cast', 'checkCircle', 'chevronRight', 'clipboard', 'clock', 'cloud',
    'code', 'command', 'compass', 'cpu', 'crosshair', 'database', 'edit', 'feather', 'file',
    'fileText', 'film', 'folder', 'globe', 'grid', 'hammer', 'hardDrive', 'hash', 'headphones',
    'heart', 'helpCircle', 'hexagon', 'home', 'image', 'inbox', 'info', 'key', 'layers',
    'layout', 'lifeBuoy', 'link', 'list', 'lock', 'map', 'maximize', 'messageCircle',
    'messageSquare', 'mic', 'monitor', 'moon', 'music', 'navigation', 'package', 'paperclip',
    'penTool', 'pieChart', 'play', 'playCircle', 'power', 'printer', 'radio', 'refreshCw',
    'save', 'scissors', 'search', 'send', 'server', 'settings', 'shield', 'shoppingBag',
    'shoppingCart', 'shuffle', 'skipBack', 'skipForward', 'sliders', 'smartphone', 'speaker',
    'star', 'sun', 'target', 'terminal', 'trash', 'trash2', 'tv', 'type', 'umbrella', 'unlock',
    'upload', 'user', 'video', 'volume', 'volume2', 'watch', 'wifi', 'zap',
    // Offered by the picker but never in the map, so it was saveable and unrenderable. Now an
    // alias, which is why this asserts through resolveIcon() rather than against the map.
    'text',
  ];

  group('the set never loses a name', () {
    test('every icon the app has ever offered still resolves', () {
      final unresolvable =
          historicalNames.where((n) => resolveIcon(n) == null).toList();
      expect(
        unresolvable,
        isEmpty,
        reason: 'These names exist in saved profiles and would render as a blank face. '
            'Restore them to macroIcons, or add a legacyIconAliases entry pointing at '
            'whatever replaced them. Removing is never correct.',
      );
    });
  });

  group('resolveIcon', () {
    test('resolves a direct name', () {
      expect(resolveIcon('play'), equals(macroIcons['play']));
    });

    test('follows a legacy alias to its replacement', () {
      expect(resolveIcon('text'), equals(macroIcons['type']));
    });

    test('returns null for an unknown name', () {
      expect(resolveIcon('definitely-not-an-icon'), isNull);
    });

    test('returns null rather than throwing on null', () {
      expect(resolveIcon(null), isNull);
    });

    test('every alias points at a name that exists', () {
      for (final entry in legacyIconAliases.entries) {
        expect(
          macroIcons.containsKey(entry.value),
          isTrue,
          reason: 'Alias ${entry.key} points at ${entry.value}, which is not in macroIcons.',
        );
      }
    });

    test('no alias shadows a real icon', () {
      for (final name in legacyIconAliases.keys) {
        expect(
          macroIcons.containsKey(name),
          isFalse,
          reason: '$name is both a real icon and an alias; the alias would never be reached.',
        );
      }
    });
  });

  group('picker categories', () {
    test('every categorised name exists in the map', () {
      for (final section in iconCategories) {
        for (final name in section.value) {
          expect(
            macroIcons.containsKey(name),
            isTrue,
            reason: '${section.key} lists "$name", which is not in macroIcons.',
          );
        }
      }
    });

    test('every icon in the map is reachable in exactly one category', () {
      final counts = <String, int>{};
      for (final section in iconCategories) {
        for (final name in section.value) {
          counts[name] = (counts[name] ?? 0) + 1;
        }
      }
      // Unreachable: in the map but filed nowhere, so it can never be picked. The derived
      // "Other" section is what prevents this, and this test is what proves it still works.
      expect(
        macroIcons.keys.where((k) => !counts.containsKey(k)).toList(),
        isEmpty,
        reason: 'Icons present in macroIcons but absent from every picker category.',
      );
      // Duplicated: appears twice in the picker, which is how "mic" used to show up.
      expect(
        counts.entries.where((e) => e.value > 1).map((e) => e.key).toList(),
        isEmpty,
        reason: 'Icons listed in more than one picker category.',
      );
    });

    test('no category is empty', () {
      for (final section in iconCategories) {
        expect(section.value, isNotEmpty, reason: '${section.key} has no icons.');
      }
    });
  });
}
