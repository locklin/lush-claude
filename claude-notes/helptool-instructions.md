# Lush Helptool Documentation Format

## Overview

Lush has a self-documenting system where `#?` markers in `.lsh` source files
define help entries viewable via `(helptool)` or `(apropos)`.  The parser
lives in `lsh/libstd/ldoc.lsh` and `lsh/libstd/brace.lsh`.

## Entry Markers

A `#?` at the start of a line introduces a help entry.  What follows
determines the entry type:

```
#? **** Top-Level Section Title     (book chapter)
#? ***  Section Title               (section)
#? **   Subsection Title            (subsection)
#? *    Sub-subsection Title        (sub-subsection)
#? (function-name <arg1> <arg2>)    (function documentation)
#? variable-name                    (variable documentation)
#? * ClassName                      (class documentation)
#? (new ClassName <arg1>)           (constructor documentation)
#? (==> <obj> method-name <args>)   (method documentation)
```

More asterisks = higher in the hierarchy.  The helptool tree nests
lower-level entries under higher-level ones.

## Entry Body

The body follows immediately after the `#?` line and consists of
consecutive `;;` comment lines.  The body ends at the next `#?` line,
a non-`;;` line, or a blank line.

```lisp
#? (my-function <x> <y>)
;; Compute the sum of <x> and <y>.
;; Returns a number.
(de my-function (x y) (+ x y))
```

Angle brackets `<x>` in the body are rendered bold/highlighted (use for
parameter names and short code literals).

## CRITICAL RULE: Only Use Valid Brace Tags

The brace parser (`brace.read-tag` in `lsh/libstd/brace.lsh:182`) rejects
any tag not in its hardcoded list.  Common HTML tags that are **NOT** valid
Lush brace tags include: `<ol>`, `<table>`, `<tr>`, `<td>`, `<th>`,
`<span>`, `<em>`, `<strong>`, `<hr>`, `<dl>`, `<dt>`, `<dd>`,
`<blockquote>`, `<sup>`, `<sub>`, `<section>`, `<nav>`, `<article>`.

Using any of these produces:
`*** error : illegal tag in brace construct: <tagname>`

**The complete list of valid tags** (plus their UPPERCASE equivalents):

- Formatting: `p`, `pre`, `code`, `li`, `br`, `tt`, `b`, `i`, `u`, `c`,
  `font`, `center`, `img`, `ul`, `div`
- Headings: `h1`, `h2`, `h3`
- Links: `hlink`, `see`
- Conditional: `if-html`, `if-latex`, `if-ogre`, `if-text`
- Metadata: `author`, `symbol`, `location`, `keywords`, `date`, `desc`,
  `title`, `type`
- Global: `lit`, `meta`, `ex`

**Common mistake:** using `{<ol>}` for ordered lists.  Lush has no ordered
list tag — use `{<ul>}` instead.  (Bug found and fixed in
`packages/datatable/datatable-csv.lsh:238`.)

## CRITICAL RULE: Dot-Tags Are DEPRECATED — Use Brace-Tags Only

**NEVER generate new dot-tags** (`;;.PP`, `;;.SEE`, `;;.TYPE`, `;;.CODE`,
etc.) in any new or modified documentation.  Dot-tags are a legacy syntax
from early Lush.  **All new `#?` entries MUST use brace-tag syntax
exclusively.**

Many existing Lush packages (pre-dating Claude's work) use dot-tags
throughout.  Those entries are fine as-is — do not bulk-convert legacy
code.  But when writing NEW entries, or editing existing entries to add
brace-tag features (like `{<b>}`, `{<ul>}`, `{<see>}`), you MUST ensure
the entire entry uses brace-tags only.

### The Mixing Rule

**Within a single entry body, you must use EITHER dot-tags OR brace-tags,
NEVER both.**  The parser (`ldoc.lsh:read-help-body`) scans each entry:

- Sets `oldstylep` if ANY line starts with `.[A-Z]` (a dot-tag)
- Sets `newstylep` if ANY line contains `{<` (a brace-tag)
- If BOTH are set → error: "document uses a mixture of dot-tags and brace-tags"

Different entries in the same file CAN use different styles — the check
is per-entry, not per-file.  But in practice, use brace-tags for every
new entry.

### Checklist Before Committing Documentation

1. **New entries**: Verify zero dot-tags.  Use `;;` for paragraph breaks
   (not `;;.PP`), `{<see>}` for cross-refs (not `;;.SEE`), etc.
2. **Edited entries**: If you added any `{<...>}` brace-tag, grep the
   entry for `;;.` lines and convert them per the table below.
3. **Quick test**: `grep -n '^\;\;\.PP\|^\;\;\.SEE\|^\;\;\.TYPE' file.lsh`
   in files you touched — there should be zero hits in new entries.

## Brace-Tag Syntax (Preferred)

### Inline Tags

```
{<b> bold text}              Bold/emphasis
{<c> (some-code)}            Inline code (monospace)
<angle-bracket-text>         Bold/highlighted (parameter names)
```

### Block Tags

```
{<code>                      Code block (preformatted, blue)
  (some code here)
  (more code)
</code>}

{<pre>                       Preformatted text block
  some preformatted text
</pre>}
```

### List Items

**CRITICAL: Always wrap `{<li>}` items in explicit `{<ul>}` blocks!**

```
{<ul>
{<li> First item}
{<li> Second item}
{<li> Third item}
}
```

The parser has an auto-wrapping function (`brace.splice-ul` in
`lsh/libstd/brace.lsh:389`) that is SUPPOSED to wrap bare `{<li>}`
items in `{<ul>}`, but it crashes on the `-1` spacing tokens that
`brace.read-text` inserts for newlines between items.  The crash is:
`*** car : not a list : -1`.  Using explicit `{<ul>}` blocks avoids
the crash because `splice-ul` skips `<ul>` nodes entirely (line 399).

In comment form:
```lisp
;; {<ul>
;; {<li> First item}
;; {<li> Second item}
;; {<li> Third item}
;; }
```

### Cross-References and Metadata

```
{<see> (other-function <args>)}     Cross-reference link
{<see> Section Title}               Link to a section

{<type> DE}                         Entry type (DE, DX, DMC, VAR, CLASS, MSG)
{<location> path/to/file.lsh}       Source file location
{<author> Name}                     Author
{<desc> Short description}          One-line description
```

### Paragraph Breaks

A blank `;;` line (just `;;` with nothing after it) produces a paragraph
break in brace mode.  Do NOT use `;;.PP` or `;;.P` — those are dot-tags.

## Dot-Tag Syntax (DEPRECATED — Reference Only)

**Do NOT use dot-tags in new code.**  This section exists solely so you
can READ and UNDERSTAND legacy Lush documentation that predates brace-tags.
If you need to edit a legacy entry and add brace-tag features, convert
the entire entry to brace-tags using the conversion table below.

Dot-tags work ONLY in entries with NO `{<` brace-tags.

```
;;.PP  or  ;;.P           Paragraph break
;;.BR                     Line break
;;.CODE ... ;;.P          Code block (until next dot-tag)
;;.PRE  or  ;;.VP         Preformatted text (until next dot-tag)
;;.LI  or  ;;.IP          List item (until next dot-tag)
;;.SEE <reference>         Cross-reference
;;.TYPE <type>             Entry type metadata
;;.AUTHOR <name>           Author metadata
;;.DESC <description>      Description metadata
;;.HLINK <url-or-entry>   Hyperlink
;;.EX <expression>         Evaluated example
;;.IMG <path>              Image inclusion
```

## Converting Dot-Tags to Brace-Tags

When editing a legacy entry that uses dot-tags and you need to add any
brace-tag feature, you MUST convert ALL dot-tags in that entry.
Conversion reference:

| Dot-Tag | Brace Equivalent |
|---------|-----------------|
| `;;.PP` / `;;.P` (standalone) | `;;` (blank comment line) |
| `;;.VP` | `;;` (blank comment line) |
| `;;.CODE` ... `;;.P` | `;; {<code>` ... `;; </code>}` |
| `;;.PRE` ... `;;.P` | `;; {<pre>` ... `;; </pre>}` |
| `;;.LI text` | `;; {<li> text}` |
| `;;.SEE ref` | `;; {<see> ref}` |
| `;;.TYPE type` | `;; {<type> type}` |
| `;;.AUTHOR name` | `;; {<author> name}` |
| `;;.BR` | (just use a blank `;;` line) |
| `;;.END` | (remove — not a standard tag) |

## Complete Example (Brace-Style)

```lisp
#? *** MyPackage: Widget Library
;; A library for creating and manipulating widgets.
;;
;; Features:
;; {<ul>
;; {<li> Create widgets with {<b> make-widget}.}
;; {<li> Resize with {<b> widget-resize}.}
;; {<li> Supports nested widget hierarchies.}
;; }
;;
;; Example:
;; {<code>
;;  (libload "mypackage/mypackage")
;;  (let ((w (make-widget "button" 100 50)))
;;    (widget-resize w 200 100))
;; </code>}
;; {<see> (make-widget <type> <w> <h>)}

#? (make-widget <type> <w> <h>)
;; Create a new widget of the given <type> with dimensions <w> x <h>.
;; <type> must be one of: "button", "label", "panel".
;; Returns an opaque widget handle.
;; {<see> (widget-resize <widget> <w> <h>)}
(de make-widget (type w h) ...)

#? * WidgetContainer
;; A container that holds child widgets.
;; {<type> CLASS}

#? (new WidgetContainer <capacity>)
;; Create a new container that can hold up to <capacity> children.

#? (==> <container> add-child <widget>)
;; Add <widget> as a child of this container.
;; Returns the child index.
```

## What NOT to Document

- Internal/private functions (names starting with `_`) generally do not
  need `#?` entries, though it does not hurt to add them.
- Test files in `tests/` directories do not need `#?` entries.
- Config files (`*-config.lsh`) may optionally have a `#? **` subsection.

## Two Documentation File Formats

The help system reads `#?` entries from two file types:

- **`.lsh` files** — documentation embedded inline in source code (what we write).
  The `#?` markers and `;;` body lines sit alongside function definitions.
- **`.hlp` files** — standalone documentation files (no code, just help entries).
  The `#?` markers and body lines are bare (no `;;` prefix needed).

Both formats use the same `#?` marker syntax and brace/dot-tag formatting.
The help system (`lsh/libstd/help.lsh`) reads both identically via
`read-help-headers` and `read-help-entry`.

**There is no tool to extract `.lsh` docs into `.hlp` format.**  The two
formats are parallel inputs, not a source/target pipeline.  For our
packages we use inline `.lsh` documentation exclusively.

### Export to other formats

The help system can render documentation to HTML or LaTeX:

```lisp
(make-html-manual)    ;; export full Lush manual to HTML
(make-latex-manual)   ;; export full Lush manual to LaTeX
```

These render FROM `.hlp`/`.lsh` entries, reading through `help-book`.

## File-Level Structure

A well-documented `.lsh` package file should have:

1. `;;;` header comments (file description, not helptool-visible)
2. `(libload ...)` declarations
3. `#? *** Package: Title` — top-level section header with overview
4. `#? ** Subsection` — logical groupings
5. `#? (function ...)` / `#? variable` — individual entries before each definition
6. Each public function/class/method should have a `#?` entry
