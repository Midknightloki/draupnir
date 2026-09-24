import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

// ===========================================================================================
// The icon vocabulary offered when binding a macro.
//
// Two rules govern this file.
//
// NOTHING IS EVER REMOVED. The key is what gets written into a profile's `icon` field and
// stored on the device, so deleting one gives every macro already using it a blank face. The
// set only grows; anything superseded becomes a [legacyIconAliases] entry instead.
//
// GLYPHS MUST SURVIVE 18x18. The device rasterises the chosen glyph to an 18x18 1bpp bitmap,
// so fine interior detail turns to noise on the knob even though it looks correct in the app.
// Prefer bold, open shapes. A handful below (the align family, listOrdered, the git verbs,
// braces, dices) are knowingly over-detailed: they are useful enough to bind that they earn
// their place, and they get sharper for free when the icon-rendering milestone raises the
// bitmap resolution.
// ===========================================================================================

/// Every selectable icon, by the name stored in the profile.
final Map<String, IconData> macroIcons = {
  // --- Media -----------------------------------------------------------------------------
  'play': LucideIcons.play,
  'playCircle': LucideIcons.playCircle,
  'pause': LucideIcons.pause,
  'stop': LucideIcons.square,
  'skipBack': LucideIcons.skipBack,
  'skipForward': LucideIcons.skipForward,
  'rewind': LucideIcons.rewind,
  'fastForward': LucideIcons.fastForward,
  'volume': LucideIcons.volume,
  'volume1': LucideIcons.volume1,
  'volume2': LucideIcons.volume2,
  'mute': LucideIcons.volumeX,
  'music': LucideIcons.music,
  'headphones': LucideIcons.headphones,
  'speaker': LucideIcons.speaker,
  'shuffle': LucideIcons.shuffle,
  'repeat': LucideIcons.repeat,

  // --- Meetings & streaming --------------------------------------------------------------
  'mic': LucideIcons.mic,
  'micOff': LucideIcons.micOff,
  'video': LucideIcons.video,
  'videoOff': LucideIcons.videoOff,
  'screenShare': LucideIcons.screenShare,
  'screenShareOff': LucideIcons.screenShareOff,
  'phone': LucideIcons.phone,
  'phoneOff': LucideIcons.phoneOff,
  'hand': LucideIcons.hand,
  'users': LucideIcons.users,
  'record': LucideIcons.circleDot,
  'monitorPlay': LucideIcons.monitorPlay,
  'webcam': LucideIcons.webcam,
  'cast': LucideIcons.cast,
  'camera': LucideIcons.camera,

  // --- Editing ---------------------------------------------------------------------------
  'copy': LucideIcons.copy,
  'paste': LucideIcons.clipboardPaste,
  'clipboard': LucideIcons.clipboard,
  'scissors': LucideIcons.scissors,
  'undo': LucideIcons.undo,
  'redo': LucideIcons.redo,
  'save': LucideIcons.save,
  'search': LucideIcons.search,
  'replace': LucideIcons.replace,
  'edit': LucideIcons.edit,
  'penTool': LucideIcons.penTool,
  'type': LucideIcons.type,
  'bold': LucideIcons.bold,
  'italic': LucideIcons.italic,
  'underline': LucideIcons.underline,
  'alignLeft': LucideIcons.alignLeft,
  'alignCenter': LucideIcons.alignCenter,
  'alignRight': LucideIcons.alignRight,
  'list': LucideIcons.list,
  'listOrdered': LucideIcons.listOrdered,
  'trash': LucideIcons.trash,
  'trash2': LucideIcons.trash2,

  // --- Window & system -------------------------------------------------------------------
  'maximize': LucideIcons.maximize,
  'minimize': LucideIcons.minimize,
  'close': LucideIcons.x,
  'columns': LucideIcons.columns2,
  'rows': LucideIcons.rows2,
  'monitor': LucideIcons.monitor,
  'lock': LucideIcons.lock,
  'unlock': LucideIcons.unlock,
  'power': LucideIcons.power,
  'settings': LucideIcons.settings,
  'sliders': LucideIcons.sliders,
  'refreshCw': LucideIcons.refreshCw,
  'printer': LucideIcons.printer,
  'zap': LucideIcons.zap,

  // --- Dev -------------------------------------------------------------------------------
  'code': LucideIcons.code,
  'terminal': LucideIcons.terminal,
  'squareTerminal': LucideIcons.squareTerminal,
  'gitBranch': LucideIcons.gitBranch,
  'gitCommit': LucideIcons.gitCommitHorizontal,
  'gitPullRequest': LucideIcons.gitPullRequest,
  'bug': LucideIcons.bug,
  'braces': LucideIcons.braces,
  'database': LucideIcons.database,
  'server': LucideIcons.server,
  'cloud': LucideIcons.cloud,
  'package': LucideIcons.package,
  'cpu': LucideIcons.cpu,
  'hardDrive': LucideIcons.hardDrive,

  // --- Gaming ----------------------------------------------------------------------------
  'gamepad2': LucideIcons.gamepad2,
  'swords': LucideIcons.swords,
  'target': LucideIcons.target,
  'crosshair': LucideIcons.crosshair,
  'shield': LucideIcons.shield,
  'dices': LucideIcons.dices,

  // --- Navigation ------------------------------------------------------------------------
  'arrowUpCircle': LucideIcons.arrowUpCircle,
  'arrowRightCircle': LucideIcons.arrowRightCircle,
  'arrowDownCircle': LucideIcons.arrowDownCircle,
  'arrowLeftCircle': LucideIcons.arrowLeftCircle,
  'chevronRight': LucideIcons.chevronRight,
  'home': LucideIcons.home,
  'compass': LucideIcons.compass,
  'map': LucideIcons.map,
  'navigation': LucideIcons.navigation,

  // --- Symbols ---------------------------------------------------------------------------
  'star': LucideIcons.star,
  'bookmark': LucideIcons.bookmark,
  'flag': LucideIcons.flag,
  'bell': LucideIcons.bell,
  'checkCircle': LucideIcons.checkCircle,
  'alertCircle': LucideIcons.alertCircle,
  'helpCircle': LucideIcons.helpCircle,
  'info': LucideIcons.info,
  'hash': LucideIcons.hash,
  'key': LucideIcons.key,
  'tag': LucideIcons.tag,
  'heart': LucideIcons.heart,

  // --- Other -----------------------------------------------------------------------------
  // Carried forward from the original set. Kept because profiles may already reference them
  // (see the no-removal rule above), not because they earn a place in a macro vocabulary.
  'activity': LucideIcons.activity,
  'airplay': LucideIcons.airplay,
  'aperture': LucideIcons.aperture,
  'archive': LucideIcons.archive,
  'award': LucideIcons.award,
  'barChart': LucideIcons.barChart,
  'barChart2': LucideIcons.barChart2,
  'battery': LucideIcons.battery,
  'batteryCharging': LucideIcons.batteryCharging,
  'bluetooth': LucideIcons.bluetooth,
  'book': LucideIcons.book,
  'box': LucideIcons.box,
  'briefcase': LucideIcons.briefcase,
  'calendar': LucideIcons.calendar,
  'clock': LucideIcons.clock,
  'command': LucideIcons.command,
  'feather': LucideIcons.feather,
  'file': LucideIcons.file,
  'fileText': LucideIcons.fileText,
  'film': LucideIcons.film,
  'folder': LucideIcons.folder,
  'globe': LucideIcons.globe,
  'grid': LucideIcons.grid,
  'hammer': LucideIcons.hammer,
  'hexagon': LucideIcons.hexagon,
  'image': LucideIcons.image,
  'inbox': LucideIcons.inbox,
  'layers': LucideIcons.layers,
  'layout': LucideIcons.layout,
  'lifeBuoy': LucideIcons.lifeBuoy,
  'link': LucideIcons.link,
  'messageCircle': LucideIcons.messageCircle,
  'messageSquare': LucideIcons.messageSquare,
  'moon': LucideIcons.moon,
  'paperclip': LucideIcons.paperclip,
  'pieChart': LucideIcons.pieChart,
  'radio': LucideIcons.radio,
  'send': LucideIcons.send,
  'shoppingBag': LucideIcons.shoppingBag,
  'shoppingCart': LucideIcons.shoppingCart,
  'smartphone': LucideIcons.smartphone,
  'sun': LucideIcons.sun,
  'tv': LucideIcons.tv,
  'umbrella': LucideIcons.umbrella,
  'upload': LucideIcons.upload,
  'user': LucideIcons.user,
  'watch': LucideIcons.watch,
  'wifi': LucideIcons.wifi,
};

/// Picker grouping. Names only -- the glyphs come from [macroIcons].
const Map<String, List<String>> _categoryNames = {
  'Media': ['play', 'playCircle', 'pause', 'stop', 'skipBack', 'skipForward', 'rewind',
            'fastForward', 'volume', 'volume1', 'volume2', 'mute', 'music', 'headphones',
            'speaker', 'shuffle', 'repeat'],
  'Meetings & Streaming': ['mic', 'micOff', 'video', 'videoOff', 'screenShare',
            'screenShareOff', 'phone', 'phoneOff', 'hand', 'users', 'record', 'monitorPlay',
            'webcam', 'cast', 'camera'],
  'Editing': ['copy', 'paste', 'clipboard', 'scissors', 'undo', 'redo', 'save', 'search',
            'replace', 'edit', 'penTool', 'type', 'bold', 'italic', 'underline', 'alignLeft',
            'alignCenter', 'alignRight', 'list', 'listOrdered', 'trash', 'trash2'],
  'Window & System': ['maximize', 'minimize', 'close', 'columns', 'rows', 'monitor', 'lock',
            'unlock', 'power', 'settings', 'sliders', 'refreshCw', 'printer', 'zap'],
  'Dev': ['code', 'terminal', 'squareTerminal', 'gitBranch', 'gitCommit', 'gitPullRequest',
            'bug', 'braces', 'database', 'server', 'cloud', 'package', 'cpu', 'hardDrive'],
  'Gaming': ['gamepad2', 'swords', 'target', 'crosshair', 'shield', 'dices'],
  'Navigation': ['arrowUpCircle', 'arrowRightCircle', 'arrowDownCircle', 'arrowLeftCircle',
            'chevronRight', 'home', 'compass', 'map', 'navigation'],
  'Symbols': ['star', 'bookmark', 'flag', 'bell', 'checkCircle', 'alertCircle', 'helpCircle',
            'info', 'hash', 'key', 'tag', 'heart'],
};

/// The picker's sections, in display order, with a trailing "Other" holding everything in
/// [macroIcons] that no category claims.
///
/// Other is DERIVED rather than listed, so an icon can never be added to the map and then be
/// invisible in the picker because someone forgot to file it.
final List<MapEntry<String, List<String>>> iconCategories = () {
  final claimed = _categoryNames.values.expand((v) => v).toSet();
  assert(
    claimed.every(macroIcons.containsKey),
    'Category lists a name absent from macroIcons',
  );
  final other = macroIcons.keys.where((k) => !claimed.contains(k)).toList();
  return [
    ..._categoryNames.entries,
    if (other.isNotEmpty) MapEntry('Other', other),
  ];
}();

/// Legacy `icon` values that predate the current set. Macros saved with one still render,
/// but they are not offered in the picker -- each points at the glyph that replaced it.
const Map<String, String> legacyIconAliases = {
  'text': 'type',
};

/// The glyph for an `icon` name, following [legacyIconAliases]; null if the name is unknown.
///
/// Every place that turns a name into a glyph goes through here. Three of them used to
/// special-case 'hammer', 'text' and 'mic' inline with Material icons, which drifted: the
/// editor preview drew Icons.build for 'hammer' while the save path, which rasterises only
/// names present in the map, sent no bitmap at all. Picking those two produced a macro that
/// looked fine in the app and had a blank face on the device.
IconData? resolveIcon(String? name) {
  if (name == null) return null;
  return macroIcons[name] ?? macroIcons[legacyIconAliases[name]];
}
