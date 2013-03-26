IronBee Changes                                                   {#CHANGES}
===============

IronBee v0.7.0
--------------

**Deprecations**

* The `ac` module is deprecated.  It will emit a warning if loaded.

**Documentation**

* Syntax added to all operators.

* Preface added.

**Build**

* libhtp is now configured as part of configure stage rather than build
  stage.  In addition, libhtp will make use of any configure options.  Use
  ``./configure --help=recursive`` to see libhtp specific configure options.

* Extensive cleanup regarding use of `CFLAGS`, `CXXFLAGS`, etc.  Those
  variables are now respected and may be specified at configure or make time.
  Several configure options used to control those variables have been removed
  in favor of directly setting them.

* Warning settings changed to `-Wall -Wextra`.  `-Werror` will be enabled on
  newer compilers (any clang or gcc 4.6 or later).

* Build system now compatible with automake 1.13.  In addition, IronBee will
  take advantage of the new parallel test harness if automake 1.13 is used.

* Configure now checks for `ruby`, `gem`, and `ruby_protobuf` gem if C++ code
  is enabled.

* Configure now checks for `libcurl` and `yajl` and only enabled RIAK support
  if present.

* Several unneeded checks removed.

**Configuration**

* Added `InspectionEngineOptions` to set the defaults for the inspection
  engine.

* Added `IncludeIfExists` directive to include a file only if it exists and is
  accessible.  This allows for inclusion of optional files.

**Engine**

* `ib_tx_t::data` has changed from a generic hash to an array indexed by
  module index.  This change puts it in line with per-module engine data and
  per-module context data.  `ib_tx_data_set()` and `ib_tx_data_get()` can be
  used by modules to read/write this data.

* Added RIAK kvstore.

* Several fixes to dynamic collections in the DPI.

* Lua rule support moved from the rule component to the Lua module.  The rules
  component gained support for modules to register arbitrary external rule
  drivers (see `ib_rule_register_external_driver()`), which the Lua module
  now uses.

* Data fields were cleaned up and refactored.  Notable changes to the public
  API include:

  * All capture related data routines have been moved to capture.h and begin
    `ib_capture` instead of `ib_data`.
  * Several transformation functions have been moved to transformation.h and
	to `ib_tfn` from `ib_data`.
  * All remaining data routines are now in `data.h` instead of `engine.h`.
  * All public `dpi` fields are now `data`.
  * To disambiguate, previous module data code has moved from `data` to
    `module_data`.

* Added managed collections which allow TX collections to be automatically
  populated / persisted.

* Added a core collection manager which takes one or more name=value pairs,
  and will automatically populate a collection with the specified name/value
  pairs.

* Added a core collection manager which takes a JSON formatted file,
  will automatically populate a collection from the content of the file.
  Optionally, the collection can persist to the collection, as well.

* Removed backward compatibility support for the `ip=` and `port=` parameters
  to the Hostname directive.

* Removed backward compatibility support for `=+` to the `SetVar` action.

* Logging overhaul.

  * For servers, use `ib_log_set_logger` and `ib_log_set_loglevel` to setup
    custom loggers.  Provider interface is gone.
  * For configuration writers, use `Log` and `LogLevel`; `DebugLog` and
    `DebugLogLevel` are gone`.  `LogHandler` is also gone.
  * For module writers, use `ib_log_vex` instead of `ib_log_vex_ex`.  Include
    `log.h` for logging routines.
  * For engine developers, logging code is now in `log.c` and `log.h`.

* LogEvents has been refactored to use a direct API rather than a provider.

* Added utility functions that wrap YAJL, using it to decode JSON into an
  `ib_list_t` of `ib_field_t` pointers, and to encode an `ib_list_t` of
  `ib_field_t` pointers into JSON.

* Added `@match` and `@imatch` operators to do string-in-set calculations.

* Added `@istreq`, a string insensitive version of `@streq`.

* Support for unparsed data has been removed from IronBee.

  * The `ib_conndata_t` type has been removed.
  * `ib_conn_data_create()` has been removed.
  * The `ib_state_conndata_hook_fn_t` function typedef has been removed.
  * The `ib_hook_conndata_register()` and `ib_hook_conndata_unregister()
`    functions have been removed.
  * The `ib_state_notify_conn_data_in()` and `ib_state_notify_conn_data_out()
`    functions have been removed.

* The libhtp library has been updated to 0.5.

**Modules**

* The `pcre` module has been updated to use the new transaction data API.

* The `pcre` module `dfa` operator now supports captures.

* Added a 'persist' module, which implements a collection manager that can
  populate and persist a collection using a file-system kvstore.

* Added a 'fast' module which supports rapid selection of rules to evaluate
  based on Aho-Corasick patterns.  See below and `fast/fast.html`.

* Added a module implementing libinjection functionality to aid in detecting
  SQL injection. This module exposes the `normalizeSqli` and the
  `normalizeSqliFold` transformations as well as the `is_sqli` operator.

* Added a module implementing Ivan Ristic's sqltfn library for normalizing
  SQL to aid in detecting SQL injection. This module exposes the
  `normalizeSqlPg` transformation.

* The `htp` module has been vastly reworked to work properly with libhtp 0.5.

**Fast**

* Added a variety of support for the fast rule system (the fast module
  described above is the runtime component).  Support includes utilities to
  suggest fast patterns for rules and for easy generation of the fast automata
  needed by the fast module.  See above and `fast/fast.html`.

**IronBee++**

* Moved catch, throw, and data support from internals to public.  These
  routines are not needed if you only use IronBee++ APIs but are very useful
  when accessing the IronBee C API from C++.

* Fixed bug with adding to `List<T>` where `T` was a `ConstX` IronBee++ class.

**Automata**

* Intermediate format and Eudoxus now support arbitrary automata metadata in
  the form of key-value pairs.  All command line generators include an
  `Output-Type` metadata key with value set to the output type as defined by
  `ee`.  `ee` now defaults to using this metadata to determine output type.
  This changes increments the Eudoxus format version and, as such, is not
  compatible with compiled automata from earlier versions.

* Eudoxus output callbacks are now passed the engine.

* Added `ia_eudoxus_all_outputs()` to iterate through every output in an
  automata.  `ee -L` can be used to do this from the command line.

* Added '\iX' to Aho Corasick patterns which matches upper case of X and
  lower case of X for any X in A-Za-z.

* Added '\$' to Aho Corasick patterns which matches CR or NL.

* Added union support to Aho Corasick patterns, e.g., `[A-Q0-5]`.

**Clipp**

* All generators except `pb` now produced parsed events.  Use `@unparse` to
  get the previous behavior.  But note that IronBee no longer supports
  unparsed events.

**Other**

* The old CLI (ibcli) has been removed.

* Removed FTRACE code.

* Various bug fixes and cleanup.

IronBee v0.6.0
--------------

**Build**

* IronBee++ and CLIPP are now built by default.  Use `--disable-cpp` to
  prevent.

* Build system now handles boost and libnids libraries better.  New
  `--with-boost-suffix` configuration option.

* Removed a number of unnecessary checks in configure.

* Included libhtp source, so this is no longer required.

**Engine**

* Enhanced support for buffering request/response data, including
  runtime support via the setflag action.

* Added initial support for persistent data. (see:
  `include/ironbee/kvstore.h`)

* Partial progress towards rework of configuration state transitions.
  Currently implicit.  Next version should be gone completely.

* Events can now be suppressed by setting the `suppress` field.

* Directory creation (`ib_util_mkpath`) rewritten.

**Rules, Configuration and Logging**

* Enhanced rule engine diagnostics logging (`RuleEngineLogData`,
  `RuleEngineLogLevel`).

* Simplified Hostname directive by moving IP/Port to a new
  Service directive.  For 0.6.x only, support the "ip=" and "port="
  parameters to the Hostname directive for backward compatibility with 0.5.x.

* Enhanced configuration context selection, which now takes Site,
  Service, Hostname and Location into account.

* Added an `InitVar` directive to set custom fields at config time.

* `SetVar` `=+` operator changed to `+=`.  Also added `-=` and `*=`.  For
  0.6.x only, support `=+` for backward compatibility with 0.5.x.

* Added floating point field type; removed unsigned field type.  Note that
  floating point values do not support eq and ne.

* The `ne` operator now correctly compares numbers.

* Initial support for implicit type conversions in operators.

* Fixed `pmf` operator so that relative filenames are based on
  config file location vs CWD.

* Enhanced PCRE matching to support setting limits.

* `AuditLogFileMode` now works.

* Default of `AuditEngine` is now `RelevantOnly`.

* Cleaned up audit log format, removing event level action and adding
  transaction level action, message, tags and threat level.

**Lua**

* Updated luajit code to v2.0.0.

* Enhanced Lua rule API with more access to internals.

**Modules**

* Enhanced GeoIP module to use O1/O01 country codes when
  lookups fail.

**Servers**

* Added support for regexp based header editing.

* Rewrote Apache httpd server module for httpd 2.4.

**Automata**

* Added IronAutomata framework for building, modifying, and executing automata
  (see: `automata/doc/example.md`).  Currently works as stand alone library
  but is not integrated into IronBee.

**CLIPP**

* CLIPP manual updated. (see: `clipp/clipp.md`)

* CLIPP tests now provide more information about failures.

**IronBee++**

* Support for new site API.

* Support for new float field type.

**Documentation**

* Added CHANGES file.

* Many manual updates.

* Doxygen dependency calculation fixed.  `make doxygen` in `docs` should now
  run only if files have changed.

* Removed long deprecated `fulldocs` doxygen.  Use `external` or `internal`
  instead.

* Updated to doxygen 1.8.1.

**Other**

* Various bug fixes and code cleanup.


