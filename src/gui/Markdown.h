// Markdown.h -- markdown as attributed text for the Cocoa transcript.
//
// The structure comes from src/mdparse.cpp; this half only decides how each
// piece looks. Splitting it that way keeps the intricate parsing out of GCC's
// Objective-C++ front end and lets --selftest cover it.
//
// Deliberately not a WebView: Leopard's WebKit is ancient and JavaScript is
// slow on a G5. This is AppKit text all the way down.
#import <Cocoa/Cocoa.h>

#include <string>

// Render a whole markdown document.
NSAttributedString *PPAttributedFromMarkdown(const std::string &md);

// One line of body text: the vertical rhythm the whole transcript is set on.
//
// Paragraph spacing is a whole multiple of it, so successive paragraphs stay on
// the same grid, which is what lets ruled paper line up with the writing.
CGFloat PPMarkdownLineHeight(void);

// Distance from the top of a line of body text down to its baseline. Ruled
// paper needs it: a rule belongs just under the baseline, where the writing
// sits on it, not through the middle of the letters.
CGFloat PPMarkdownBaselineOffset(void);

// Attributes for text that has arrived but whose block has not closed yet.
// Streaming appends runs of this and only re-renders once a block completes,
// so a code block is highlighted once rather than once per token.
NSDictionary *PPMarkdownStreamAttributes(void);

// Attributes for the "You" / "ppcode" speaker headings and for chrome lines,
// so the whole transcript shares one set of fonts and colours.
NSDictionary *PPMarkdownSpeakerAttributes(NSColor *color);

// A UTF-8 C string as an NSString.
//
// GCC compiles @"..." into an NSConstantString wrapping the literal's raw bytes,
// and NSConstantString reads them as ASCII -- so a UTF-8 ellipsis inside a
// literal arrives as three garbage characters. Anything non-ASCII has to be
// built at runtime instead. See knowledge/60-objcpp-gcc.md.
NSString *PPUTF8(const char *utf8);
