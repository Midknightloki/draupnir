import 'package:flutter/material.dart';
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
  final TextEditingController _ipController = TextEditingController(text: 'draupnir.local');
  int? _editingKeyIdx;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      context.read<DraupnirState>().connect(_ipController.text);
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
          if (state.profilesData != null) ...[
            _buildProfileSwitcher(state),
            const SizedBox(width: 16),
            _buildModeToggle(state),
            const SizedBox(width: 16),
            IconButton(
              icon: const Icon(Icons.refresh),
              onPressed: () => state.fetchProfiles(),
            ),
            IconButton(
              icon: const Icon(Icons.settings),
              onPressed: () => _showSettingsDialog(state),
            ),
          ],
          const SizedBox(width: 16),
        ],
      ),
      body: Column(
        children: [
          if (state.profilesData == null && !state.isLoading) _buildConnectionBar(state),
          if (state.isLoading)
            const Expanded(child: Center(child: CircularProgressIndicator()))
          else if (state.needsPairing)
            Expanded(
              child: Center(
                child: Column(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    const Icon(Icons.lock_outline, color: Colors.orange, size: 64),
                    const SizedBox(height: 16),
                    Text(
                      state.error ?? 'Pairing Required',
                      style: const TextStyle(color: Colors.orange, fontSize: 16, fontWeight: FontWeight.bold),
                      textAlign: TextAlign.center,
                    ),
                    const SizedBox(height: 24),
                    ElevatedButton.icon(
                      icon: state.isPairing 
                        ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white))
                        : const Icon(Icons.link),
                      label: Text(state.isPairing ? 'PAIRING...' : 'PAIR DEVICE'),
                      style: ElevatedButton.styleFrom(
                        padding: const EdgeInsets.symmetric(horizontal: 32, vertical: 16),
                        backgroundColor: Colors.orange,
                        foregroundColor: Colors.white,
                      ),
                      onPressed: state.isPairing ? null : () => state.pair(),
                    ),
                  ],
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

  Widget _buildProfileSwitcher(DraupnirState state) {
    final profiles = state.profilesData!['profiles'] as List;
    return Row(
      children: [
        DropdownButton<int>(
          value: state.activeProfileIdx,
          dropdownColor: AppTheme.surfaceHighlight,
          underline: const SizedBox(),
          items: List.generate(profiles.length, (idx) {
            Color profColor = AppTheme.accent;
            if (profiles[idx]['color'] != null) {
              try {
                String hex = profiles[idx]['color'].toString().replaceAll('#', '');
                profColor = Color(int.parse('FF$hex', radix: 16));
              } catch (_) {}
            }
            return DropdownMenuItem(
              value: idx,
              child: Text(
                profiles[idx]['name'] ?? 'Profile $idx',
                style: GoogleFonts.orbitron(color: profColor, fontWeight: FontWeight.bold),
              ),
            );
          }),
          onChanged: (val) {
            if (val != null) {
              state.setActiveProfile(val);
              setState(() => _editingKeyIdx = null);
            }
          },
        ),
        IconButton(
          icon: const Icon(Icons.edit, color: Colors.grey),
          onPressed: () => _showEditProfileDialog(state),
        ),
        IconButton(
          icon: const Icon(Icons.add_circle_outline, color: AppTheme.accent),
          onPressed: () => _showAddProfileDialog(state),
        ),
      ],
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
                  TextField(
                    controller: _ipController,
                    decoration: const InputDecoration(labelText: 'Device IP / Hostname'),
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
                    state.connect(_ipController.text);
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

  Widget _buildModeToggle(DraupnirState state) {
    return Container(
      decoration: BoxDecoration(
        color: AppTheme.surfaceHighlight,
        borderRadius: BorderRadius.circular(8),
      ),
      child: Row(
        children: [
          InkWell(
            onTap: state.isEditorMode ? () => state.toggleEditorMode() : null,
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
              decoration: BoxDecoration(
                color: !state.isEditorMode ? AppTheme.accent : Colors.transparent,
                borderRadius: const BorderRadius.horizontal(left: Radius.circular(8)),
              ),
              child: Text(
                'RUN MODE',
                style: TextStyle(
                  color: !state.isEditorMode ? Colors.black : Colors.white,
                  fontWeight: FontWeight.bold,
                ),
              ),
            ),
          ),
          InkWell(
            onTap: !state.isEditorMode ? () => state.toggleEditorMode() : null,
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
              decoration: BoxDecoration(
                color: state.isEditorMode ? AppTheme.accent : Colors.transparent,
                borderRadius: const BorderRadius.horizontal(right: Radius.circular(8)),
              ),
              child: Text(
                'EDIT MODE',
                style: TextStyle(
                  color: state.isEditorMode ? Colors.black : Colors.white,
                  fontWeight: FontWeight.bold,
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildConnectionBar(DraupnirState state) {
    return Container(
      padding: const EdgeInsets.all(16),
      color: AppTheme.surface,
      child: Row(
        children: [
          Expanded(
            child: TextField(
              controller: _ipController,
              decoration: const InputDecoration(
                labelText: 'Device IP / Hostname',
                border: OutlineInputBorder(),
                isDense: true,
              ),
            ),
          ),
          const SizedBox(width: 16),
          ElevatedButton(
            onPressed: () => state.connect(_ipController.text),
            child: const Text('CONNECT'),
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
            crossAxisAlignment: CrossAxisAlignment.baseline,
            textBaseline: TextBaseline.alphabetic,
            children: [
              Text(
                profileName,
                style: GoogleFonts.orbitron(fontSize: 24, fontWeight: FontWeight.bold, color: profileColor),
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
