Title:  IronBee Predicate Rules Manual
Author: Christopher Alfeld <calfeld@qualys.com>

# Part 1: Orientation

## Introduction ##

This document is intended as a tutorial and initial reference to orient rule writers to using Predicate.  Predicate is an alternative rule system for the IronBee web application firewall.  It is assumed that you are familiar with IronBee and web application firewalls.

Predicate associates a predicate *expression* with a rule and fires the rule if and only if the predicate expression evaluates to true.  In comparison to the default rule system, Predicate has superior support for composition and configuration time optimization; on the downside, it does not support rule ordering and can be unintuitive.

This document is focused on writing predicate expressions for rules.  For an overview and discussion of the system, see `predicate/predicate.md`.  For a comprehensive reference to the language, see `predicate/reference.md`.

SExpressions (sexprs) are the underlying language of Predicate.  It is not expected that you, a rule writer, will write SExpressions directly.  However, you will see them in error messages and tools and is wortwhile to be familiar with them.  As such, for most Lua code in this document, I have added Lua comments containing the equivalent SExpression, e.g.,

    P.Gt(1000, P.Length(P.Var('REQUEST_URI')))
    -- (gt 1000 (length (var 'REQUEST_URI')))

The second line is the S-Expression corresponding to the first line, prefixed with the Lua comment designator, `--`.

## Basic Terminology

Top-Level Expression
: An expression associated with a rule.  The rule will be fired, if and when, the expression becomes truthy.

Truthy
: An expression or value that is interpreted as true.

Falsy
: An expression or value that is interpreted as false.

Waggle
: A Lua interface to writing IronBee rules.

Frontend
: A Lua interface to writing Predicate expressions.

SExpr
: The low level representation of Predicate expressions.

## First Steps

Let's begin with a series of basic examples.  Consider the following logic:

> If the URI of the request is really long, then ...

Intrepreting, "is really long" to mean "has a length greater than 1000", we can write this in the Predicate Frontend as:

    P.Gt(1000, P.Length(P.Var('REQUEST_URI')))
    -- (gt 1000 (length (var 'REQUEST_URI')))

This is Lua code.  `P.Gt()` is a Lua function that produces a *predicate object* from its arguments, which are in turn predicate objects (or Lua literals).  The Waggle `predicate()` directive understands predicate objects and turns them into sexprs to pass on to Predicate.  For more information on Predicate objects, see XXX.

Predicate expressions are built up by composing *predicate functions* along with literals.

Let's extend our logic to:

> If the URI of the request is really long AND the request is a GET request, then ...

    P.And(
        P.Gt(1000, P.Length(P.Var('REQUEST_URI'))),
        P.Eq('GET', P.Var('REQUEST_METHOD'))
    )
    -- (and
    --     (gt 1000 (length (var 'REQUEST_URI')))
    --     (eq 'GET' (var 'REQUEST_METHOD'))
    -- )

The frontend provides some additional interfaces to more easily express certain patterns.  In particular, it allows using the `+` operator for logical and.  This changes our expression to:

      P.Gt(1000, P.Length(P.Var('REQUEST_URI')))
    + P.Eq('GET', P.Var('REQUEST_METHOD'))
    -- (and
    --     (gt 1000 (length (var 'REQUEST_URI')))
    --     (eq 'GET' (var 'REQUEST_METHOD'))
    -- )

The frontend also allows us to use object method syntax, where the object is passed in to the function as the last argument:

      P.Var('REQUEST_URI'):length():gt(1000)
    + P.Var('REQUEST_METHOD'):eq('GET')

    -- (and
    --     (gt 1000 (length (var 'REQUEST_URI')))
    --     (eq 'GET' (var 'REQUEST_METHOD'))
    -- )

When and whether to use such shortcuts is a matter of style.  Use them if you believe they make the logic clearer.

## Second Steps

Let's look for a suspicious filename in every parameter:

        P.Rx('/etc/(?:passwd|shadow)', P.Var('ARGS'))

<pre class=sexpr>(operator 'rx' '/etc/(?:passwd|shadow)' (var 'ARGS'))</pre>

`P.Rx()` is an example of using an IronBee operator.  IronBee operators are functions provided by modules that can be used by any rule system, not just Predicate.

Now let's limit our regexp to only `GET` and `POST` requests:

    P.And(
        P.Rx('/etc/(?:passwd|shadow)', P.Var('ARGS')),
        P.Or(
            P.Eq('GET', P.Var('REQUEST_METHOD')),
            P.Eq('POST', P.Var('REQUEST_METHOD'))
        )
    )

    -- (and
    --     (operator 'rx' '/etc/(?:passwd|shadow)' (var 'ARGS'))
    --     (or
    --         (eq 'GET' (var 'REQUEST_METHOD'))
    --         (eq 'POST' (var 'REQUEST_METHOD'))
    --     )
    -- )

There is a shortcut for or, `/`.  Using that and our other alternatives:


      P.Var('ARGS'):rx('/etc/(?:passwd|shadow)')
    + (
          P.Var('REQUEST_METHOD'):eq('GET')
        / P.Var('REQUEST_METHOD'):eq('POST')
      )

    -- (and
    --     (operator 'rx' '/etc/(?:passwd|shadow)' (var 'ARGS'))
    --     (or
    --         (eq 'GET' (var 'REQUEST_METHOD'))
    --         (eq 'POST' (var 'REQUEST_METHOD'))
    --     )
    -- )

## Composition

A primary motivation for Predicate is to allow easy composition of rule logic.  The previous examples have not directly taken advantage of that.  Let's factor out some common pieces of logic, such as "is a `GET` request":

    local IsGet = P.Var('REQUEST_METHOD'):eq('GET')
    -- (eq 'GET' (var 'REQUEST_METHOD))

And "is a `POST` request":

    local IsPost = P.Var('REQUEST_METHOD'):eq('POST')
    -- (eq 'POST' (var 'REQUEST_METHOD))

The example from the previous section then becomes:

      P.Var('ARGS'):rx('/etc/(?:passwd|shadow)')
    + (IsGet / IsPost)

    -- (and
    --     (operator 'rx' '/etc/(?:passwd|shadow)' (var 'ARGS'))
    --     (or
    --         (eq 'GET' (var 'REQUEST_METHOD'))
    --         (eq 'POST' (var 'REQUEST_METHOD'))
    --     )
    -- )

Note how the use of intermediate Lua variables to hold pieces of expressions does not affect the resulting sexpr.  I.e., this sort of composition is at the Lua level and happens before conversion to an sexpr.  For a way to do composition post-sexpr, see [Templates][].

We are not limited to variables.  Consider:

> Header X is longer than 1000 bytes.

First, let's define a function to find the value of the "Header X":

    local function RequestHeader(which)
        P.Sub(which, P.Var('REQUEST_HEADERS'))
    end

This function takes the name of a header and provides a predicate object representing the value of that header.  It uses a new function, `P.Sub()`, which is used to select a specific member from a collection.

We can now use `RequestHeader()` to define a notion of a long header:

    local function LongHeader(which)
        RequestHeader(which):length():gt(1000)
    end

We can now use `LongHeader()` to express:

> The Host header is longer than 1000 bytes.

    LongHeader('HOST')
    -- (gt 1000 (length (sub 'Host' (var 'REQUEST_HEADERS))))

There is additional value to reusing pieces of logic.  Predicate automatically detects any reused expressions across all Predicate expressions and only evaluates them once, reusing the result.  This reuse can provide significant performance benefits.

## Configuration Time

IronBee operates at two different times.  At configuration time, it interprets its configuration and sets up any data structures it needs to evaluate traffic.  At runtime (also called evaluation time), it interprets web traffic, determines which rules should be fired (involves evaluating predicate expressions), and fires those rules.

When using Predicate, there is a further distinction to be made at configuration time.  There are things that occur in Lua and things that occur in Predicate.  In Lua, the Lua code is executed to produce predicate objects which are turned in the sexprs.  Those sexprs are then passed to Predicate.  Predicate merges all sexprs together and, once it has everything, performs validation and optimization passes.

This division has a number of implications.  Two important ones are:

1. Most warnings and errors occur at the end of configuration, are in terms of sexprs rather than Lua code, and do not include Lua file or line number information.  Future plans include improving this by transmitting file and line number information to Predicate.  At present, [Templates][] can assist in clearer error messages.
2. Since Lua based composition is performed in Lua, the resulting SExprs that are communicated to Predicate can become quite large.  The use of [Templates][] where possible can alleviate this problem.

Many Predicate functions support configuration time evaluation if all of their arguments are known at configuration time.  For example, consider setting a policy variable in Lua:

    -- Change this to true to apply rule to Post requests.
    local ApplyToPost = false

And then using it in a predicate expression, where `something_complicated` is some complex logic:

    (IsGet / (ApplyToPost + IsPost)) + something_complicated
    -- (and
    --   (or
    --     (eq 'GET' (var 'REQUEST_METHOD'))
    --     (and (false) (eq 'POST' (var 'REQUEST_METHOD')))
    --   )
    --   something_complicated
    -- )

Since `ApplyToPost` is false, this expressions will always be false, no matter what `something_complicated` turns out to be.  Predicate understands this and transforms the entire expression to false at configuration time.

## The DAG

XXX

## Values

We have made it this far without actually worrying about what the value return by a function is.  As an example of how values can be complex, consider the following expressions:

    P.Var('ARGS'):sub('a'):length():gt(5)
    -- (gt 5 (length (sub 'a' (var 'ARGS'))))

And consider the expression in the context of the following request:

    GET /example?a=123&a=123456

Here there are two parameters (members of `ARGS`) named `a`, one of which is longer than 5 bytes and one of which is not.  How do we interpret the expression in this situation?

In a boolean sense, the expression is truthy and can accurate be interpreted as:

> Does any member of `ARGS` named `a` have length greater than 5.

As we will see, the actual value of the expression is:

    [a:'123456']

The result of any expression, including any literal, is called a *ValueList*.  A ValueList is a list of 0 or more *Values*.  And a Value is a name, a type, and a value.  Names are always strings.  At present, the possible types with their values are:

String
: A sequence of bytes, possibly including NULs.

Number
: A signed integer.

Float
: A signed floating point.

List
: A list of Values.

In Predicate, the empty list, `[]` is falsy, and every other value is truthy.

The result of a literal is a ValueList with a single Value.  E.g., `1000` becomes `[:1000]` (the lack of text before the : indicates an empty name), and `'foo'`, becomes `[:'foo']`.

There are a few more complications.  Consider the expression:

    P.Not(P.Rx('foo', P.Var('ARGS'))
    -- (not (operator 'rx' 'foo' (var 'ARGS')))

Meaning

> There is no argument with value containing `foo`.

The `ARGS` collection begins each transaction empty, potentially grows after the request URI is parsed, and potentially grows again once the body is parsed.  Imagine we have seen the URI but not the body.  If an argument containing `foo` appears in the URI, then this expression must be falsy, but if it does not, we cannot yet say whether it is truthy or falsy.  Instead, we must wait for the request body to be parsed.

To accomodate `foo` appearing only in the body, Predicate allows ValueLists to grow.  The result of `P.Var('ARGS')` begins empty and may grow later.  ValueLists are only allowed to grow, they may never shrink or change earlier elements.  A consequence of this is that expressions may change from falsy to truthy but never from truthy to falsy.  This allows Predicate to begin this expression as falsy and change it to truthy after the request body.

But if `foo` appears in the URI, we want to know that the expression is falsy immediately, if for no other reason than to not spend time evaluating it later.  To accommodate this, every node has a notion of finished or not.  Once a node is finished, it may not modify its ValueList.

With this in hand, we can now describe how the expressions works:

- `P.Var('ARGS')` begins empty and unfinished.  After the request URI is parsed, it may add any arguments in the request URI but stays unfinished.  After the request body is parsed, it may add any arguments in the request body and becomes finished, knowing that no more arguments can appear.
- `P.Rx('foo', ...)` begins by checking its second argument.  As that argument is empty, `P.Rx()`s ValueList is empty.  As that argument is unfinished, `P.Rx()` is unfinished.  When values are added to its second argument, it checks the new values and adds any that contain `foo` to its ValueList.  Only when its second argument becomes finished, does it also become finished.
- `P.Not(...)` begins by checking its argument.  As its argument is falsy but unfinished, `P.Not()` must be falsy and unfinished.  It must be falsy, becomes its argument may become truthy in the future and `P.Not()` may not change from truthy to falsy.  `P.Not()` must remain falsy until it knows its result will not change, either when its argument becomes truthy (in which case, `P.Not()` knows it will be falsy and can be finished) or when its argument becomes finished.  In the example, if an argument containing `foo` appears in the request URI, then the first argument becomes truthy and `P.Not()` can become finished and falsy.  If an argument containing `foo` never appears, that `P.Not()` can only become truthy and finished after its child becomes falsy and finished after the request body.

These details can become complicated.  It works out that `P.Not()` (and its related functions such as `P.Nand()`) are some of the few places where these details matter.  In most other cases, it suffices to understand that if there are multiple values, a Predicate expression is truthy if it is "true" for any of the values.

## More Terminology

Simple
: A node or expression is called *simple* if it is always finished and has zero or one values in its ValueList.  The canonical example is literals.

Value
: A value is a name, a type, and a value.  The name is a string; the type is one of number, float, string, or list.

ValueList
: A ValueList is zero or more values.  The value of a predicate function is a ValueList.  ValueLists may grow but may never shrink or change already added values.  The empty ValueList is falsy and a non-empty ValueList is truthy.  The ValueList of a literal is a list with a single element being that value with an empty name.

# Part 2: Predicate Functions

This section introduces many, but not all, functions available to predicate.  For a complete list, see `predicate/reference.md`.  Also remember that any IronBee transformation or operator can be used in Predicate.

Many Predicate functions require that certain arguments are literals.  As a general rule of thumb, the last argument can be dynamic and all other arguments should be literals.  The main exception are functions that take an arbitrary number of arguments (e.g., `P.Or()`), in which case all arguments can be literal or dynamic.  Any other exceptions will be mentioned below.

## Boolean

Predicate directly provides three basic boolean connectives: `and`, `or`, and `not`.  The frontend adds several others implemented in terms of them: `xor`, `nxor`, `nand`, and `nor`.  E.g.,

    P.Xor(a, b)
    -- (or (and a (not b)) (and (not a) b))

The frontend also provides a variety of shortcuts:

- `a + b` is equivalent to `P.And(a, b)`.
- `a / b` is equivalent to `P.Or(a, b)`.
- `-a` is equivalent to `P.Not(a)`.
- `a - b` is equivalent to `a + (-b)`
- `P.Xor(a, b)` is equivalent to `(a - b) + (b - a)`.
- `a ^ b` is equivalent to `P.Xor(a, b)`.
- `P.Nand(a, b)` is equivalent to `-(a + b)`.
- `P.Nor(a, b)` is equivalent to `-(a / b)`.
- `P.Nxor(a, b)` is equivalent to `-(a ^ b)`.

Finally, there are canonical constants for providing true and false values:

    P.True
    -- (true)

    P.False
    -- (false)

The expressions `(true)` and `(false)` produce canonical truthy and falsy values, respectively.  These are: `[:'']` for true, and `[]` for false.

The functions `and` and `or` are special in that they do not care about the order of their arguments.  Thus `P.And(a, b)` and `P.And(b, a)` are not only logically equivalent, Predicate will recognize them as identical and merge them into a single DAG node.

On occasion, for performance reasons, it is desired to have short-circuiting versions: `P.AndSC()` and `P.OrSC()`.  These short-circuiting versions should be considered as identical except that they provide hints to the evaluator that later arguments should have their evaluation delayed until it is clear that they are needed from their earlier arguments.

Finally, there is an if statement: `P.If(p, t, f)`, which takes the value of `t` if `p` is truthy and `f` if `p` is falsy.

## ValueList

Predicate provides a variety of functions for constructing and manipulating ValueLists.

To extract particular values, there is `P.First(v)` which provides a ValueList containing only the first value of `v`; `P.Rest(v)` which provides all but the first value; and `P.Nth(n, v)` which provides the nth value.

ValueLists are similar to values of type list.  You can go from one to the other with `P.Scatter()` and `P.Gather()`.  `P.Scatter(a)` expects `a` to be a single list value and returns the elements of that list.  `P.Gather(a)` takes all the values of `a` and returns a single list value holding the values of `a`.

To construct ValueLists, there is `P.Cat(...)` which concatenates all the values of its arguments; and `P.SetName(n, v)` which sets the name of every value of `v` to `n`.  `P.SetName()` can be used with literals to build arbitrary value lists at configuration time, e.g.,

    P.Cat(P.SetName('foo', 1), P.SetName('bar', 2))
    -- (cat (setname 'foo' 1) (setname 'bar' 2))

The result of this expression is `[foo:1, bar:2]`.

## Predicate Predicates

Predicate Predicates are functions that return canonical truthy or falsy values, i.e., `P.True` or `P.False`.

Predicate predicates include:

`P.isLonger(N, v)`
: Truthy iff `v` has `N` or more values.

`P.isLiteral(v)`
: Truthy iff `v` is a literal.

`P.isSimple(v)`
: Truthy iff `v` is a literal and has zero or one values.

## Filters

Filter functions apply a subfunction to every value of an input.  For example, `P.Eq('x', v)`, is a list of every value of `v` that is equal to `x`.

In addition to `P.Eq`, there is `P.Ne` for not equal.  There are also `P.Lt`, `P.Le`, `P.Gt`, and `P.Ge` for the usual arithmetic comparisons.

For choosing values by name, there are `P.Named` for case sensitive and `P.NamedI` for case insensitive.  `P.Sub` is a synonym for `P.NamedI`.  There is also `P.NamedRx` for choosing by regular expression.

## IronBee

### Phase

The intent is that a Predicate expression or rule need not worry about phases.  The expression will become truthy and the rule executed at the earliest possible phase.  However, this ideal does not always match reality.  A module might provide vars without phase information, or a rule writer might want greater control over when the rule fires.  There are two ways to gain such control: first, any Predicate rule may include a phase descriptor in which case the rule will only be checked (and potentially fired) in that phase; second, the functions below can control expressions according to phase.

`P.WaitPhase(p, v)` does not evaluate `v` until `p`.  Before phase `p` it will be falsy and unfinished.  After phase `p` it will behave like `v`.

`P.FinishPhase(p, v)` has the same values of `v` up through phase `p`.  Once evaluated at phase `p` it is finished, even if `v` is not and continues to change.

### Var

The function `P.Var()` is used to access vars.  It is an exception to the normal rules in that all its arguments must be static.  `P.Var(n)` is used to access the var named `n`.  It will use the phase information provided by the module that registered `n` to determine when to take a value and when to finish.  If a module does not provide these phases, but the user knows what they should be, they can be provided in the long form of var `P.Var(n, w, f)` where `w` is the first phase that the var may have a meanigful value and `f` is the the last phase the var may have additional values.

A var may be a dynamic collection.  Dynamic collections can be asked for a list value by providing a single string argument (sometimes called the *question*).  `P.Ask(q, v)` asks a dynamic collection in `v`, question `q`.  `P.Ask()` is often used with `P.Var()`, e.g.,

    P.Ask('question', P.Var('O'))
    -- (ask 'question' (var 'o'))

You may see `P.Field()` appear in old examples.  `P.Field()` is the pre-0.9 name for `P.Var()` and is deprecated.

### Operator

Any IronBee operator may be called via `P.Operator()`.  `P.Operator(n, a, v)` calls the operator named `n` on every value in `v` with argument `a`.  The result is the *capture collections* for each application that the operator returned true for.  In this form, operators both filter the ValueList of `v` and transform it into the capture collections.  If you want to use it as a filter, preserving values without transforming (discarding the capture collections), use `P.FOperator()`.

The frontend wraps many operators in functions allowing direct access.  E.g., `P.Rx(a, v)` is the same as `P.Operator('rx', a, v)`.

### Transformation

Any IronBee transformation may be called via `P.Transformation()`.  `P.Transformation(n, v)` applies the transformation named `n` to every value in `v`, with the result being the transformed values.

As with operators, most transformations are directly available in the frontend.

## Development

There are a few Predicate functions that are intended for developing and testing rather than production evaluation.

`P.P()` is the equivalent of a print statement.  It outputs all of its arguments to standard error and takes the value of its last argument.  Here is an example of using `P.P()` to see an intermediate value:

    P.Gt(1000, P.P(
        'Host header length is',
        P.Var('REQUEST_HEADERS'):sub('Host'):length()
    ))
    -- (gt 1000 (p
    --     'Host header length is'
    --     (length (sub 'Host' (var 'REQUEST_HEADERS')))
    -- ))

`P.Sequence()` is used to construct sequences of values.  `P.Identity(v)` behaves like `v`.  Both of these have little use outside of testing Predicate; they are detailed in the reference manual.

# Part 3: Additional Topics

## Tools ##

### PP ###

PP is a program (`predicate/pp.rb`) that can be run on a Waggle file containing Predicate rules.  It will extract all Predicate expressions from those rules, validate them, and produce an annotated HTML report that includes the sexprs, issues, and graphs.

See `predicate/pp.pdf`.

### PredicateTrace ###

PredicateTrace is a feature of the Predicate module that produces extensive information on how Predicate evaluates.  It can be turned with the `PredicateTrace` directive which takes a path to write the trace data to (an empty string means standard error).  The trace data can then be interpreted by a program (`predicate/render_ptrace.rb`) which produces an HTML report showing the values of every node in the DAG at each phase.

PredicateTrace is extremely expensive and should never be used in production or with large DAGs.  It can be useful, however, to debug small sets of rules.

## Templates [Templates] ##

Templates provide a mechanism for constructing complex expressions from a few parameters.  They are similar to using Lua functions to construct Predicate objects, but are expanded by the Predicate module rather than Lua.  For illustration, consider the following two examples.

    local function RequestHeader(which)
        P.Sub(which, P.Var('REQUEST_HEADERS'))
    end

    P.define('RequestHeader', ['which'],
        P.Sub(P.Ref(which), P.Var('REQUEST_HEADERS'))
    )
    -- (RequestHeader which) := (sub (ref 'which') (var 'REQUEST_HEADERS'))

The former, the Lua function, can be used, e.g., `RequestHeader('Content-Length')`.  The latter, the Predicate Template, can be used, e.g., `P.RequestHeader('Content-Length')`.  Semantically, they are identical: both mean the request header named "Content-Length".  The former expands in Lua and passes `(sub 'Content-Length' (var 'REQUEST_HEADERS'))` to the Predicate module.  The latter passes `(RequestHeader 'Content-Length')` to the Predicate module, and the module expands it to `(sub 'Content-Length' (var 'REQUEST_HEADERS'))`.

Templates consist of three things: a name, an argument list, and a body.  A new Predicate function with the given name is created.  When it appears in an expression, its arguments are labeled according to the argument list: the first argument is labelled with the first item in the argument list, the second with the second, and so forth.  Then, the body is transformed by replacing any subexpression of the form `(ref X)` with the argument labeled `X`.  Templates can be created in Lua by calling `P.define()`; references can be used in the body via `P.Ref()`.

For situations where they can be used, templates are generally the superior choice.  Advantages over Lua functions include:

- The template name can appear in error messages and introspection, making debugging easier.
- Less data is communicated from Lua to Predicate.  For complex rule systems built on top of libraries of templates or functions, this can be substantial and templates can improve both memory and time performance.

The main limitation of templates is that they can only do simple substitutions.  Here is an example of a Lua function that has no easy template equivalent:

    def EtcFile(filename)
      P.Rx('^/etc/' .. filename .. '$', P.Var('REQUEST_URI'))
    end

`EtcFile` constructs a regexp string from an argument; a task easily done in Lua but difficult in Predicate.  `EtcFile` is best implemented as a Lua function, not as a template.

## Extending Predicate ##

Predicate can be extended by IronBee modules.  There are five ways to extend Predicate with a module:

Vars
: Your module can define and set a Var.  Predicate can use vars via `P.Var()`.  In order for a Var to be usable by Predicate you must do two things:

1. If your var is not a list var, then, once set, it may never change.  If your var is a list var, then it may append elements to the list but never remove or change earlier elements.
2. You may register your var and indicate the first phase it may have a meaningful value and the last phase in which it may set or append a value.  Vars are registered with `ib_var_source_register()`.

Dynamic Vars
: A dynamic var, implemented as a dynamic list field, is effectively a function that takes a string and returns a collection.  Predicate can use dynamic vars via `P.Ask()`.  Dynamic vars must obey the same conditions as vars.

Operator
: An operator is a function that takes a string literal and a single value and outputs true or false and an optional *capture collection*.  Predicate can access operators via `P.Operator()` and `P.FOperator()`.  Operators are registered via XXX.

Transformation
: A transformation is a function that takes a value and returns a value.  Predicate can access transformations via `P.Transformation()`.  Transformations are registered via XXX.

Call Node
: A new type of Call Node can be defined by creating, in C++, a descendent of `Predicate::Call`, implementing certain vitual methods, and informing Predicate of it via XXX.  Predicate can access new call nodes via `P.Call()`.  Call Nodes are dismayingly tricky to properly implement, but are the only way to get advanced functionality such as multiple arguments, validation, or transformations.  The interface for implementing Call Nodes is likely to change.  Finally, unlike the methods above, new Call Nodes will only be usable by Predicate and unavailable to other rule systems or modules.

If it all possible, use one of the first four methods.  Those are comparitively simple and will seemlessly work with other rule systems and modules.
