import 'package:flutter/material.dart';
import '../theme.dart';

/// Colour picker with a palette dropdown, shared by the macro editor and the profile dialog.
///
/// Both used to carry their own copy of the same `Wrap` over one flat colour list. They are the
/// same control doing the same job, so they are one widget now -- the palettes would otherwise
/// have had to be wired up twice and could drift apart afterwards.
class PalettePicker extends StatefulWidget {
  const PalettePicker({
    super.key,
    required this.selected,
    required this.onChanged,
    this.label,
  });

  final Color selected;
  final ValueChanged<Color> onChanged;

  /// Section heading. Null renders no heading, for callers that supply their own.
  final String? label;

  @override
  State<PalettePicker> createState() => _PalettePickerState();
}

class _PalettePickerState extends State<PalettePicker> {
  late String _activePalette;

  @override
  void initState() {
    super.initState();
    _activePalette = _paletteContaining(widget.selected);
  }

  @override
  void didUpdateWidget(PalettePicker old) {
    super.didUpdateWidget(old);
    // Follow the colour if it was changed from outside (the editor reloading a different macro),
    // but NOT while the user is browsing: picking a colour calls onChanged, which comes back as
    // a new `selected`, and re-deriving the tab from it would be a no-op for an in-palette pick
    // and a surprise jump for a shared one. Only move when the incoming colour is absent from
    // the palette currently open.
    if (widget.selected != old.selected &&
        !_colorsFor(_activePalette).contains(widget.selected)) {
      _activePalette = _paletteContaining(widget.selected);
    }
  }

  /// The palette a colour belongs to, or the first palette when it belongs to none.
  ///
  /// "None" is a real case, not a defensive branch: a macro saved before the palettes existed,
  /// or one whose colour arrived in an imported profile from another device, can hold any hex at
  /// all. Those open on the first palette and are shown by the current-colour swatch instead, so
  /// the picker never silently misrepresents what the macro is actually set to.
  String _paletteContaining(Color c) {
    for (final entry in AppTheme.colorPalettes.entries) {
      if (entry.value.contains(c)) return entry.key;
    }
    return AppTheme.colorPalettes.keys.first;
  }

  List<Color> _colorsFor(String name) =>
      AppTheme.colorPalettes[name] ?? const <Color>[];

  /// Four representative colours for a palette's dropdown row.
  ///
  /// The themed palettes are 4 hue families of 4 tones, so index 2, 6, 10 and 14 is the third
  /// step of each family -- the palette's character, one dot per hue. Third and not second:
  /// that is the peak-chroma step in every palette, so the strip shows Black Trenchcoat as its
  /// actual phosphor green rather than a pale mint. DIY is a flat list with no families, so it
  /// is sampled at even intervals across whatever length it happens to be.
  List<Color> _previewSwatches(List<Color> palette) {
    if (palette.length == 16) {
      return [palette[2], palette[6], palette[10], palette[14]];
    }
    if (palette.length < 4) return palette;
    final step = palette.length / 4;
    return [for (int i = 0; i < 4; i++) palette[(i * step).floor()]];
  }

  @override
  Widget build(BuildContext context) {
    final colors = _colorsFor(_activePalette);
    final inPalette = colors.contains(widget.selected);

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            if (widget.label != null)
              Text(widget.label!, style: const TextStyle(fontWeight: FontWeight.bold)),
            const Spacer(),
            // The current colour, always visible and always accurate -- including when it is in
            // no palette, which is exactly when a grid of swatches with nothing ringed would be
            // most misleading.
            Container(
              width: 20,
              height: 20,
              decoration: BoxDecoration(
                color: widget.selected,
                shape: BoxShape.circle,
                border: Border.all(color: Colors.white24),
              ),
            ),
            if (!inPalette) ...[
              const SizedBox(width: 6),
              const Text('custom',
                  style: TextStyle(fontSize: 11, color: AppTheme.textSecondary)),
            ],
          ],
        ),
        const SizedBox(height: 8),
        // A dropdown, not a row of tabs. Six palette names do not fit the width of a phone, so
        // tabs meant a horizontal scroll inside a vertically scrolling form -- the later
        // palettes were offscreen with nothing to say so, and the gesture fought the page.
        // A dropdown costs one tap, shows every option at once, and grows down the axis the
        // form already scrolls.
        //
        // Each row carries a swatch strip, so the palettes can be compared by eye before
        // committing rather than selected by name and then judged.
        DropdownButtonFormField<String>(
          value: _activePalette,
          isExpanded: true,
          decoration: const InputDecoration(
            isDense: true,
            contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 10),
            border: OutlineInputBorder(),
          ),
          dropdownColor: AppTheme.surfaceHighlight,
          // The closed state is the name alone. The strip belongs in the open menu, where it is
          // doing comparison work; under the swatch grid it would just repeat it.
          selectedItemBuilder: (context) => [
            for (final name in AppTheme.colorPalettes.keys)
              Align(
                alignment: Alignment.centerLeft,
                child: Text(name, style: const TextStyle(fontSize: 13)),
              ),
          ],
          items: [
            for (final entry in AppTheme.colorPalettes.entries)
              DropdownMenuItem(
                value: entry.key,
                child: Row(
                  children: [
                    Expanded(
                      child: Text(entry.key,
                          style: const TextStyle(fontSize: 13),
                          overflow: TextOverflow.ellipsis),
                    ),
                    const SizedBox(width: 8),
                    // One swatch per hue family: for the 4x4 palettes that is the brightest
                    // tone of each, which is the palette's character in four dots. DIY has no
                    // such structure, so it is sampled evenly across the list instead.
                    for (final c in _previewSwatches(entry.value))
                      Container(
                        width: 12,
                        height: 12,
                        margin: const EdgeInsets.only(left: 3),
                        decoration: BoxDecoration(
                          color: c,
                          shape: BoxShape.circle,
                          border: Border.all(color: Colors.white24),
                        ),
                      ),
                  ],
                ),
              ),
          ],
          onChanged: (name) {
            if (name != null) setState(() => _activePalette = name);
          },
        ),
        const SizedBox(height: 10),
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: [
            for (final color in colors)
              InkWell(
                onTap: () => widget.onChanged(color),
                customBorder: const CircleBorder(),
                child: Container(
                  width: 32,
                  height: 32,
                  decoration: BoxDecoration(
                    color: color,
                    shape: BoxShape.circle,
                    border: Border.all(
                      color: color == widget.selected
                          ? Colors.white
                          : Colors.transparent,
                      width: 3,
                    ),
                  ),
                ),
              ),
          ],
        ),
      ],
    );
  }
}
