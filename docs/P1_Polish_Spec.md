## UI/UX findings for P1 implementation

1. Encoder resolution mismatch- the encoder is reading 4 positions inbetween each  detent which makes it dificult to make  accurate selections because you are fighting the detents.
   * example:Detent positions vs displayed positions
     * Detent 0 = display position 3
     * detent 1 = Pos 7
     * detent 2 = Pos 11
     * detent 3 = Pos 15
     * detent 4 = Loops back to Pos 3
   * Definition of good: screen feedback matches tactile feedback
2. No Icons or Color based legend- the config has a icon in the spec, but they are never actually displayed anywhere, icons can be a critical visual shortcut, and they make the UI nicer.
   * Definition of good: macros have thier associated icon and color mapping
   * Suggested implementation: 16 circles spaced radially around the edge of the dial and a visual indicator like the needle of a gauge pointing to the active macro. The circles are color coded and have the icon associated with the macro centered inside it.  The full name of the active macro is still displayed in the center of the screen. The profile name is relocated to slightly above where the macro name is positioned. The pos marker no longer needs to be displayed
3. profile selection indicator- There is currently no visual feedback about other profiles being availble. end users will intuitively know to swipe left or right to change between available profiles
   * Definition of good: User has visual feedback that there are additional profiles indicated with directional markers
   * Suggested Implementation: add 🢐 🢒 characters to either side of the active profile description. The first profile in the list would only have 🢒 on the right side, and the last would only have 🢐 on the left side and the section no longer wraps around to the beginning. Tapping either arrow on the screen switches profiles in the associated direction, Swiping still work as before, but no wrapping behavior.
