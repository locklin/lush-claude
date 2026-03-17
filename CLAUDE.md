For this project, we have the Lush programming language. 
It is a lisp dialect with a powerful gui, a helptool, a compilable subset and the ability to compile and link to C code embedded in the language.
Its origins are in the 1980s so it was originally a 32 bit language. Claude has mostly updated it to be 64 bit clean, and has added a number of 
features to the package manager. Look in claude-notes directory for summaries of work done thus far; most of it is recorded there.
One very important feature for the Lush packages is the helptool format in claude-notes/helptool-instructions.md -this should always be loaded 
in the context window, and all new work in the Lush packages should be well documented according to these directives.

Claude is sandboxed on this machine and doesn't have global write permissions; never ask for access to directories you are not given access to.


## LAnguage notes
- `t` is a reserved boolean literal in Lush — never use it as a variable name
- `regex-quote` does not exist in Lush — use alternative string escaping
- `for*` is not valid Lush syntax — use `for`
- Lush uses 1-based indexing for `mid` (string substring)
- `cadddr`/`cddddr` ARE defined in wire.lsh — do not replace them
- `boundp`, `return`, `objectp`, `is-a`, `ceil` may not be available as Lush builtins — verify before using
- Zero-length matrices may not be supported — always check
- `error` syntax in Lush differs from other Lisps — verify the exact calling convention
- C bridge string access needs explicit casting

## Environment & Sandbox
- The installed Lush binary is at `/usr/local/bin/lush`, NOT `src/lush`
- This environment uses blaude/bubblewrap sandboxing, NOT standard Claude sandbox or Docker containers
- Network access is restricted inside the sandbox — do not attempt network calls or expect to kill bwrap processes from inside
- When editing files, verify you are editing the ACTIVE copy (system install vs workspace) before making changes
- Data directories default to `/datafast1/experiment/` paths — always confirm the correct path, never default to /tmp
## Planning Documents
- When asked to write a plan, make it CONCRETE: include exact function signatures, file paths, code snippets, and specific line-level changes
- Never submit a vague/high-level plan — the user will reject it
- When creating multi-phase plans, each phase should be independently implementable and testable

## Package Refactoring Rules
- When eliminating a package (e.g., csvread → datatable), FULLY DELETE the old package — no shims, no backwards-compatibility wrappers
- Update ALL documentation references across the entire codebase when removing or renaming a package
- Always verify actual dependency chains in code before claiming something can be made optional
## Testing
- Always run the full test suite after implementation changes — do not assume tests pass
- When tests fail, read the ACTUAL error output carefully before guessing at fixes
- Test thresholds for statistical/numerical code must account for small sample sizes
- When bash tail/grep fails to capture test output, try running tests directly and reading output files
