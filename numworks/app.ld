/* nwgba - order of the code inside the app.
 *
 * The app is linked in two steps: this script is used for the first one (a
 * relocatable link, the .nwa file); the installer (nwlink) does the second
 * one on the user's computer and simply appends our .text. All this script
 * does is put first, in one piece and aligned on a cache line, the
 * functions the compiler was told run often (tools/pgo.sh). */
SECTIONS {
  .text : ALIGN(32) {
    *(.text.hot .text.hot.*)
    *(.text .text.*)
  }
}
