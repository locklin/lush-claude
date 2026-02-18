Using claude to clean up the lush1 stuff:

Opening prompt:

-----

I have a small programming language written in C. It contains the distant
ancestor of all modern neural nets. It is called lush, and lives in
~/src/claude-sandbox/lush

To describe what it does, it contains a small lisp dialect which is
interpreted, a strongly typed subset of the language which is
compilable, and a way of embedding C and C++ code into sources which
allow them to be compiled and linked to the language dynamically using
binutils. The interpreted language has a nice GUI object system called
ogre, and also a literate programming style which allows people to
decorate their code with comments that get automatically parsed into a
document system that can be accessed using ogre. The interpreted
language allows for interactive use, and the compiled version and
ability to call C++ and C from the interpreted language allows one to
speed up "hot spots" in the work.

The code is so old, it was written in 32 bit times, and so, even
though it compiles and runs on 64 bit machines, the data structures such as
-idx2- (and probably everything else) are not 64 bit clean, and will do
peculiar things if you try to put a larger than 32-bit addressable
matrix into this structure. This is just one data structure; there
will be many more, and all of the internal plumbing will need to be
checked for 64 bit safety. Make some suggestions and a list of
proposed changes to make the whole thing 64 bit clean. You can add
test scripts if that is helpful in unearthing 32-64 bit problems, but
don't alter the core functions of the interpretor yet. Just write out
a detailed plan you can execute later, with feedback from me as to
what pieces to work on first. Rank them by difficulty and impact. Give
alternative approaches and rank them by risk if there are some. One
alternative, and probably a good step for debugging is to add warnings
around all the non-64 bit clean parts, so if you try to allocate a
bigger than 32bit -idx2- (for example) the interpretor will warn you
that what you are about to do might not work properly.

There are also dark corners of this programming language which may not
work properly, but the core functionality seems to be functioning in
32 bit mode at least, even when you call 64 bit libraries. We can
ignore the dark corners for now, but if you spot anything
questionable, write it down as notes.


Lush also depends on old libraries which may not be well maintained,
such as binutils. If you spot anything questionable here, write it
down as notes, and offer potential mitigations.

The build is also written in an antiquated style, but
I'd like to keep it in its present form using old timey
Autoconf GNU tools. In this case you can simply "./configure" then
"make" and it should build Lush. If there's a way to run the full gnu
autoconf and get a good result, make suggestions and add them to the
plan, but don't alter anything.

Finally, the original lush sources can be found here:
https://github.com/locklin/lush-code
It is a machine translation of a SVN repo, showing the full history of
the lush project which dates back 25 years. There is a branch in it
called lush2 which was at one point somewhat working and somewhat 64
bit clean, but the author of this is missing in action, and this code
no longer compiles. It is also sufficiently different to be a
different programming language. Still, some of the ideas in it may be
of use in this project of updating lush to be 64 bit clean. The directory
in ~/src/claude-sandbox/lush corresponds to
https://github.com/locklin/lush-code/tree/master/lush1/trunk and as it
presently compiles and runs, this is the part of the codebase I want
you to fix. The older historical pieces may be of interest also.

It is a fairly large project, I think, to update to 64 bit clean with
modern API usage. So rather than making any changes, I want a plan for
fixes, as well as notes on interesting things you've observed that may
be of concern for later. So, the two work outputs will be
Claude-plan.md and Claude-notes.md. Ideally Claude-plan.md can be
edited by me and executed by you later.
