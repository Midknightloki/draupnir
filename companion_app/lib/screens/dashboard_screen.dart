import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';
import 'package:google_fonts/google_fonts.dart';

import '../state/draupnir_state.dart';
import '../theme.dart';
import 'editor_panel.dart';

class DashboardScreen extends StatefulWidget {
  const DashboardScreen({super.key});

  @override
  State<DashboardScreen> createState() => _DashboardScreenState();
}

class _DashboardScreenState extends State<DashboardScreen> {
  int? _editingKeyIdx;

  // No auto-connect on launch: BLE is the only transport now (the Wi-Fi/HTTP path is gone with
  // the web UI), and a BLE scan is an explicit user action, not something to fire off in
  // initState.

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
                        onPressed: state.isLoading ? null : () => state.connectBluetooth(),
                      ),
                    ],
                  ),
                ),
              ),
            )
          else if (state.error != null)
            Expanded(
              child: Center(
                child: Column(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    const Icon(Icons.error_outline, color: Colors.red, size: 48),
                    const SizedBox(height: 16),
                    Text(state.error!, style: const TextStyle(color: Colors.red)),
                  ],
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
          const Text(
            'Pair "Draupnir" in your phone\'s Bluetooth settings first — the knob shows the PIN '
            'on its screen.',
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
                  : () => state.connectBluetooth(),
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
            child: GridView.builder(
              gridDelegate: SliverGridDelegateWithFixedCrossAxisCount(
                crossAxisCount: MediaQuery.of(context).orientation == Orientation.landscape ? 8 : 4,
                crossAxisSpacing: 12,
                mainAxisSpacing: 12,
              ),
              itemCount: 16,
              itemBuilder: (context, index) {
                final macro = state.currentMacros.firstWhere(
                  (m) => m['pos'] == index,
                  orElse: () => null,
                );

                final hasMacro = macro != null;
                final name = hasMacro ? (macro['name'] ?? 'Macro') : '';
                
                Color keyColor = AppTheme.surfaceHighlight;
                if (hasMacro && macro['color'] != null) {
                  try {
                    String hex = macro['color'].toString().replaceAll('#', '');
                    keyColor = Color(int.parse('FF$hex', radix: 16));
                  } catch (_) {}
                }

                final isEditingThis = state.isEditorMode && _editingKeyIdx == index;

                return Material(
                  color: keyColor.withOpacity(hasMacro ? 0.8 : 0.3),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(12),
                    side: BorderSide(
                      color: isEditingThis ? Colors.white : (hasMacro ? keyColor : Colors.transparent),
                      width: isEditingThis ? 4 : 2,
                    ),
                  ),
                  child: InkWell(
                    borderRadius: BorderRadius.circular(12),
                    onTap: () {
                      if (state.isEditorMode) {
                        setState(() => _editingKeyIdx = index);
                      } else if (hasMacro) {
                        state.triggerMacro(index);
                      }
                    },
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
            ),
          ),
        ],
      ),
    );
  }
}
