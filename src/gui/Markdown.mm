#import "Markdown.h"

#include "mdparse.hpp"
#include "render.hpp"

#include <vector>

using ppcode::md::Run;
using ppcode::md::Node;
namespace md = ppcode::md;
namespace render = ppcode::render;

// ---------------------------------------------------------------------------
// Metrics
//
// Point sizes rather than columns: this is a proportional font and the text
// view does its own wrapping, which is exactly why render::markdown() cannot be
// reused for layout here.
// ---------------------------------------------------------------------------

static const CGFloat kBodySize = 12.0;
static const CGFloat kMonoSize = 11.0;
static const CGFloat kListIndent = 18.0;    // per nesting level
static const CGFloat kListHang = 16.0;      // marker column width
static const CGFloat kCodeIndent = 20.0;
static const CGFloat kQuoteIndent = 16.0;   // per quote level

CGFloat PPMarkdownLineHeight(void) {
  static CGFloat cached = 0.0;
  if (cached > 0.0) { return cached; }

  NSLayoutManager *lm = [[[NSLayoutManager alloc] init] autorelease];
  cached = [lm defaultLineHeightForFont:[NSFont systemFontOfSize:kBodySize]];
  if (cached < 4.0) { cached = 15.0; }

  return cached;
}

CGFloat PPMarkdownBaselineOffset(void) {
  static CGFloat cached = 0.0;
  if (cached > 0.0) { return cached; }

  NSLayoutManager *lm = [[[NSLayoutManager alloc] init] autorelease];
  cached = [lm defaultBaselineOffsetForFont:[NSFont systemFontOfSize:kBodySize]];
  if (cached < 2.0) { cached = 12.0; }

  return cached;
}

// A blank ruled line between paragraphs, rather than an arbitrary gap: keeping
// the spacing a whole multiple of the line height is what keeps every body line
// landing on a rule.
static CGFloat ParaSpacing(void) { return PPMarkdownLineHeight(); }

// Lock a paragraph to a whole number of body lines.
//
// This is what actually makes ruled paper work. Aligning the rules to the body
// font is not enough on its own: a heading, a code block or a table has its own
// line height, so everything after one lands at some arbitrary offset and the
// rules cut through it. Forcing every line to a multiple of the same unit keeps
// the whole transcript on one baseline grid, whatever is in it.
static void LockToGrid(NSMutableParagraphStyle *p, int lines) {
  CGFloat h = PPMarkdownLineHeight() * (lines > 0 ? lines : 1);
  [p setMinimumLineHeight:h];
  [p setMaximumLineHeight:h];
}

NSString *PPUTF8(const char *utf8) {
  NSString *s = [NSString stringWithUTF8String:utf8];

  return s ? s : @"";
}

// A std::string that may not be valid UTF-8. stringWithUTF8String: returns nil
// on bad input and appending nil would raise, so fall back to a byte-preserving
// encoding rather than losing the text.
static NSString *Str(const std::string &s) {
  if (s.empty()) { return @""; }

  NSString *r = [NSString stringWithUTF8String:s.c_str()];
  if (r) { return r; }

  r = [[[NSString alloc] initWithBytes:s.data()
                                length:s.size()
                              encoding:NSISOLatin1StringEncoding] autorelease];

  return r ? r : @"";
}

// ---------------------------------------------------------------------------
// Fonts and colours
//
// The palette is a light-background one. render::style_def() cannot be reused:
// its theme is built for a dark terminal, so Plain is near-white and would be
// invisible on paper. The code colours below are the Xcode 3 defaults, which
// are what this platform's own editor used.
// ---------------------------------------------------------------------------

static NSFont *BodyFont(void) {
  return [NSFont systemFontOfSize:kBodySize];
}

static NSFont *MonoFont(void) {
  NSFont *f = [NSFont fontWithName:@"Monaco" size:kMonoSize];

  return f ? f : [NSFont userFixedPitchFontOfSize:kMonoSize];
}

static NSColor *Rgb(int r, int g, int b) {
  return [NSColor colorWithCalibratedRed:r / 255.0
                                   green:g / 255.0
                                    blue:b / 255.0
                                   alpha:1.0];
}

static NSColor *CodeColorForStyle(render::Style s) {
  switch (s) {

    // Comments
    case render::Style::Comment:

      return Rgb(0x00, 0x74, 0x00);

    // Strings and characters
    case render::Style::String:

      return Rgb(0xC4, 0x1A, 0x16);

    // Numeric literals
    case render::Style::Number:

      return Rgb(0x1C, 0x00, 0xCF);

    // Language keywords and @-directives
    case render::Style::Keyword:

      return Rgb(0xAA, 0x0D, 0x91);

    // Framework and built-in types
    case render::Style::Type:

      return Rgb(0x3F, 0x6E, 0x75);

    // Preprocessor
    case render::Style::Preproc:

      return Rgb(0x64, 0x38, 0x20);

    // Constants such as nil, YES, true
    case render::Style::Constant:

      return Rgb(0x1C, 0x00, 0xCF);

    // Call and selector names
    case render::Style::Function:

      return Rgb(0x26, 0x47, 0x4B);

    // Everything else in a code block
    default:

      return Rgb(0x20, 0x20, 0x20);
  }
}

NSDictionary *PPMarkdownStreamAttributes(void) {
  NSMutableParagraphStyle *p =
      [[[NSMutableParagraphStyle alloc] init] autorelease];
  [p setParagraphSpacing:ParaSpacing()];
  LockToGrid(p, 1);

  return [NSDictionary dictionaryWithObjectsAndKeys:
             BodyFont(), NSFontAttributeName,
             Rgb(0x1A, 0x1A, 0x1A), NSForegroundColorAttributeName,
             p, NSParagraphStyleAttributeName,
             nil];
}

NSDictionary *PPMarkdownSpeakerAttributes(NSColor *color) {
  NSMutableParagraphStyle *p =
      [[[NSMutableParagraphStyle alloc] init] autorelease];
  [p setParagraphSpacingBefore:10.0];
  [p setParagraphSpacing:2.0];

  return [NSDictionary dictionaryWithObjectsAndKeys:
             [NSFont boldSystemFontOfSize:kBodySize], NSFontAttributeName,
             color ? color : [NSColor blackColor], NSForegroundColorAttributeName,
             p, NSParagraphStyleAttributeName,
             nil];
}

// ---------------------------------------------------------------------------
// Inline runs
// ---------------------------------------------------------------------------

// Attributes for one inline run layered over a block's base font and colour.
static NSDictionary *AttributesForRun(const Run &run, NSFont *base,
                                      NSColor *color, NSParagraphStyle *para) {
  NSMutableDictionary *a = [NSMutableDictionary dictionary];
  NSFontManager *fm = [NSFontManager sharedFontManager];
  NSFont *font = base;
  NSColor *fg = color;

  if (run.style & md::StyleCode) {
    font = MonoFont();
    fg = Rgb(0x7A, 0x30, 0x30);
    [a setObject:Rgb(0xF0, 0xF0, 0xEA) forKey:NSBackgroundColorAttributeName];
  }

  if (run.style & md::StyleBold) {
    font = [fm convertFont:font toHaveTrait:NSBoldFontMask];
  }

  // Monaco has no italic on Leopard; convertFont: returns the original face
  // rather than failing, so this is safe to ask for unconditionally.
  if (run.style & md::StyleItalic) {
    font = [fm convertFont:font toHaveTrait:NSItalicFontMask];
  }

  if (run.style & md::StyleStrike) {
    [a setObject:[NSNumber numberWithInt:NSUnderlineStyleSingle]
          forKey:NSStrikethroughStyleAttributeName];
  }

  if (run.style & md::StyleLink) {
    fg = Rgb(0x0A, 0x37, 0xA8);
    [a setObject:[NSNumber numberWithInt:NSUnderlineStyleSingle]
          forKey:NSUnderlineStyleAttributeName];

    // A malformed target yields a nil NSURL, and putting nil in the dictionary
    // would raise -- so the run simply stays styled but not clickable.
    NSURL *url = [NSURL URLWithString:Str(run.href)];
    if (url) { [a setObject:url forKey:NSLinkAttributeName]; }
  }

  [a setObject:font forKey:NSFontAttributeName];
  [a setObject:fg forKey:NSForegroundColorAttributeName];
  if (para) { [a setObject:para forKey:NSParagraphStyleAttributeName]; }

  return a;
}

static void AppendRuns(NSMutableAttributedString *out,
                       const std::vector<Run> &runs, NSFont *base,
                       NSColor *color, NSParagraphStyle *para) {
  for (size_t i = 0; i < runs.size(); i++) {
    NSString *text = Str(runs[i].text);
    if ([text length] == 0) { continue; }

    NSDictionary *attrs = AttributesForRun(runs[i], base, color, para);
    NSAttributedString *piece =
        [[[NSAttributedString alloc] initWithString:text
                                         attributes:attrs] autorelease];
    [out appendAttributedString:piece];
  }
}

static void AppendPlain(NSMutableAttributedString *out, NSString *text,
                        NSDictionary *attrs) {
  if ([text length] == 0) { return; }

  NSAttributedString *piece =
      [[[NSAttributedString alloc] initWithString:text
                                       attributes:attrs] autorelease];
  [out appendAttributedString:piece];
}

// Every block ends with its own newline carrying the block's paragraph style,
// so the spacing below it comes from that style rather than from blank lines.
static void EndBlock(NSMutableAttributedString *out, NSFont *font,
                     NSParagraphStyle *para) {
  NSDictionary *a = [NSDictionary dictionaryWithObjectsAndKeys:
                        font, NSFontAttributeName,
                        para, NSParagraphStyleAttributeName,
                        nil];
  AppendPlain(out, @"\n", a);
}

// ---------------------------------------------------------------------------
// Paragraph styles per block
// ---------------------------------------------------------------------------

static NSMutableParagraphStyle *BaseStyle(int quote) {
  NSMutableParagraphStyle *p =
      [[[NSMutableParagraphStyle alloc] init] autorelease];
  CGFloat q = quote * kQuoteIndent;
  [p setFirstLineHeadIndent:q];
  [p setHeadIndent:q];
  [p setParagraphSpacing:ParaSpacing()];
  LockToGrid(p, 1);

  return p;
}

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------

static void EmitHeading(NSMutableAttributedString *out, const Node &n) {
  CGFloat size = kBodySize;
  if (n.level <= 1) { size = 17.0; }

  else if (n.level == 2) { size = 15.0; }

  else if (n.level == 3) { size = 13.0; }

  NSMutableParagraphStyle *p = BaseStyle(n.quote);
  [p setParagraphSpacingBefore:PPMarkdownLineHeight()];
  [p setParagraphSpacing:0.0];
  LockToGrid(p, n.level <= 2 ? 2 : 1);

  NSFont *font = [NSFont boldSystemFontOfSize:size];
  AppendRuns(out, n.runs, font, Rgb(0x10, 0x10, 0x28), p);
  EndBlock(out, font, p);
}

static void EmitParagraph(NSMutableAttributedString *out, const Node &n) {
  NSMutableParagraphStyle *p = BaseStyle(n.quote);
  NSFont *font = BodyFont();
  NSColor *color = n.quote > 0 ? Rgb(0x4A, 0x4A, 0x4A) : Rgb(0x1A, 0x1A, 0x1A);

  // Quoted prose is set in italic, which is how a pull quote reads on paper and
  // costs nothing on a machine with no room for a drawn rule.
  if (n.quote > 0) {
    std::vector<Run> runs = n.runs;
    for (size_t i = 0; i < runs.size(); i++) runs[i].style |= md::StyleItalic;
    AppendRuns(out, runs, font, color, p);
  }

  else {
    AppendRuns(out, n.runs, font, color, p);
  }

  EndBlock(out, font, p);
}

static void EmitList(NSMutableAttributedString *out, const Node &n) {
  CGFloat base = n.quote * kQuoteIndent + 12.0 + n.level * kListIndent;

  NSMutableParagraphStyle *p =
      [[[NSMutableParagraphStyle alloc] init] autorelease];
  [p setFirstLineHeadIndent:base];
  [p setHeadIndent:base + kListHang];
  [p setParagraphSpacing:0.0];
  LockToGrid(p, 1);
  // A single tab stop at the hanging indent turns "marker\ttext" into a proper
  // hanging indent, so wrapped lines align under the text and not the marker.
  [p setTabStops:[NSArray array]];
  [p addTabStop:[[[NSTextTab alloc] initWithType:NSLeftTabStopType
                                        location:base + kListHang] autorelease]];

  NSString *marker = nil;
  if (n.kind == md::Block::Numbered) {
    marker = Str(n.marker);
  }

  else {
    // Built at runtime: a bullet in a @"..." literal would come out as garbage.
    const char *glyph = "\xE2\x80\xA2";                      // bullet
    if (n.level == 1) { glyph = "\xE2\x97\xA6"; }            // white bullet
    else if (n.level >= 2) { glyph = "\xE2\x96\xAA"; }       // small square
    marker = PPUTF8(glyph);
  }

  NSFont *font = BodyFont();
  NSMutableDictionary *ma = [NSMutableDictionary dictionary];
  [ma setObject:font forKey:NSFontAttributeName];
  [ma setObject:Rgb(0x40, 0x50, 0x70) forKey:NSForegroundColorAttributeName];
  [ma setObject:p forKey:NSParagraphStyleAttributeName];
  AppendPlain(out, [marker stringByAppendingString:@"\t"], ma);

  AppendRuns(out, n.runs, font, Rgb(0x1A, 0x1A, 0x1A), p);
  EndBlock(out, font, p);
}

static void EmitRule(NSMutableAttributedString *out, const Node &n) {
  NSMutableParagraphStyle *p = BaseStyle(n.quote);
  [p setParagraphSpacingBefore:6.0];
  [p setParagraphSpacing:8.0];
  // Real glyphs, and few enough to fit an ordinary window without wrapping.
  // Two earlier attempts failed for reasons worth recording: a run of U+2500
  // depends on the system font having that glyph, and an underline over spaces
  // is not drawn at all, because AppKit skips decoration on trailing
  // whitespace. Em dashes are in every font and butt together into a line.
  NSMutableString *bar = [NSMutableString string];
  NSString *dash = PPUTF8("\xE2\x80\x94");
  for (int i = 0; i < 56; i++) [bar appendString:dash];

  NSFont *font = [NSFont systemFontOfSize:9.0];
  NSDictionary *a = [NSDictionary dictionaryWithObjectsAndKeys:
                        font, NSFontAttributeName,
                        Rgb(0x9A, 0x9A, 0x9A), NSForegroundColorAttributeName,
                        p, NSParagraphStyleAttributeName,
                        nil];
  AppendPlain(out, bar, a);
  EndBlock(out, font, p);
}

static void EmitCode(NSMutableAttributedString *out, const Node &n) {
  CGFloat indent = n.quote * kQuoteIndent + kCodeIndent;

  // Every line of a code block is its own paragraph, so putting the block's
  // spacing on the shared style would space *between* lines and set the code
  // double-spaced. Only the first line leads and only the last line trails.
  NSMutableParagraphStyle *p =
      [[[NSMutableParagraphStyle alloc] init] autorelease];
  [p setFirstLineHeadIndent:indent];
  [p setHeadIndent:indent];
  LockToGrid(p, 1);

  NSMutableParagraphStyle *first = [[p mutableCopy] autorelease];
  [first setParagraphSpacingBefore:6.0];

  NSMutableParagraphStyle *last = [[p mutableCopy] autorelease];
  [last setParagraphSpacing:8.0];

  NSMutableParagraphStyle *only = [[first mutableCopy] autorelease];
  [only setParagraphSpacing:8.0];

  NSFont *font = MonoFont();
  NSColor *fill = Rgb(0xF2, 0xF2, 0xEC);

  // A very large column count so highlight() does not wrap: the text view is
  // the thing that knows how wide it is.
  std::vector<render::Line> lines = render::highlight(n.text, n.lang, 100000);

  for (size_t i = 0; i < lines.size(); i++) {
    const render::Line &line = lines[i];

    bool isFirst = (i == 0);
    bool isLast = (i + 1 == lines.size());
    NSParagraphStyle *style = p;
    if (isFirst && isLast) { style = only; }

    else if (isFirst) { style = first; }

    else if (isLast) { style = last; }

    for (size_t s = 0; s < line.spans.size(); s++) {
      NSDictionary *a = [NSDictionary dictionaryWithObjectsAndKeys:
                            font, NSFontAttributeName,
                            CodeColorForStyle(line.spans[s].style),
                                NSForegroundColorAttributeName,
                            fill, NSBackgroundColorAttributeName,
                            style, NSParagraphStyleAttributeName,
                            nil];
      AppendPlain(out, Str(line.spans[s].text), a);
    }

    NSDictionary *nl = [NSDictionary dictionaryWithObjectsAndKeys:
                           font, NSFontAttributeName,
                           fill, NSBackgroundColorAttributeName,
                           style, NSParagraphStyleAttributeName,
                           nil];
    AppendPlain(out, @"\n", nl);
  }
}

// Consecutive TableRow nodes, laid out monospaced and padded to a common width
// per column. NSTextTable exists on 10.5 but is fiddly and heavy; a monospace
// grid is legible, cheap, and matches what the terminal front end shows.
static size_t EmitTable(NSMutableAttributedString *out,
                        const std::vector<Node> &nodes, size_t start) {
  size_t end = start;
  while (end < nodes.size() && nodes[end].kind == md::Block::TableRow) end++;

  // Plain text of every cell, so the column widths can be measured.
  std::vector<std::vector<std::string> > grid;
  size_t columns = 0;
  for (size_t r = start; r < end; r++) {
    std::vector<std::string> row;
    for (size_t c = 0; c < nodes[r].cells.size(); c++) {
      std::string cell;
      for (size_t k = 0; k < nodes[r].cells[c].size(); k++)
        cell += nodes[r].cells[c][k].text;
      row.push_back(cell);
    }
    if (row.size() > columns) { columns = row.size(); }
    grid.push_back(row);
  }

  std::vector<size_t> width(columns, 0);
  for (size_t r = 0; r < grid.size(); r++)
    for (size_t c = 0; c < grid[r].size(); c++)
      if (grid[r][c].size() > width[c]) width[c] = grid[r][c].size();

  CGFloat indent = nodes[start].quote * kQuoteIndent + kCodeIndent;
  NSMutableParagraphStyle *p =
      [[[NSMutableParagraphStyle alloc] init] autorelease];
  [p setFirstLineHeadIndent:indent];
  [p setHeadIndent:indent];
  [p setLineBreakMode:NSLineBreakByClipping];
  LockToGrid(p, 1);

  NSFont *mono = MonoFont();
  NSFont *bold = [[NSFontManager sharedFontManager] convertFont:mono
                                                    toHaveTrait:NSBoldFontMask];

  for (size_t r = 0; r < grid.size(); r++) {
    const Node &node = nodes[start + r];
    NSMutableParagraphStyle *rp = p;
    if (node.table_start) {
      rp = [[p mutableCopy] autorelease];
      [rp setParagraphSpacingBefore:6.0];
    }

    if (node.table_end) {
      rp = [[rp mutableCopy] autorelease];
      [rp setParagraphSpacing:6.0];
    }

    std::string text;
    for (size_t c = 0; c < columns; c++) {
      std::string cell = c < grid[r].size() ? grid[r][c] : std::string();
      if (c) { text += "  "; }
      text += cell;
      // Guarded: width[c] is the maximum, so this cannot underflow, but the
      // codebase has been bitten by size_t padding arithmetic before.
      if (width[c] > cell.size()) text.append(width[c] - cell.size(), ' ');
    }

    NSDictionary *a = [NSDictionary dictionaryWithObjectsAndKeys:
                          node.header ? bold : mono, NSFontAttributeName,
                          Rgb(0x20, 0x20, 0x20), NSForegroundColorAttributeName,
                          rp, NSParagraphStyleAttributeName,
                          nil];
    AppendPlain(out, Str(text), a);
    AppendPlain(out, @"\n", a);

    // A ruled line under the header, drawn with the same monospace metrics so
    // it lines up with the columns above it.
    if (node.header) {
      std::string rule;
      for (size_t c = 0; c < columns; c++) {
        if (c) { rule += "  "; }
        rule.append(width[c], '-');
      }
      NSDictionary *ra = [NSDictionary dictionaryWithObjectsAndKeys:
                             mono, NSFontAttributeName,
                             Rgb(0xA0, 0xA0, 0xA0), NSForegroundColorAttributeName,
                             rp, NSParagraphStyleAttributeName,
                             nil];
      AppendPlain(out, Str(rule), ra);
      AppendPlain(out, @"\n", ra);
    }
  }

  return end;
}

// ---------------------------------------------------------------------------

NSAttributedString *PPAttributedFromMarkdown(const std::string &markdown) {
  NSMutableAttributedString *out =
      [[[NSMutableAttributedString alloc] init] autorelease];
  std::vector<Node> nodes = md::parse(markdown);

  size_t i = 0;
  while (i < nodes.size()) {
    const Node &n = nodes[i];

    switch (n.kind) {

      // A run of table rows is laid out together so the columns can align.
      case md::Block::TableRow:
        i = EmitTable(out, nodes, i);
        continue;

      case md::Block::Heading:
        EmitHeading(out, n);
        break;

      case md::Block::Code:
        EmitCode(out, n);
        break;

      case md::Block::Bullet:
      case md::Block::Numbered:
        EmitList(out, n);
        break;

      case md::Block::Rule:
        EmitRule(out, n);
        break;

      // Paragraph, and anything added to the model later.
      default:
        EmitParagraph(out, n);
        break;
    }

    i++;
  }

  return out;
}
