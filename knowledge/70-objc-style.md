---
title: Objective-C formatting style
priority: 15
tags: [objc, style, formatting]
---

## Objective-C style

Brielle's `OBJC_FORMATTING.md` governs Objective-C written for her. Follow it.
The layout rules below are the operative ones; the full document lives at
`~/OBJC_FORMATTING_guess.md` on the G5 and is the authority where this summary
is ambiguous.

**A Swift companion (`SWIFT_FORMATTING.md`) exists and the section numbering of
the two mirrors each other.** Swift cannot run on Leopard, so it is not a target
here — but if Swift is ever generated on this machine, it must follow that guide
rather than default Swift conventions.

### Governing principles

1. Cognitive load per line beats column count.
2. Wrap the way a human would, not the way a formatter would.
3. Write for the reader who never opens this file.
4. Objective-C is visually noisier than Swift, so whitespace works harder.
   Brackets, colons and pointer stars compete for attention; every blank line
   and hoisted local buys back signal the syntax spent.

### Layout

| Rule | Value |
| --- | --- |
| Indent | **2 spaces**, never tabs |
| Soft line limit | **90 columns** |
| Pointer star | binds to the name: `NSString *label` |
| Trailing whitespace | none, including on blank lines |
| End of file | exactly one newline |
| Consecutive blank lines | never more than one |

### Vertical rhythm

- **Blank line before `return`**, unless the body is a single expression.
- **Declaration blocks**: consecutive declarations group together, blank line
  after; blank line before unless it opens the scope.
- **Control flow** (`if`, `for`, `while`, `do`, `switch`, `@try`,
  `@autoreleasepool`): blank line above and below unless it opens or closes the
  scope.
- **Type members**: one blank line between *every* member — properties, methods,
  enumerators. No exception for short or private ones.
- **Cleanup code**, being Objective-C's stand-in for `defer`, gets blank lines
  around it plus a comment saying what is reclaimed and why it must happen on
  every exit path.

### Braces

Never one-true-brace. `else`, `else if`, `@catch` and `@finally` begin a new
line **with a blank line** between them and the preceding `}`:

```objc
if (variance > 1.0) {
  badge = @"warn";
}

else {
  badge = @" ";
}
```

The `guard` analog is an early-return `if` with an inverted condition. Short ones
go on one line — `if (!self) { return nil; }` — and stack as a block. When the
condition list overflows, break it with the operator trailing and close with
`) {`.

### Switch

`case` sits at the **same indentation as `switch`**. Every case gets a comment
and a blank line before it. Brace a case body that declares storage. An
intentional fallthrough is commented as such. Omit `default:` when switching over
an enum so a new enumerator produces a warning.

### Method declarations

Colon-align the selector parts:

```objc
- (NSInteger)ingestPayload:(NSDictionary *)payload
                  calendar:(NSCalendar *)calendar
                     error:(NSError **)error;
```

When a long first keyword pushes the alignment past the column budget, degrade to
a flat 4-space continuation indent instead. Never break a signature partially.

### Message sends

Break with the **receiver alone on the opening line** and the selector parts
colon-aligned at a 2-space indent. Do not colon-align against the receiver, which
makes the alignment column depend on the receiver's length:

```objc
NSInteger inserted = [self.window
  ingestPayload:payload
       calendar:calendar
          error:&error];
```

**Nesting past two levels gets hoisted into named locals.** The locals are not
overhead, they are the documentation.

### Expressions

- Boolean chains: one condition per line, operator trailing, comment each.
- Prefer the elvis form `x ?: fallback` over a full ternary that repeats itself.
- Nested ternaries only when every branch is a literal and the whole thing fits.
- Collection literals break with a trailing comma on the last element.

---

## What of this actually compiles on Leopard

The **layout rules above transfer completely**. Many of the *language features*
in the guide's examples do not exist here, because they postdate this platform by
years. When writing for 10.5, translate:

| Guide uses | On Leopard write |
| --- | --- |
| `NS_ASSUME_NONNULL_BEGIN`, `nullable`, `_Nullable` | omit entirely; document nullability in the doc comment |
| Generics: `NSDictionary<NSString *, id> *` | `NSDictionary *` |
| `@[...]`, `@{...}`, `@(x)` literals | `[NSArray arrayWithObjects:..., nil]`, `[NSDictionary dictionaryWithObjectsAndKeys:...]`, `[NSNumber numberWithInt:x]` |
| `array[i]`, `dict[key]` subscripting | `objectAtIndex:`, `objectForKey:` |
| `NS_ENUM`, `NS_OPTIONS`, `NS_ERROR_ENUM` | plain `typedef enum { ... } Name;` |
| `instancetype` | `id` |
| `NS_DESIGNATED_INITIALIZER`, `NS_UNAVAILABLE` | omit; say so in the doc comment |
| `SomeClass.class` | `[SomeClass class]` |
| `@autoreleasepool { }` | `NSAutoreleasePool *p = [[NSAutoreleasePool alloc] init]; ... [p release];` |

Dot syntax for properties, `@property`/`@synthesize`, and fast enumeration are
Objective-C 2.0 and **are** available on 10.5 — but note that fast enumeration
does not work in GCC's Objective-C**++** front end (see the Objective-C++
document).

Because the guide's `switch` rule assumes `NS_ENUM` exhaustiveness warnings that
a plain C enum does not give, include `default:` on Leopard when the switched
value could grow, and say why in a comment.
