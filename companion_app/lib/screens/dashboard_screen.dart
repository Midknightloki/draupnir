import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:file_selector/file_selector.dart';
import 'package:share_plus/share_plus.dart';
import 'package:provider/provider.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../state/draupnir_state.dart';
import '../services/profile_transfer.dart';
import '../theme.dart';
import 'editor_panel.dart';

class DashboardScreen extends StatefulWidget {
  const DashboardScreen({super.key});

  @override
  State<DashboardScreen> createState() => _DashboardScreenState();
}

class _DashboardScreenState extends State<DashboardScreen> {
  int? _editingKeyIdx;

  // Every connect goes through here so the picker is always attached. The state layer only asks
  // when the scan finds more than one board — with a single Draupnir on the air this is exactly
  // the old behaviour, no extra tap.
  Future<void> _connect(DraupnirState state) =>
      state.connectBluetooth(chooseDevice: _pickDevice);

  // Two boards, two names ("Draupnir" = Waveshare knob, "Draupnir_Mini" = M5Dial), but the app
  // matches on the name loosely and must not care which is which — so this shows what was
  // actually advertised rather than mapping names to board models. Dismissing returns null,
  // which cancels the connect quietly.
  Future<BluetoothDevice?> _pickDevice(List<ScanResult> candidates) {
    return showDialog<BluetoothDevice>(
      context: context,
      builder: (ctx) => SimpleDialog(
        backgroundColor: AppTheme.surface,
        title: const Text('Which Draupnir?',
            style: TextStyle(color: Colors.white, fontSize: 18)),
        children: [
          for (final r in candidates)
            SimpleDialogOption(
              onPressed: () => Navigator.pop(ctx, r.device),
              child: ListTile(
                contentPadding: EdgeInsets.zero,
                leading: const Icon(Icons.bluetooth, color: AppTheme.accent),
                title: Text(
                  r.device.platformName.isEmpty ? '(unnamed)' : r.device.platformName,
                  style: const TextStyle(color: Colors.white),
                ),
                subtitle: Text(
                  '${r.device.remoteId}  ·  ${r.rssi} dBm',
                  style: const TextStyle(color: Colors.white54, fontSize: 12),
                ),
              ),
            ),
          SimpleDialogOption(
            onPressed: () => Navigator.pop(ctx, null),
            child: const Text('CANCEL', style: TextStyle(color: Colors.white54)),
          ),
        ],
      ),
    );
  }

  Future<void> _exportConfig(DraupnirState state) async {
    final doc = await state.fetchProfilesForExport();
    if (doc == null) return; // state.error is set and already rendered
    await _writeEnvelope(buildConfigEnvelope(doc), 'draupnir-config');
  }

  Future<void> _exportProfile(DraupnirState state, int profileIdx) async {
    final doc = await state.fetchProfilesForExport();
    if (doc == null) return;
    final profiles = doc['profiles'] as List;
    if (profileIdx < 0 || profileIdx >= profiles.length) return;
    final profile = Map<String, dynamic>.from(profiles[profileIdx] as Map);
    final safeName = profile['name']
        .toString()
        .replaceAll(RegExp(r'[^A-Za-z0-9._-]+'), '-')
        .toLowerCase();
    await _writeEnvelope(
        buildProfileEnvelope(profile, doc), 'draupnir-profile-$safeName');
  }

  /// Writes the envelope to a temp file and hands it to the OS share sheet.
  ///
  /// NOT a save dialog: file_selector's getSaveLocation is unimplemented on Android. The Android
  /// plugin (file_selector_android 0.5.2+8) overrides only openFile, openFiles and
  /// getDirectoryPath, so getSaveLocation falls through to the platform interface default, which
  /// throws UnimplementedError. Import still uses openFile, which IS implemented -- that
  /// asymmetry is why the import half worked while export silently did nothing.
  ///
  /// The share sheet is also the better fit: "share a profile with someone else" is one of the
  /// three purposes this feature exists for, and it lets the user put the file in Drive, Files,
  /// email or Nearby Share without us picking a directory for them.
  ///
  /// Directory.systemTemp is the app's own cache dir on Android, so no path_provider is needed
  /// and the file is cleaned up by the OS. share_plus copies it out via a FileProvider.
  Future<void> _writeEnvelope(
      Map<String, dynamic> envelope, String suggestedName) async {
    try {
      final bytes =
          utf8.encode(const JsonEncoder.withIndent('  ').convert(envelope));
      final file = File('${Directory.systemTemp.path}/$suggestedName.json');
      await file.writeAsBytes(bytes, flush: true);

      final result = await SharePlus.instance.share(ShareParams(
        files: [XFile(file.path, mimeType: 'application/json')],
        subject: '$suggestedName.json',
      ));

      if (!mounted) return;
      if (result.status == ShareResultStatus.success) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Export shared.')),
        );
      }
      // dismissed/unavailable need no message -- the user backed out deliberately.
    } catch (e) {
      // Never fail silently. The first version of this threw UnimplementedError into an async
      // gap with no UI at all, which presented as "I tapped Export and nothing happened".
      if (!mounted) return;
      await _alert('Export failed', '$e');
    }
  }

  Future<void> _importFile(DraupnirState state) async {
    ParsedTransfer parsed;
    try {
      final XFile? file = await openFile(
        acceptedTypeGroups: const [
          XTypeGroup(label: 'Draupnir export', extensions: ['json'])
        ],
      );
      if (file == null) return; // user cancelled
      parsed = parseEnvelope(utf8.decode(await file.readAsBytes()));
    } on TransferException catch (e) {
      if (!mounted) return;
      await _alert('Can\'t import that file', e.message);
      return;
    } catch (e) {
      // Picker or read failure, as opposed to a bad file. Same rule as export: never fail
      // silently into an async gap.
      if (!mounted) return;
      await _alert('Couldn\'t open that file', '$e');
      return;
    }

    final above = countMacrosAbovePos15(parsed);
    final isConfig = parsed.isConfig;
    final buffer = StringBuffer();
    if (isConfig) {
      final n = (parsed.payload['profiles'] as List).length;
      buffer.writeln(
          'This replaces everything on your Draupnir with $n profile(s) from this file.');
    } else {
      buffer.writeln(
          'This adds "${parsed.payload['name']}" as a new profile. Nothing is overwritten.');
    }
    if (above > 0) {
      buffer.writeln();
      // Phrased as a property of the profile, not of the device: the app can only identify a
      // board by its advertised name, and the spec is explicit that the name is a label for
      // humans, not a protocol constant.
      buffer.writeln(
          '$above macro(s) sit above ring position 15. They transfer and fire normally, '
          'but will not appear on the M5Dial\'s 16-dot ring.');
    }
    buffer.writeln();
    buffer.write('You can undo this straight afterwards.');

    if (!mounted) return;
    final go = await _confirm(
        isConfig ? 'Replace everything?' : 'Add this profile?',
        buffer.toString());
    if (go != true) return;

    final ok = await state.importTransfer(parsed);
    if (!mounted) return;
    if (ok) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: const Text('Import complete.'),
          action: SnackBarAction(
            label: 'UNDO',
            onPressed: () => state.undoImport(),
          ),
          duration: const Duration(seconds: 8),
        ),
      );
    }
  }

  Future<void> _alert(String title, String body) => showDialog<void>(
        context: context,
        builder: (c) => AlertDialog(
          title: Text(title),
          content: Text(body),
          actions: [
            TextButton(
                onPressed: () => Navigator.pop(c), child: const Text('OK'))
          ],
        ),
      );

  Future<bool?> _confirm(String title, String body) => showDialog<bool>(
        context: context,
        builder: (c) => AlertDialog(
          title: Text(title),
          content: Text(body),
          actions: [
            TextButton(
                onPressed: () => Navigator.pop(c, false),
                child: const Text('CANCEL')),
            TextButton(
                onPressed: () => Navigator.pop(c, true),
                child: const Text('CONTINUE')),
          ],
        ),
      );

  // No auto-connect on launch: BLE is the only transport now (the Wi-Fi/HTTP path is gone with
  // the web UI), and a BLE scan is an explicit user action, not something to fire off in
  // initState.

  @override
  void initState() {
    super.initState();
    // Local prefs read only — deliberately NOT a connection attempt. The comment above about
    // not auto-connecting in initState still stands; this starts no scan and touches no radio.
    // It only decides whether "Undo Last Import" appears in the menu after an app restart.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      context.read<DraupnirState>().loadImportSnapshotState();
    });
  }

  @override
  Widget build(BuildContext context) {
    final state = context.watch<DraupnirState>();

    return Scaffold(
      appBar: AppBar(
        title: Text(
          state.isEditorMode ? 'CONFIGURATOR' : 'VIRTUAL DECK',
          style: GoogleFonts.orbitron(fontWeight: FontWeight.bold),
        ),
        actions: [
          if (state.profilesData != null && state.isBluetooth)
            IconButton(
              icon: const Icon(Icons.bluetooth_connected, color: Colors.blue),
              tooltip: 'Connected via BLE. Tap to disconnect.',
              onPressed: () => state.disconnectBluetooth(),
            ),
          PopupMenuButton<String>(
            icon: Badge(
              isLabelVisible: state.debugLog.isNotEmpty,
              label: Text('${state.debugLog.length}'),
              child: const Icon(Icons.more_vert),
            ),
            onSelected: (val) {
              switch (val) {
                case 'refresh':
                  state.fetchProfiles();
                  break;
                case 'settings':
                  _showSettingsDialog(state);
                  break;
                case 'debug':
                  _showDebugLog(state);
                  break;
                case 'export_config':
                  _exportConfig(state);
                  break;
                case 'export_profile':
                  _exportProfile(state, state.activeProfileIdx);
                  break;
                case 'import':
                  _importFile(state);
                  break;
                case 'undo_import':
                  state.undoImport();
                  break;
              }
            },
            itemBuilder: (context) => [
              if (state.profilesData != null)
                const PopupMenuItem(
                  value: 'refresh',
                  child: ListTile(leading: Icon(Icons.refresh), title: Text('Refresh Profiles')),
                ),
              const PopupMenuItem(
                value: 'settings',
                child: ListTile(leading: Icon(Icons.settings), title: Text('Device Settings')),
              ),
              PopupMenuItem(
                value: 'debug',
                child: ListTile(
                  leading: const Icon(Icons.bug_report_outlined),
                  title: Text('Debug Log (${state.debugLog.length})'),
                ),
              ),
              const PopupMenuDivider(),
              // Export performs a fresh fetch and import ends in a save, so both are meaningless
              // without a live link. Undo is gated only on a snapshot existing, so it survives a
              // reconnect.
              if (state.profilesData != null && state.isBluetooth)
                const PopupMenuItem(
                  value: 'export_config',
                  child: ListTile(
                      leading: Icon(Icons.backup_outlined),
                      title: Text('Export All Profiles')),
                ),
              if (state.profilesData != null && state.isBluetooth)
                const PopupMenuItem(
                  value: 'export_profile',
                  child: ListTile(
                      leading: Icon(Icons.ios_share),
                      title: Text('Export This Profile')),
                ),
              if (state.profilesData != null && state.isBluetooth)
                const PopupMenuItem(
                  value: 'import',
                  child: ListTile(
                      leading: Icon(Icons.file_open_outlined),
                      title: Text('Import From File')),
                ),
              if (state.hasImportSnapshot)
                const PopupMenuItem(
                  value: 'undo_import',
                  child: ListTile(
                      leading: Icon(Icons.undo),
                      title: Text('Undo Last Import')),
                ),
            ],
          ),
          const SizedBox(width: 8),
        ],
      ),
      floatingActionButton: state.profilesData != null
          ? FloatingActionButton.extended(
              onPressed: () => state.toggleEditorMode(),
              backgroundColor: state.isEditorMode ? AppTheme.surfaceHighlight : AppTheme.accent,
              foregroundColor: state.isEditorMode ? Colors.white : Colors.black,
              icon: Icon(state.isEditorMode ? Icons.edit : Icons.play_arrow),
              label: Text(
                state.isEditorMode ? 'EDIT MODE' : 'RUN MODE',
                style: const TextStyle(fontWeight: FontWeight.bold),
              ),
            )
          : null,
      body: Column(
        children: [
          if (state.profilesData != null)
            SizedBox(height: 48, child: _buildProfileTabs(state)),
          if (state.profilesData == null && !state.isLoading) _buildConnectionBar(state),
          if (state.isLoading)
            const Expanded(child: Center(child: CircularProgressIndicator()))
          // Pairing is the OS's job now — standard BLE passkey pairing against the PIN shown on
          // the knob — so this is guidance, not an action. There is deliberately no in-app
          // "Pair" button: the old one drove a `cmd: "pair"` the firmware never implemented.
          else if (state.needsPairing)
            Expanded(
              child: Center(
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 32),
                  child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      const Icon(Icons.bluetooth_searching, color: Colors.orange, size: 64),
                      const SizedBox(height: 16),
                      const Text(
                        'PAIRING REQUIRED',
                        style: TextStyle(color: Colors.orange, fontSize: 16, fontWeight: FontWeight.bold),
                        textAlign: TextAlign.center,
                      ),
                      const SizedBox(height: 12),
                      Text(
                        state.error ?? DraupnirState.pairingRequiredMessage,
                        style: const TextStyle(color: Colors.white70, fontSize: 14),
                        textAlign: TextAlign.center,
                      ),
                      const SizedBox(height: 24),
                      ElevatedButton.icon(
                        icon: const Icon(Icons.refresh),
                        label: const Text('TRY AGAIN'),
                        style: ElevatedButton.styleFrom(
                          padding: const EdgeInsets.symmetric(horizontal: 32, vertical: 16),
                          backgroundColor: Colors.orange,
                          foregroundColor: Colors.white,
                        ),
                        onPressed: state.isLoading ? null : () => _connect(state),
                      ),
                    ],
                  ),
                ),
              ),
            )
          // The link is fine here -- the device answered, it just refused. Distinguishing this
          // from a connection failure matters: the fix is a gesture on the dial, not anything
          // to do with Bluetooth.
          else if (state.needsConfigMode)
            Expanded(
              child: Center(
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 32),
                  child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      const Icon(Icons.swipe_down, color: Colors.amber, size: 64),
                      const SizedBox(height: 16),
                      const Text(
                        'CONFIG MODE REQUIRED',
                        style: TextStyle(color: Colors.amber, fontSize: 16, fontWeight: FontWeight.bold),
                        textAlign: TextAlign.center,
                      ),
                      const SizedBox(height: 12),
                      Text(
                        state.error ?? DraupnirState.configModeRequiredMessage,
                        style: const TextStyle(color: Colors.white70, fontSize: 14),
                        textAlign: TextAlign.center,
                      ),
                      const SizedBox(height: 24),
                      // Retry the request, not the connection -- the BLE link is still up, so
                      // there is nothing to rescan or rediscover.
                      ElevatedButton.icon(
                        icon: const Icon(Icons.refresh),
                        label: const Text('TRY AGAIN'),
                        style: ElevatedButton.styleFrom(
                          padding: const EdgeInsets.symmetric(horizontal: 32, vertical: 16),
                          backgroundColor: Colors.amber,
                          foregroundColor: Colors.black,
                        ),
                        onPressed: state.isLoading
                            ? null
                            : () => state.isBluetooth ? state.fetchProfiles() : _connect(state),
                      ),
                    ],
                  ),
                ),
              ),
            )
          else if (state.error != null)
            Expanded(
              child: Center(
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 32),
                  child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      const Icon(Icons.error_outline, color: Colors.red, size: 48),
                      const SizedBox(height: 16),
                      Text(
                        state.error!,
                        style: const TextStyle(color: Colors.red),
                        textAlign: TextAlign.center,
                      ),
                      const SizedBox(height: 24),
                      // Without this the app was a dead end on any disconnect: the error was
                      // displayed with no action attached, so the only way back to a working
                      // connection was force-closing and reopening the app. Retrying in place is
                      // safe — the disconnect handler already nulls connectedDevice/rxChar/txChar,
                      // and connectBluetooth() clears `error`, rescans, and rebuilds all of that
                      // characteristic state from scratch.
                      ElevatedButton.icon(
                        icon: const Icon(Icons.bluetooth_searching),
                        label: const Text('RECONNECT'),
                        style: ElevatedButton.styleFrom(
                          padding: const EdgeInsets.symmetric(horizontal: 32, vertical: 16),
                        ),
                        onPressed: state.isLoading ? null : () => _connect(state),
                      ),
                    ],
                  ),
                ),
              ),
            )
          else
            Expanded(
              child: Row(
                children: [
                  Expanded(child: _buildVirtualDeck(state)),
                  if (state.isEditorMode && _editingKeyIdx != null)
                    EditorPanel(
                      position: _editingKeyIdx!,
                      onClose: () => setState(() => _editingKeyIdx = null),
                    ),
                ],
              ),
            ),
        ],
      ),
    );
  }

  Widget _buildProfileTabs(DraupnirState state) {
    final profiles = state.profilesData!['profiles'] as List;
    return ColoredBox(
      color: AppTheme.surface,
      child: Row(
        children: [
          Expanded(
            child: ListView.builder(
              scrollDirection: Axis.horizontal,
              padding: const EdgeInsets.symmetric(horizontal: 8),
              itemCount: profiles.length,
              itemBuilder: (context, idx) {
                Color profColor = AppTheme.accent;
                if (profiles[idx]['color'] != null) {
                  try {
                    String hex = profiles[idx]['color'].toString().replaceAll('#', '');
                    profColor = Color(int.parse('FF$hex', radix: 16));
                  } catch (_) {}
                }
                final selected = state.activeProfileIdx == idx;
                return Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 6),
                  child: ChoiceChip(
                    label: Text(
                      profiles[idx]['name'] ?? 'Profile $idx',
                      style: GoogleFonts.orbitron(
                        fontWeight: FontWeight.bold,
                        color: selected ? Colors.black : profColor,
                      ),
                    ),
                    selected: selected,
                    selectedColor: profColor,
                    backgroundColor: AppTheme.surfaceHighlight,
                    onSelected: (_) {
                      state.setActiveProfile(idx);
                      setState(() => _editingKeyIdx = null);
                    },
                  ),
                );
              },
            ),
          ),
          IconButton(
            icon: const Icon(Icons.add_circle_outline, color: AppTheme.accent, size: 20),
            tooltip: 'New profile',
            onPressed: () => _showAddProfileDialog(state),
          ),
        ],
      ),
    );
  }

  void _showAddProfileDialog(DraupnirState state) {
    final ctrl = TextEditingController();
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('New Profile'),
        content: TextField(
          controller: ctrl,
          decoration: const InputDecoration(hintText: 'Profile Name'),
          autofocus: true,
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('CANCEL')),
          ElevatedButton(
            onPressed: () {
              if (ctrl.text.isNotEmpty) {
                state.addProfile(ctrl.text);
              }
              Navigator.pop(ctx);
            },
            child: const Text('CREATE'),
          ),
        ],
      ),
    );
  }

  void _showEditProfileDialog(DraupnirState state) {
    if (state.profilesData == null) return;
    final profiles = state.profilesData!['profiles'] as List;
    final prof = profiles[state.activeProfileIdx];
    final ctrl = TextEditingController(text: prof['name'] ?? '');
    
    Color currentColor = AppTheme.accent;
    if (prof['color'] != null) {
      try {
        String hex = prof['color'].toString().replaceAll('#', '');
        currentColor = Color(int.parse('FF$hex', radix: 16));
      } catch (_) {}
    }

    showDialog(
      context: context,
      builder: (ctx) {
        return StatefulBuilder(
          builder: (context, setDialogState) {
            return AlertDialog(
              title: const Text('Edit Profile'),
              content: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  TextField(
                    controller: ctrl,
                    decoration: const InputDecoration(labelText: 'Profile Name'),
                  ),
                  const SizedBox(height: 16),
                  const Text('Profile Color', style: TextStyle(fontWeight: FontWeight.bold)),
                  const SizedBox(height: 8),
                  Wrap(
                    spacing: 8,
                    runSpacing: 8,
                    children: AppTheme.cyberpunkPalette.map((color) {
                      final isSelected = currentColor == color;
                      return InkWell(
                        onTap: () => setDialogState(() => currentColor = color),
                        child: Container(
                          width: 32,
                          height: 32,
                          decoration: BoxDecoration(
                            color: color,
                            shape: BoxShape.circle,
                            border: Border.all(
                              color: isSelected ? Colors.white : Colors.transparent,
                              width: 3,
                            ),
                          ),
                        ),
                      );
                    }).toList(),
                  ),
                ],
              ),
              actionsAlignment: MainAxisAlignment.spaceBetween,
              actions: [
                if (profiles.length > 1)
                  TextButton(
                    onPressed: () {
                      state.deleteProfile();
                      Navigator.pop(ctx);
                    },
                    child: const Text('DELETE', style: TextStyle(color: Colors.red)),
                  )
                else
                  const SizedBox.shrink(),
                Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('CANCEL')),
                    const SizedBox(width: 8),
                    ElevatedButton(
                      onPressed: () {
                        if (ctrl.text.isNotEmpty) {
                          state.renameProfile(ctrl.text);
                          String colorHex = currentColor.value.toRadixString(16).padLeft(8, '0').substring(2).toUpperCase();
                          state.setProfileColor('#$colorHex');
                        }
                        Navigator.pop(ctx);
                      },
                      child: const Text('SAVE'),
                    ),
                  ],
                ),
              ],
            );
          }
        );
      },
    );
  }

  void _showSettingsDialog(DraupnirState state) {
    showDialog(
      context: context,
      builder: (ctx) {
        int currentOrientation = state.orientation;
        return StatefulBuilder(
          builder: (context, setDialogState) {
            return AlertDialog(
              title: const Text('Device Settings'),
              content: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    state.isBluetooth ? 'Connected via Bluetooth (BLE)' : 'Not connected',
                    style: TextStyle(
                      color: state.isBluetooth ? Colors.green : Colors.grey,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  const SizedBox(height: 16),
                  const Text('Dial Orientation', style: TextStyle(fontWeight: FontWeight.bold)),
                  DropdownButton<int>(
                    isExpanded: true,
                    value: currentOrientation,
                    items: const [
                      DropdownMenuItem(value: 0, child: Text('0° (Normal)')),
                      DropdownMenuItem(value: 1, child: Text('90° (Clockwise)')),
                      DropdownMenuItem(value: 2, child: Text('180° (Upside Down)')),
                      DropdownMenuItem(value: 3, child: Text('270° (Counter-Clockwise)')),
                    ],
                    onChanged: (val) {
                      if (val != null) {
                        setDialogState(() => currentOrientation = val);
                      }
                    },
                  ),
                ],
              ),
              actions: [
                TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('CANCEL')),
                ElevatedButton(
                  onPressed: () {
                    if (state.orientation != currentOrientation) {
                      state.setOrientation(currentOrientation);
                    }
                    Navigator.pop(ctx);
                  },
                  child: const Text('SAVE'),
                ),
              ],
            );
          }
        );
      },
    );
  }

  void _showDebugLog(DraupnirState state) {
    showDialog(
      context: context,
      builder: (ctx) => StatefulBuilder(
        builder: (context, setDialogState) {
          return AlertDialog(
            title: const Text('BLE Debug Log'),
            content: SizedBox(
              width: double.maxFinite,
              height: 400,
              child: state.debugLog.isEmpty
                  ? const Center(child: Text('No log entries yet.\nConnect via Bluetooth to see events.', textAlign: TextAlign.center))
                  : ListView.builder(
                      itemCount: state.debugLog.length,
                      itemBuilder: (ctx, i) {
                        final line = state.debugLog[i];
                        Color color = Colors.white70;
                        if (line.contains('[ERR]')) color = Colors.red;
                        else if (line.contains('[RX]')) color = Colors.greenAccent;
                        else if (line.contains('[TX]')) color = Colors.cyanAccent;
                        else if (line.contains('[CONN]') || line.contains('[MTU]')) color = Colors.blueAccent;
                        return Text(line, style: TextStyle(fontFamily: 'monospace', fontSize: 11, color: color));
                      },
                    ),
            ),
            actions: [
              TextButton(
                onPressed: () { state.clearDebugLog(); setDialogState(() {}); },
                child: const Text('CLEAR'),
              ),
              TextButton(
                onPressed: () async {
                  final text = state.debugLog.join('\n');
                  await Clipboard.setData(ClipboardData(text: text));
                  if (ctx.mounted) {
                    ScaffoldMessenger.of(ctx).showSnackBar(
                      const SnackBar(content: Text('Debug log copied to clipboard'), duration: Duration(seconds: 2)),
                    );
                  }
                },
                child: const Text('COPY'),
              ),
              TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('CLOSE')),
            ],
          );
        },
      ),
    );
  }

  Widget _buildConnectionBar(DraupnirState state) {
    return Container(
      padding: const EdgeInsets.all(16),
      color: AppTheme.surface,
      // BLE is the only transport. The Wi-Fi/IP branch here was the client half of the
      // on-device web UI, which is cut permanently (spec v3 §1, §7).
      child: Column(
        children: [
          // Both boards enforce BLE bonding now, so this is one instruction rather than two.
          // It used to say the M5Dial needed no pairing; that stopped being true when the
          // M5Dial security gate landed.
          const Text(
            'Pair your Draupnir in your phone\'s Bluetooth settings first — "Draupnir" is the '
            'Waveshare knob, "Draupnir_Mini" is the M5Dial. Each shows a PIN on its own screen '
            'while pairing. Then come back and connect.',
            style: TextStyle(color: Colors.white54, fontSize: 12),
            textAlign: TextAlign.center,
          ),
          const SizedBox(height: 16),
          Center(
            child: ElevatedButton.icon(
              icon: state.isScanningBle
                  ? const SizedBox(
                      width: 20,
                      height: 20,
                      child: CircularProgressIndicator(
                        strokeWidth: 2,
                        color: Colors.black,
                      ),
                    )
                  : const Icon(Icons.bluetooth),
              label: Text(state.isScanningBle
                  ? 'SCANNING FOR DRAUPNIR...'
                  : 'CONNECT VIA BLUETOOTH'),
              style: ElevatedButton.styleFrom(
                padding: const EdgeInsets.symmetric(
                    horizontal: 32, vertical: 16),
                backgroundColor: AppTheme.accent,
                foregroundColor: Colors.black,
              ),
              onPressed: state.isScanningBle || state.isLoading
                  ? null
                  : () => _connect(state),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildVirtualDeck(DraupnirState state) {
    String profileName = "VIRTUAL DECK";
    Color profileColor = AppTheme.accent;
    if (state.profilesData != null) {
      final profiles = state.profilesData!['profiles'] as List;
      if (state.activeProfileIdx < profiles.length) {
        final prof = profiles[state.activeProfileIdx];
        profileName = prof['name'] ?? profileName;
        if (prof['color'] != null) {
          try {
            String hex = prof['color'].toString().replaceAll('#', '');
            profileColor = Color(int.parse('FF$hex', radix: 16));
          } catch (_) {}
        }
      }
    }

    return Padding(
      padding: const EdgeInsets.all(24.0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Row(
                crossAxisAlignment: CrossAxisAlignment.center,
                children: [
                  Text(
                    profileName,
                    style: GoogleFonts.orbitron(fontSize: 24, fontWeight: FontWeight.bold, color: profileColor),
                  ),
                  if (state.profilesData != null)
                    IconButton(
                      icon: const Icon(Icons.edit, color: Colors.grey, size: 20),
                      tooltip: 'Edit current profile',
                      onPressed: () => _showEditProfileDialog(state),
                    ),
                ],
              ),
              if (state.isEditorMode)
                Text(
                  'Select key to edit',
                  style: GoogleFonts.orbitron(fontSize: 12, color: Colors.grey),
                ),
            ],
          ),
          const SizedBox(height: 16),
          Expanded(
            child: Builder(builder: (context) {
              final macros = state.sortedMacros;
              final canAdd = macros.length < DraupnirState.maxMacros;
              return GridView.builder(
              gridDelegate: SliverGridDelegateWithFixedCrossAxisCount(
                crossAxisCount: MediaQuery.of(context).orientation == Orientation.landscape ? 8 : 4,
                crossAxisSpacing: 12,
                mainAxisSpacing: 12,
              ),
              // The deck renders WHAT EXISTS, plus one trailing "+" tile.
              //
              // It used to be a fixed 16 cells where the grid index WAS the pos, and empty cells
              // doubled as the creation affordance. That cannot survive an uncapped pos, and it
              // already misrepresented the device: the ring draws one wedge per existing macro
              // and skips gaps entirely, so macros at pos 0 and 9 are two ADJACENT wedges on the
              // knob but were two distant cells here.
              itemCount: macros.length + (canAdd ? 1 : 0),
              itemBuilder: (context, index) {
                // The trailing "+" tile.
                if (index >= macros.length) {
                  return Material(
                    color: AppTheme.surfaceHighlight.withOpacity(0.3),
                    shape: RoundedRectangleBorder(
                      borderRadius: BorderRadius.circular(12),
                      side: BorderSide(color: Colors.grey.shade700, width: 2),
                    ),
                    child: InkWell(
                      borderRadius: BorderRadius.circular(12),
                      onTap: () async {
                        final pos = state.lowestFreePos;
                        if (pos < 0) return;
                        await state.updateMacro(pos, {
                          'name': 'New Macro',
                          'color': '#30C060',
                          'mode': 'play_once',
                          'actions': [],
                        });
                        // Drop straight into the editor for the macro just created -- an empty
                        // macro the user has to go find and open is not a useful outcome.
                        if (context.mounted) {
                          setState(() => _editingKeyIdx = pos);
                        }
                      },
                      child: const Center(
                        child: Icon(Icons.add, color: Colors.grey, size: 32),
                      ),
                    ),
                  );
                }

                final macro = macros[index];
                // pos is the IDENTITY. It is no longer the grid index, and conflating the two
                // edits the wrong macro in any profile with a gap.
                final pos = (macro['pos'] ?? 0) as int;
                final name = macro['name'] ?? 'Macro';

                Color keyColor = AppTheme.surfaceHighlight;
                if (macro['color'] != null) {
                  try {
                    String hex = macro['color'].toString().replaceAll('#', '');
                    keyColor = Color(int.parse('FF$hex', radix: 16));
                  } catch (_) {}
                }

                final isEditingThis = state.isEditorMode && _editingKeyIdx == pos;

                return Material(
                  color: keyColor.withOpacity(0.8),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(12),
                    side: BorderSide(
                      color: isEditingThis ? Colors.white : keyColor,
                      width: isEditingThis ? 4 : 2,
                    ),
                  ),
                  child: InkWell(
                    borderRadius: BorderRadius.circular(12),
                    onTap: () {
                      if (state.isEditorMode) {
                        setState(() => _editingKeyIdx = pos);
                      } else {
                        state.triggerMacro(pos);
                      }
                    },
                    onLongPress: () => _confirmDeleteMacro(state, pos, name.toString()),
                    child: Center(
                      child: Text(
                        name,
                        textAlign: TextAlign.center,
                        style: GoogleFonts.orbitron(
                          fontSize: MediaQuery.of(context).orientation == Orientation.landscape ? 12 : 18,
                          fontWeight: FontWeight.bold,
                          color: keyColor.computeLuminance() > 0.5 ? Colors.black : Colors.white,
                        ),
                      ),
                    ),
                  ),
                );
              },
              );
            }),
          ),
        ],
      ),
    );
  }

  Future<void> _confirmDeleteMacro(DraupnirState state, int pos, String name) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: AppTheme.surface,
        title: Text('Delete "$name"?', style: GoogleFonts.orbitron(fontSize: 16)),
        content: Text(
          'This removes the macro from this profile. The device reflows its ring to close the gap.',
          style: GoogleFonts.orbitron(fontSize: 12, color: Colors.grey),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(ctx).pop(false),
            child: const Text('Cancel'),
          ),
          TextButton(
            onPressed: () => Navigator.of(ctx).pop(true),
            child: const Text('Delete', style: TextStyle(color: Colors.redAccent)),
          ),
        ],
      ),
    );
    if (ok != true) return;
    // Close the editor if it was open on the macro just deleted, or it would sit there editing
    // something that no longer exists.
    if (_editingKeyIdx == pos) setState(() => _editingKeyIdx = null);
    await state.deleteMacro(pos);
  }
}
