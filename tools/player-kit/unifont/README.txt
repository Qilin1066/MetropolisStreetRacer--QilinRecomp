GNU Unifont, in the game's program
==================================

The game asks the Dreamcast's boot ROM for its font (the system's own
characters, drawn on some of its screens). The port answers with glyphs
from GNU Unifont 18.0.01 (the Japanese variant, unifont_jp-18.0.01.hex.gz,
https://unifoundry.com/unifont/), laid out in the boot ROM font's order by
the project's tools/make_romfont.py. They are compiled into the kit's
library (kit/lib), and so into the game the launcher builds.

Unifont's glyphs are dual-licensed, at the recipient's choice (Unifont's
README and NEWS, release 13.0.04 and later):

- the GNU General Public License, version 2 or (at your option) any later
  version, with the GNU font embedding exception: GPL-2.0.txt here (the
  COPYING file of Unifont 18.0.01), and the exception as Unifont's README
  states it:

     As a special exception, if you create a document which uses this font,
     and embed this font or unaltered portions of this font into the document,
     this font does not by itself cause the resulting document to be covered
     by the GNU General Public License. This exception does not however
     invalidate any other reasons why the document might be covered by the
     GNU General Public License. If you modify this font, you may extend
     this exception to your version of the font, but you are not obligated
     to do so. If you do not wish to do so, delete this exception statement
     from your version.

- or the SIL Open Font License, version 1.1: OFL-1.1.txt here (the file of
  that name in Unifont 18.0.01).

This project (GPL-3.0) takes them under the GPL, version 3 of the licence
being one of the later versions the first choice allows; the glyphs as
converted are in the project's source (msr/src/romfont_glyphs.inc), which
says so at its top. Unifont is by Roman Czyborra, Paul Hardy and its other
contributors (Unifont's README names them).
