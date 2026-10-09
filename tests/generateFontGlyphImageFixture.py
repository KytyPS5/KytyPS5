from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen


builder = FontBuilder(1024, isTTF=True)
glyph_order = [".notdef", "space", "A"]
builder.setupGlyphOrder(glyph_order)

empty_pen = TTGlyphPen(None)
empty_glyph = empty_pen.glyph()
triangle_pen = TTGlyphPen(None)
triangle_pen.moveTo((0, 0))
triangle_pen.lineTo((1024, 0))
triangle_pen.lineTo((512, 1024))
triangle_pen.closePath()
glyphs = {".notdef": empty_glyph, "space": empty_glyph, "A": triangle_pen.glyph()}
builder.setupGlyf(glyphs)
builder.setupHorizontalMetrics({".notdef": (512, 0), "space": (512, 0), "A": (1024, 0)})
builder.setupHorizontalHeader(ascent=1024, descent=0)
builder.setupCharacterMap({32: "space", 65: "A"})
builder.setupNameTable({"familyName": "SyntheticGlyphTest", "styleName": "Regular"})
builder.setupOS2(sTypoAscender=1024, sTypoDescender=0, usWinAscent=1024, usWinDescent=0)
builder.setupPost()
builder.setupMaxp()
builder.setupHead()
builder.save("tests/data/font_glyph_image.ttf")
