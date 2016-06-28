Scoped properties configuration library
=======================================

This project provides a lightweight and flexible library for parsing
configuration files built of properties (key/value pairs) which may reside
inside "scopes", that is blocks of configuration with assigned name and an
optional type. Scopes may constitute hierarchical nesting tree of an arbitrary
(configurable) depth.

The library API consists of small but powerful set of C functions for read/write
access of configuration file elements (properties/scopes). There is possible to
access an element located inside a specific scope when its location (expressed
by path) is known OR to discover a configuration file structure by iterating
over its elements located in some scope (or the global scope).

The library provides unique functionality to parse only a specific scope (block
of configuration), therefore highly reducing an overhead needed by frequent
access to some limited configuration block inside a larger configuration file.

The library is inspired by:

 - Microsoft INI files API: `GetPrivateProfileXXX()`, `WritePrivateProfileXXX()`
   family of functions with their simplicity of usage, sections and basic
   iteration functionality.
 - C language grammar as a base for scopes and properties definition syntax.
 - C++ namespace notion, as a sum of sets identified by the same name.

Configuration file format
-------------------------

"One word from Chairman Mao is worth ten thousand from others" --Lin Biao

The same is true with a good example:

    # A comment starts with '#' up to the end of line.
    # This property is located in the global scope.
    prop1 = value

    # Quotation marks required in the property name to avoid parsing problem.
    # A property value finishes at the end of a line or at a semicolon (unless
    # the library is configured with CONFIG_NO_SEMICOL_ENDS_VAL).
    "property 2" = some other value;

    # Scope with a type and name.
    scope_type scope_name
    {
        # 2 properties w/o their values (defined in the same line).
        a; b=;

        # Scope w/o a type. If no quotation marks had been used, the scope name
        # would be "name" with type "scope".
        "scope name"
        {
            c = property "c" value \
                with line \
                continuation;

            # Property contains escaped characters: semicolon (\;), LF (\n),
            # space (\ ) and backslash (\\).
            d = \;abc\n\ \\
        }

        # Property names may contain arbitrary characters. In this example
        # quotation marks are required since '{', '}' are parser's reserved
        # tokens. Additionally tab characters were escaped by \t.
        "{\tprop name w/o a value\t}";
    }

    # This scope is a continuation of the scope above (with the same name and
    # type). The library allows to sum (concatenate) all scopes with the same
    # name and type as a single compound scope (so called split scope). This
    # functionality is similar to the C++ namespaces.
    scope_type scope_name
    {
        # These 2 properties duplicate ones defined above.
        a=val;
        b=val;

        c;

        # Scope with "type" and "name" and an empty body.
        type name;

        # Same as above (previous scope continuation).
        type name {}
    }

Path-based addressing
---------------------

The library provides path-based method of scope addressing to reach for a
specific element inside it (similarly to path and file analogous).

Basing on the previous example:

    global scope
        -> "/"

    1st compound (split) scope in /
        -> "/scope_type:scope_name"
             or
           "/scope_type:scope_name@*"

    1st part of the split scope above
        -> "/scope_type:scope_name@0"

    2nd part of the split scope above
        -> "/scope_type:scope_name@1"
             or
           "/scope_type:scope_name@$", as the last scope of such name and type

    1st scope in split /scope_type:scope_name
        -> "/scope_type:scope_name/:scope name"

    2nd scope in split /scope_type:scope_name
        -> "/scope_type:scope_name/type:name", which as a split scope consist of:
           "/scope_type:scope_name/type:name@0"
            and
           "/scope_type:scope_name/type:name@1", both with empty bodies.

API specification
-----------------

The library provides two sets of API:

 - Low level parser API with header: `./inc/sprops/parser.h`,
 - High level functional API with header: `./inc/sprops/props.h`.

The first set of API defines low level, parser specific functionality and is
not intended to be used unless, for some reason, there is a need for direct
cooperation with the parser. The interface defines set of callbacks which allow
an application to be informed about grammar reductions.

The second set of API constitutes the proper read/write access interface (its
implementation bases on the low level parser API). The API allows addressing
elements inside their scopes AND to discover a configuration structure by
iterating over its content.

NOTE: Basically, the API treats properties values as strings built of arbitrary
characters which are not interpreted. For user convenience there have been
provided three simple functions to interpret strings as integers, floats and
enumerations, namely:

    sp_get_prop_int()
    sp_get_prop_float()
    sp_get_prop_enum()

Lists may be easily emulated by iterating over dedicated scopes content.

Refer to the mentioned header files for complete API specification, and the unit
tests located in `./ut` directory for an example of usage.

Compilation
-----------

Prerequisites:

 - GNU Make,
 - Bison parser generator of version 3 or higher (only in case of regenerating
   `parser.c` grammar definition file).

Compilation:

    make

produces static library `libsprops.a` which may be linked into an application.
Unit tests are contained in `./ut` directory and are launched by

    make -C./ut run

Notes
-----

 - Full support for UNIX (LF), Windows (CR/LF) and Legacy Mac (CR) end-of-line
   markers.
 - The library uses ONLY standard C library API and shall be ported with a
   little effort for any conforming platforms.
 - Memory allocation is performed ONLY by the generated grammar parser code
   for grammar reductions. Bison parser allows a flexible way for configuring
   such allocations e.g. via stack `alloca(3)` (used by the library) OR heap
   `malloc(3)`. This may be useful for porting to some constrained embedded
   platforms. See the Bison parser generator documentation for more details.
 - The API is fully re-entrant. No global variables are used during the parsing
   process.
 - The library is thread safe in terms of all library objects except API passed
   file-objects (that is file handles for read access and physical files for
   write access). Since there is no effective way to ensure such file-objects
   synchronization on the library level, the application is responsible to handle
   this issue. This may be done via standard thread synchronization approach or
   by other means, e.g. if many threads read a single configuration file, each
   of them may use its own read-only file handle to access the file in a thread
   safe way (such approach is much more effective than the classical mutext
   usage).

License
-------

2 clause BSD license. See LICENSE file for details.