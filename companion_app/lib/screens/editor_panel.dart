import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:file_selector/file_selector.dart';
import 'package:xml/xml.dart';
import 'dart:io';
import 'dart:convert';

import '../state/draupnir_state.dart';
import '../theme.dart';
import '../utils/feather_icons_map.dart';
import '../utils/icon_generator.dart';

class EditorPanel extends StatefulWidget {
  final int position;
  final VoidCallback onClose;

  const EditorPanel({super.key, required this.position, required this.onClose});

  @override
  State<EditorPanel> createState() => _EditorPanelState();
}

class _EditorPanelState extends State<EditorPanel> {
  late TextEditingController _nameController;
  late Color _selectedColor;
  late List<dynamic> _actions;
  late String _mode;
  late String _selectedIcon;

  @override
  void initState() {
    super.initState();
    _initFromState();
  }

  @override
  void didUpdateWidget(EditorPanel oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.position != widget.position) {
      _initFromState();
    }
  }

  void _initFromState() {
    final state = context.read<DraupnirState>();
    final macro = state.currentMacros.firstWhere(
      (m) => m['pos'] == widget.position,
      orElse: () => {'name': 'New Macro', 'color': 'FF00FF00', 'actions': [], 'mode': 'play_once', 'icon': ''},
    );

    _nameController = TextEditingController(text: macro['name'] ?? '');
    
    _selectedColor = AppTheme.accent;
    if (macro['color'] != null) {
      try {
        String hex = macro['color'].toString().replaceAll('#', '');
        _selectedColor = Color(int.parse('FF$hex', radix: 16));
      } catch (_) {}
    }

    _mode = macro['mode'] ?? 'play_once';
    _selectedIcon = macro['icon'] ?? '';

    // deep copy actions to edit locally
    _actions = List.from(macro['actions'] ?? []).map((e) => Map<String,dynamic>.from(e)).toList();
  }

  Future<void> _saveMacro() async {
    final state = context.read<DraupnirState>();
    final colorHex = _selectedColor.value.toRadixString(16).padLeft(8, '0').substring(2).toUpperCase();
    
    // Generate XBM string if it's a feather icon
    String iconXbm = "";
    if (featherIconsMap.containsKey(_selectedIcon)) {
      iconXbm = await generateXbmHexForIcon(featherIconsMap[_selectedIcon]!);
    }

    state.updateMacro(widget.position, {
      'name': _nameController.text,
      'color': colorHex,
      'mode': _mode,
      'icon': _selectedIcon,
      'icon_xbm': iconXbm,
      'actions': _actions,
    });
    
    widget.onClose();
  }

  Future<void> _importSynapseXML() async {
    try {
      final XFile? file = await openFile(
        acceptedTypeGroups: [
          XTypeGroup(
            label: 'Synapse Profiles',
            extensions: ['xml', 'synapse3'],
          ),
        ],
      );

      if (file != null) {
        final bytes = await file.readAsBytes();
        final xmlString = utf8.decode(bytes);
        final document = XmlDocument.parse(xmlString);
        
        List<Map<String, dynamic>> importedActions = [];
        
        final events = document.findAllElements('MacroEvent');
        for (var event in events) {
          final typeElem = event.findElements('Type').firstOrNull;
          if (typeElem == null) continue;

          if (typeElem.innerText == '0') {
            // Delay event
            final numberElem = event.findElements('Number').firstOrNull;
            if (numberElem != null) {
              double seconds = double.tryParse(numberElem.innerText) ?? 0.0;
              int ms = (seconds * 1000).round();
              if (ms > 0) {
                importedActions.add({'type': 'delay', 'ms': ms});
              }
            }
          } else if (typeElem.innerText == '1') {
            // Key event
            final keyEvent = event.findElements('KeyEvent').firstOrNull;
            if (keyEvent != null) {
              final stateElem = keyEvent.findElements('State').firstOrNull;
              // State 0 is KeyDown. We only add the keystroke once, so we trigger on KeyDown.
              if (stateElem != null && stateElem.innerText == '0') {
                final makecodeElem = keyEvent.findElements('Makecode').firstOrNull;
                if (makecodeElem != null) {
                  int asciiCode = int.tryParse(makecodeElem.innerText) ?? 0;
                  if (asciiCode > 0) {
                    importedActions.add({'type': 'key', 'key': String.fromCharCode(asciiCode)});
                  }
                }
              }
            }
          }
        }
        
        if (importedActions.isNotEmpty) {
          setState(() {
            _actions.addAll(importedActions);
          });
        }
      }
    } catch (e) {
      debugPrint("XML Parse Error: $e");
    }
  }

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 400,
      color: AppTheme.surfaceHighlight,
      child: Column(
        children: [
          _buildHeader(),
          Expanded(
            child: ListView(
              padding: const EdgeInsets.all(16),
              children: [
                _buildNameField(),
                const SizedBox(height: 24),
                _buildModeSection(),
                const SizedBox(height: 24),
                _buildIconSection(),
                const SizedBox(height: 24),
                _buildColorSection(),
                const SizedBox(height: 24),
                _buildActionsSection(),
              ],
            ),
          ),
          _buildFooter(),
        ],
      ),
    );
  }

  Widget _buildHeader() {
    return Container(
      padding: const EdgeInsets.all(16),
      color: AppTheme.surface,
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Text(
            'EDIT KEY ${widget.position}',
            style: const TextStyle(fontWeight: FontWeight.bold, color: AppTheme.accent),
          ),
          IconButton(
            icon: const Icon(Icons.close),
            onPressed: widget.onClose,
          ),
        ],
      ),
    );
  }

  Widget _buildNameField() {
    return TextField(
      controller: _nameController,
      decoration: const InputDecoration(
        labelText: 'Macro Name',
        border: OutlineInputBorder(),
      ),
    );
  }

  Widget _buildModeSection() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text('Execution Mode', style: TextStyle(fontWeight: FontWeight.bold)),
        const SizedBox(height: 8),
        DropdownButtonFormField<String>(
          value: _mode,
          decoration: const InputDecoration(border: OutlineInputBorder()),
          dropdownColor: AppTheme.surfaceHighlight,
          items: const [
            DropdownMenuItem(value: 'play_once', child: Text('Play Once')),
            DropdownMenuItem(value: 'toggle', child: Text('Toggle Loop')),
            DropdownMenuItem(value: 'rotary', child: Text('Rotary Dial')),
          ],
          onChanged: (val) {
            if (val != null) {
              setState(() {
                _mode = val;
                if (_mode == 'rotary' && _actions.length < 2) {
                  while (_actions.length < 2) {
                    _actions.add({'type': 'delay', 'ms': 10});
                  }
                }
              });
            }
          },
        ),
      ],
    );
  }

  Widget _buildIconSection() {
    IconData currentIcon = Icons.help_outline;
    if (_selectedIcon == 'hammer') currentIcon = Icons.build;
    else if (_selectedIcon == 'text') currentIcon = Icons.text_fields;
    else if (_selectedIcon == 'mic') currentIcon = Icons.mic;
    else if (featherIconsMap.containsKey(_selectedIcon)) currentIcon = featherIconsMap[_selectedIcon]!;

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text('Key Icon', style: TextStyle(fontWeight: FontWeight.bold)),
        const SizedBox(height: 8),
        Row(
          children: [
            Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: AppTheme.accent.withOpacity(0.2),
                border: Border.all(color: AppTheme.accent),
                borderRadius: BorderRadius.circular(8),
              ),
              child: Icon(currentIcon, color: AppTheme.accent, size: 32),
            ),
            const SizedBox(width: 16),
            ElevatedButton.icon(
              icon: const Icon(Icons.search),
              label: const Text('Select Icon'),
              onPressed: () => _showIconPickerModal(),
            ),
          ],
        ),
      ],
    );
  }

  void _showIconPickerModal() {
    showDialog(
      context: context,
      builder: (ctx) {
        String searchQuery = "";
        return StatefulBuilder(
          builder: (context, setModalState) {
            final filteredKeys = featherIconsMap.keys.where((k) => k.toLowerCase().contains(searchQuery.toLowerCase())).toList();
            return AlertDialog(
              title: const Text('Select Icon'),
              backgroundColor: AppTheme.surfaceHighlight,
              content: SizedBox(
                width: 400,
                height: 400,
                child: Column(
                  children: [
                    TextField(
                      decoration: const InputDecoration(
                        labelText: 'Search Icons',
                        prefixIcon: Icon(Icons.search),
                        border: OutlineInputBorder(),
                      ),
                      onChanged: (val) => setModalState(() => searchQuery = val),
                    ),
                    const SizedBox(height: 16),
                    Expanded(
                      child: GridView.builder(
                        gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
                          crossAxisCount: 5,
                          crossAxisSpacing: 8,
                          mainAxisSpacing: 8,
                        ),
                        itemCount: filteredKeys.length + 3, // +3 for defaults
                        itemBuilder: (context, idx) {
                          String iconName;
                          IconData iconData;
                          if (idx == 0 && "hammer".contains(searchQuery.toLowerCase())) { iconName = 'hammer'; iconData = Icons.build; }
                          else if (idx == 1 && "text".contains(searchQuery.toLowerCase())) { iconName = 'text'; iconData = Icons.text_fields; }
                          else if (idx == 2 && "mic".contains(searchQuery.toLowerCase())) { iconName = 'mic'; iconData = Icons.mic; }
                          else {
                            int mapIdx = idx - 3;
                            if (mapIdx < 0 || mapIdx >= filteredKeys.length) return const SizedBox.shrink();
                            iconName = filteredKeys[mapIdx];
                            iconData = featherIconsMap[iconName]!;
                          }

                          return InkWell(
                            onTap: () {
                              setState(() => _selectedIcon = iconName);
                              Navigator.pop(ctx);
                            },
                            child: Container(
                              decoration: BoxDecoration(
                                color: _selectedIcon == iconName ? AppTheme.accent.withOpacity(0.3) : AppTheme.surface,
                                borderRadius: BorderRadius.circular(8),
                                border: Border.all(color: _selectedIcon == iconName ? AppTheme.accent : Colors.transparent),
                              ),
                              child: Icon(iconData, color: Colors.white),
                            ),
                          );
                        },
                      ),
                    ),
                  ],
                ),
              ),
              actions: [
                TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('CANCEL')),
              ],
            );
          }
        );
      },
    );
  }

  Widget _buildColorSection() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text('Key Color', style: TextStyle(fontWeight: FontWeight.bold)),
        const SizedBox(height: 8),
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: AppTheme.cyberpunkPalette.map((color) {
            final isSelected = _selectedColor == color;
            return InkWell(
              onTap: () => setState(() => _selectedColor = color),
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
    );
  }

  Widget _buildActionsSection() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          mainAxisAlignment: MainAxisAlignment.spaceBetween,
          children: [
            const Text('Action Sequence', style: TextStyle(fontWeight: FontWeight.bold)),
            PopupMenuButton<String>(
              icon: const Icon(Icons.add, color: AppTheme.accent),
              color: AppTheme.surfaceHighlight,
              onSelected: (val) {
                setState(() {
                  if (val == 'text') {
                    _actions.add({'type': 'text', 'value': ''});
                  } else if (val == 'delay') {
                    _actions.add({'type': 'delay', 'ms': 100});
                  } else if (val == 'key') {
                    _actions.add({'type': 'key', 'key': 'A', 'mods': []});
                  } else if (val == 'consumer') {
                    _actions.add({'type': 'consumer', 'code': 'MUTE'});
                  } else if (val == 'mouse') {
                    _actions.add({'type': 'mouse', 'button': 'LEFT', 'event': 'CLICK'});
                  }
                });
              },
              itemBuilder: (context) => [
                const PopupMenuItem(value: 'key', child: Text('Add Keystroke')),
                const PopupMenuItem(value: 'mouse', child: Text('Add Mouse Action')),
                const PopupMenuItem(value: 'consumer', child: Text('Add Media/System Action')),
                const PopupMenuItem(value: 'text', child: Text('Add Text Block')),
                const PopupMenuItem(value: 'delay', child: Text('Add Delay')),
              ],
            )
          ],
        ),
        const SizedBox(height: 8),
        if (_mode != 'rotary')
          OutlinedButton.icon(
            onPressed: _importSynapseXML,
            icon: const Icon(Icons.upload_file),
            label: const Text('Import Synapse XML'),
          ),
        if (_mode != 'rotary') const SizedBox(height: 8),
        if (_mode == 'rotary')
          Column(
            children: [
              Card(
                margin: const EdgeInsets.only(bottom: 8),
                color: AppTheme.background,
                child: Padding(
                  padding: const EdgeInsets.all(8.0),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Text('Clockwise Action', style: TextStyle(color: AppTheme.accent, fontWeight: FontWeight.bold)),
                      const SizedBox(height: 8),
                      _buildActionEditor(_actions[0]),
                    ],
                  ),
                ),
              ),
              Card(
                margin: const EdgeInsets.only(bottom: 8),
                color: AppTheme.background,
                child: Padding(
                  padding: const EdgeInsets.all(8.0),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Text('Counter-Clockwise Action', style: TextStyle(color: AppTheme.accent, fontWeight: FontWeight.bold)),
                      const SizedBox(height: 8),
                      _buildActionEditor(_actions[1]),
                    ],
                  ),
                ),
              ),
            ],
          )
        else if (_actions.isEmpty)
          const Text('No actions configured.', style: TextStyle(color: Colors.grey))
        else
          ReorderableListView.builder(
            shrinkWrap: true,
            physics: const NeverScrollableScrollPhysics(),
            itemCount: _actions.length,
            onReorderItem: (oldIdx, newIdx) {
              setState(() {
                if (newIdx > oldIdx) newIdx -= 1;
                final item = _actions.removeAt(oldIdx);
                _actions.insert(newIdx, item);
              });
            },
            itemBuilder: (context, idx) {
              final action = _actions[idx];
              return Card(
                key: ValueKey('$idx-${action.hashCode}'),
                margin: const EdgeInsets.only(bottom: 8),
                color: AppTheme.background,
                child: ListTile(
                  leading: const Icon(Icons.drag_indicator),
                  title: _buildActionEditor(action),
                  trailing: IconButton(
                    icon: const Icon(Icons.delete, color: Colors.redAccent),
                    onPressed: () => setState(() => _actions.removeAt(idx)),
                  ),
                ),
              );
            },
          ),
      ],
    );
  }

  Widget _buildActionEditor(Map<String, dynamic> action) {
    Widget editor = const SizedBox.shrink();

    if (action['type'] == 'text') {
      editor = TextField(
        controller: TextEditingController(text: action['value'])..selection = TextSelection.collapsed(offset: (action['value'] as String?)?.length ?? 0),
        decoration: const InputDecoration(labelText: 'Text to type', isDense: true),
        onChanged: (val) => action['value'] = val,
      );
    } else if (action['type'] == 'delay') {
      editor = TextField(
        controller: TextEditingController(text: action['ms'].toString())..selection = TextSelection.collapsed(offset: action['ms'].toString().length),
        decoration: const InputDecoration(labelText: 'Delay (ms)', isDense: true),
        keyboardType: TextInputType.number,
        onChanged: (val) => action['ms'] = int.tryParse(val) ?? 100,
      );
    } else if (action['type'] == 'key') {
      editor = Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text('Modifiers', style: TextStyle(fontSize: 12, color: Colors.grey)),
          Wrap(
            spacing: 8.0,
            children: ['CTRL', 'SHIFT', 'ALT', 'WIN'].map((mod) {
              List mods = action['mods'] ?? [];
              bool isSelected = mods.contains(mod);
              return FilterChip(
                label: Text(mod, style: const TextStyle(fontSize: 12)),
                selected: isSelected,
                selectedColor: AppTheme.accent.withOpacity(0.4),
                onSelected: (selected) {
                  setState(() {
                    if (selected) {
                      mods.add(mod);
                    } else {
                      mods.remove(mod);
                    }
                    action['mods'] = mods;
                  });
                },
              );
            }).toList(),
          ),
          const SizedBox(height: 8),
          Row(
            children: [
              Expanded(
                child: DropdownButtonFormField<String>(
                  isExpanded: true,
                  value: ['A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z','1','2','3','4','5','6','7','8','9','0','ENTER','SPACE','TAB','ESC','BACKSPACE','DELETE','UP','DOWN','LEFT','RIGHT','PRINTSCREEN','F1','F2','F3','F4','F5','F6','F7','F8','F9','F10','F11','F12'].contains(action['key']?.toString().toUpperCase()) ? action['key']?.toString().toUpperCase() : 'Custom',
                  decoration: const InputDecoration(labelText: 'Key', isDense: true),
                  dropdownColor: AppTheme.surfaceHighlight,
                  items: [
                    'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
                    '1','2','3','4','5','6','7','8','9','0',
                    'ENTER','SPACE','TAB','ESC','BACKSPACE','DELETE',
                    'UP','DOWN','LEFT','RIGHT','PRINTSCREEN',
                    'F1','F2','F3','F4','F5','F6','F7','F8','F9','F10','F11','F12',
                    'Custom'
                  ].map((k) => DropdownMenuItem(value: k, child: Text(k))).toList(),
                  onChanged: (val) {
                    setState(() {
                      if (val != 'Custom') action['key'] = val;
                    });
                  },
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: TextField(
                  controller: TextEditingController(text: action['key'])..selection = TextSelection.collapsed(offset: (action['key'] as String?)?.length ?? 0),
                  decoration: const InputDecoration(labelText: 'Custom Keystroke', isDense: true),
                  onChanged: (val) => action['key'] = val,
                ),
              ),
            ],
          ),
        ],
      );
    } else if (action['type'] == 'consumer') {
      editor = Row(
        children: [
          Expanded(
            child: DropdownButtonFormField<String>(
              isExpanded: true,
              value: ['MUTE', 'VOL_UP', 'VOL_DOWN', 'PLAY_PAUSE', 'NEXT', 'PREV'].contains(action['code']) ? action['code'] : 'CUSTOM',
              decoration: const InputDecoration(labelText: 'Media Action', isDense: true),
              dropdownColor: AppTheme.surfaceHighlight,
              items: const [
                DropdownMenuItem(value: 'MUTE', child: Text('Mute')),
                DropdownMenuItem(value: 'VOL_UP', child: Text('Volume Up')),
                DropdownMenuItem(value: 'VOL_DOWN', child: Text('Volume Down')),
                DropdownMenuItem(value: 'PLAY_PAUSE', child: Text('Play / Pause')),
                DropdownMenuItem(value: 'NEXT', child: Text('Next Track')),
                DropdownMenuItem(value: 'PREV', child: Text('Previous Track')),
                DropdownMenuItem(value: 'CUSTOM', child: Text('Custom Hex Code')),
              ],
              onChanged: (val) {
                setState(() {
                  if (val != 'CUSTOM') action['code'] = val;
                });
              },
            ),
          ),
          const SizedBox(width: 8),
          Expanded(
            child: TextField(
              controller: TextEditingController(text: action['code'])..selection = TextSelection.collapsed(offset: (action['code'] as String?)?.length ?? 0),
              decoration: const InputDecoration(labelText: 'Custom Code (Hex)', isDense: true),
              onChanged: (val) => action['code'] = val,
            ),
          ),
        ],
      );
    } else if (action['type'] == 'mouse') {
      editor = Row(
        children: [
          Expanded(
            child: DropdownButtonFormField<String>(
              isExpanded: true,
              value: ['LEFT', 'RIGHT', 'MIDDLE', 'MB4', 'MB5', 'SCROLL_UP', 'SCROLL_DOWN', 'SCROLL_LEFT', 'SCROLL_RIGHT'].contains(action['button']?.toString().toUpperCase()) ? action['button']?.toString().toUpperCase() : 'LEFT',
              decoration: const InputDecoration(labelText: 'Mouse Button', isDense: true),
              dropdownColor: AppTheme.surfaceHighlight,
              items: const [
                DropdownMenuItem(value: 'LEFT', child: Text('Left Click')),
                DropdownMenuItem(value: 'RIGHT', child: Text('Right Click')),
                DropdownMenuItem(value: 'MIDDLE', child: Text('Middle Click')),
                DropdownMenuItem(value: 'MB4', child: Text('Mouse Button 4 (Back)')),
                DropdownMenuItem(value: 'MB5', child: Text('Mouse Button 5 (Forward)')),
                DropdownMenuItem(value: 'SCROLL_UP', child: Text('Scroll Up')),
                DropdownMenuItem(value: 'SCROLL_DOWN', child: Text('Scroll Down')),
                DropdownMenuItem(value: 'SCROLL_LEFT', child: Text('Scroll Left')),
                DropdownMenuItem(value: 'SCROLL_RIGHT', child: Text('Scroll Right')),
              ],
              onChanged: (val) {
                setState(() {
                  if (val != null) action['button'] = val;
                });
              },
            ),
          ),
          const SizedBox(width: 8),
          Expanded(
            child: DropdownButtonFormField<String>(
              isExpanded: true,
              value: ['CLICK', 'DOUBLE_CLICK', 'HOLD', 'RELEASE'].contains(action['event']?.toString().toUpperCase()) ? action['event']?.toString().toUpperCase() : 'CLICK',
              decoration: const InputDecoration(labelText: 'Event', isDense: true),
              dropdownColor: AppTheme.surfaceHighlight,
              items: const [
                DropdownMenuItem(value: 'CLICK', child: Text('Click')),
                DropdownMenuItem(value: 'DOUBLE_CLICK', child: Text('Double Click')),
                DropdownMenuItem(value: 'HOLD', child: Text('Hold / Press')),
                DropdownMenuItem(value: 'RELEASE', child: Text('Release')),
              ],
              onChanged: (val) {
                setState(() {
                  if (val != null) action['event'] = val;
                });
              },
            ),
          ),
        ],
      );
    }

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      mainAxisSize: MainAxisSize.min,
      children: [
        DropdownButtonFormField<String>(
          isExpanded: true,
          isDense: true,
          decoration: const InputDecoration(labelText: 'Action Type', isDense: true),
          dropdownColor: AppTheme.surfaceHighlight,
          value: action['type'],
          items: const [
            DropdownMenuItem(value: 'key', child: Text('Keystroke')),
            DropdownMenuItem(value: 'mouse', child: Text('Mouse Action')),
            DropdownMenuItem(value: 'consumer', child: Text('Media/System Action')),
            DropdownMenuItem(value: 'text', child: Text('Text Block')),
            DropdownMenuItem(value: 'delay', child: Text('Delay')),
          ],
          onChanged: (val) {
            setState(() {
              if (val != null && val != action['type']) {
                action['type'] = val;
                if (val == 'key') { action['key'] = 'A'; action['mods'] = []; }
                else if (val == 'mouse') { action['button'] = 'LEFT'; action['event'] = 'CLICK'; }
                else if (val == 'consumer') { action['code'] = 'MUTE'; }
                else if (val == 'delay') { action['ms'] = 100; }
                else if (val == 'text') { action['value'] = ''; }
              }
            });
          },
        ),
        const SizedBox(height: 8),
        editor,
      ],
    );
  }

  Widget _buildFooter() {
    return Container(
      padding: const EdgeInsets.all(16),
      color: AppTheme.surface,
      child: Row(
        mainAxisAlignment: MainAxisAlignment.end,
        children: [
          ElevatedButton(
            onPressed: _saveMacro,
            child: const Text('SAVE TO DEVICE'),
          ),
        ],
      ),
    );
  }
}
