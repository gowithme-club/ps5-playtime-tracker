/* Files built into the payload: the tile icon and background, and the
 * console page.
 *
 * GWM_ASSET(name, "assets/file") puts the file's bytes in .rodata as name[],
 * ending at name_end[]. The path is relative to the repository root, where
 * the Makefile runs the compiler.
 */

#ifndef GWM_ASSET_H
#define GWM_ASSET_H

#define GWM_ASSET(name, file)                                              \
	__asm__(".section .rodata\n"                                           \
	        ".balign 16\n"                                                 \
	        #name ":\n"                                                    \
	        ".incbin \"" file "\"\n"                                       \
	        #name "_end:\n"                                                \
	        ".byte 0\n"                                                    \
	        ".previous\n");                                                \
	extern const unsigned char name[] __asm__(#name);                      \
	extern const unsigned char name##_end[] __asm__(#name "_end");

/* Each file is defined once here; tile.h and web.h both use the icon. */
GWM_ASSET(gwm_icon_png, "assets/icon0.png")
GWM_ASSET(gwm_page_html, "assets/page.html")
GWM_ASSET(gwm_pic0, "assets/pic0.dds.rle")

#endif /* GWM_ASSET_H */
