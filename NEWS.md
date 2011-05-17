libFirm 1.19.0 (1011-03-15)
---------------------------

* Includes "SSA-Based Register Allocation with PBQP"
* Improved Sparc backend
* New (optimistic) fixpoint based value-range propagation/bit analysis
* Code cleanup and refactoring
* Bugfixes

libFirm 1.18.1 (1010-05-05)
---------------------------

* Fix bug where stackframe was not always setup for -fno-omit-frame-pointer
* bugfixes in Asm handling

libFirm 1.18.0 (2010-04-15)
---------------------------

* Includes "Preference Guided Register Assignment" algorithm
* Experimental Value Range Propagation
* Loop Inversion and experimental Loop Unrolling code
* Simplified construction interface. Most node constructors don't need graph/block arguments anymore.
* Reworked type interface. Type names are optional now. Support for additional linkage types that among others support C++ 'linkonce' semantics now.
* Small changes in constructors and node getters/setters (mostly adding 'const' to some getters)
* code cleanup, smaller improvements in API specification
* bugfixes

libFirm 1.17.0 (2009-05-15)
---------------------------

* bugfixes
* advanced load/store optimisation which hoists loads out of loops
* Internal restruturing: Alot of node structures are automatically generated
   from a specification file now.
* Add support for multiple calling conventions
* New experimental support for reading and writing programgraphs to disk
* Support and optimisations for trampolines
* fix PIC support

libFirm 1.16.0 (2009-01-28)
---------------------------

* bugfixes
* support for builtin nodes

libFirm 1.15.0 (2008-12-01)
---------------------------
* bugfixes

libFirm 1.14.0 (2008-11-22)
---------------------------

* Implementation of Clicks Combined Analysis/Optimisations
* New switch lowering code
* support for global asm statements
* improved asm support
* PIC support for Mac OS X
* New register pressure minimizing scheduler
* Improvements to spill algorithm
* fix endless loop problems
* further improve inlining heuristics
* improve peephole optimisations for x86
* bugfixes

libFirm 1.13.0 (2008-07-31)
---------------------------

* VanDrunen's GVN-PRE fixed
* operator strength reduce fixed and improved
* fixed 64bit code generation for some rare compare cases
* better tailrecursion optimization: handles x * func() and x + func()
* improved inliner: better heuristics for inlining, can now inline recursive calls
* improved spiller
* lowering of CopyB nodes
* better memory disambiguator
* float->64bit conversion fixed for x87
* removed old verbosity level based debugging: all modules use the new debug facility
* Improved Confirm based optimization and conditional evaluation (using Confirm nodes)
* BugFixes: tail recursion, load/store optimization, lowering of structure return, conditional
  evaluation, removal of unused methods
* reduced numer of indirections for backend operation
* ia32 Backend: supports more CPU architectures
* ARM Backend: fixed frame access
* support for special segments (like constructors, destructors)

libFirm 1.12.1 (2008-02-18)
---------------------------

* bugfixes for new style initializers with bitfield types
* make lowerer look at constant initializers too

libFirm 1.12.0 (2008-02-14)
---------------------------

* dependency on libcore and libobstack dropped
* there's an alternative easier to use way to construct compound initializers
* bugfixes
* improved support for exceptions
* speed improvements
* optimisation of known libc functions

libFirm 1.11.0 (2008-11-05)
---------------------------

* Lots of bugfixes
* Compilation speed improved
* Completely improved and rewritten handling of x86 address mode
* Optimized Mul -> Lea,Shift,Add transformation
* 64bit operations fixed and improved
* More local optimisations
* New backend peephole optimisations
* Explicit status flag modeling (only for x86 for now)
* Improvements of Load/Store optimisation and alias analysis
* All C benchmarks from Spec CINT2000 work now (with our edg frontend)
