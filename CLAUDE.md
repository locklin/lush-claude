For this project, we have the Lush programming language. 
It is a lisp dialect with a powerful gui, a helptool, a compilable subset and the ability to compile and link to C code embedded in the language.
Its origins are in the 1980s so it was originally a 32 bit language. Claude has mostly updated it to be 64 bit clean, and has added a number of 
features to the package manager. Look in claude-notes directory for summaries of work done thus far; most of it is recorded there.
One very important feature for the Lush packages is the helptool format in claude-notes/helptool-instructions.md -this should always be loaded 
in the context window, and all new work in the Lush packages should be well documented according to these directives.

Claude is sandboxed on this machine and doesn't have global write permissions; never ask for access to directories you are not given access to.
