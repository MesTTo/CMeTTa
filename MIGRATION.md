# C ABI 1

Version 1 is an ABI break from the earlier unversioned surface. Recompile C
consumers and extensions together against this header. The installed library
has SONAME `libcmetta.so.1`; `MT_VERSION` is the release source and
`mt_version()` reports the loaded library's version.

Within this ABI major, exported signatures, enum values, public structure
layouts and ownership contracts must remain compatible. Opaque handles leave
their representation private. A change to a callback signature, public
structure layout or ownership rule requires another major. This contract is
for a matching C ABI and platform, not binary portability between platforms.
`make surface`, the separately compiled extension test and
`make install-check` exercise the shipped interface. Future releases must
also test existing consumer binaries; the present tests make no claim about
code that has not been released.

## Allocations and ownership

Use `mt_free` for library-owned strings and list arrays. Direct `free` no
longer works even with the default allocator: a private header retains the
allocator that owns each block. Allocate arrays transferred through `mt_list`
with `mt_alloc` or `mt_calloc`, and resize them with `mt_resize`. An array
borrowed by `mt_exprv` may still be a stack or libc array.

`mt_allocator_set` selects the allocator for subsequent allocations on the
calling thread. It returns the previous selection. Existing blocks retain
their original callback and user pointer. Both must outlive those blocks and
support whichever thread releases them. The allocator owns C allocations;
SWI's heap and external libraries retain their own allocators.

`mt_object` and `mt_function` now consume the supplied user resource on every
path, including allocation failure and a NULL function. Do not release it
again after a failed constructor. Borrowed-storage constructors have a
different contract: `mt_text_ref` and `mt_expr_ref` acquire the owner's release
obligation only on successful construction. A failed borrow leaves the owner
with its caller.

An unsigned macro argument preserves its full value; `UINT64_MAX` becomes an
exact big integer. `mt_bigint("0007")` now constructs `MT_INT` 7. A `const
mt_atom *` macro argument retains a reference; a mutable atom argument still
transfers its reference. Explicit constructors preserve exact symbol names.

`mt_def` now preserves the published name exactly. Replace calls to the old
automatic `word-count` spelling with `word_count`, or register `word-count`
explicitly. The binding never converts underscores to hyphens.

A native object's type_name now contributes the same exact symbol to MeTTa's
get-type and typed dispatch through the grounded type ownership seam. NULL
contributes no type candidate; it does not invent a default class.

## Providers

Replace the text callbacks and indexed `atom_at` with typed atom callbacks and
`match(user, pattern, limit, &iterator)`. The iterator yields owned atoms with
`MT_ROW`, ends with `MT_DONE`, and closes once. A variable pattern requests all
atoms. The engine unifies candidates itself; preserve duplicates and ignore
an advisory limit unless your candidate stream is already exact.

`add`, `remove`, `clear` and transaction callbacks return `MT_OK` or an error.
`remove` writes whether it removed one occurrence through its boolean output.
NULL declines a capability. Supply `begin`, `commit` and `rollback` together
or omit all three. Completion uses the captured provider, even if its name was
withdrawn and reused. `mt_provider_open` consumes `user` through `release` on
every path. A live query or transaction may delay release after close.

## Source, cursors and transactions

Logical text, names and source use UTF-8; lengths count bytes. Invalid UTF-8
is refused at the engine boundary with its byte offset. Earlier releases
interpreted UTF-8 bytes as Latin-1. Filenames use SWI's platform encoding.

`mt_run`, `mt_load` and `mt_do` now dispatch to either a runtime or a space.
Their addressable functions are `mt_self_run`/`mt_space_run`,
`mt_self_load`/`mt_space_load` and `mt_self_do`/`mt_space_do`. Use these when
taking a function pointer. A NULL receiver in a generic call must have its
intended pointer type, for example `(metta *)NULL`.

Native producers use `mt_iterator` and `mt_answer_iter`. Named operations
produce nondeterministic answers directly. A function value returning a
producer instead returns an opaque iterator; consume it through `c-iter`.
`mt_stream_of` borrows its cursor for direct C consumption. Iterators are
consumptive and must not be driven concurrently.

`mt_all` and `mt_one` no longer return a successful prefix after a later pull
fails. Inspect `mt_step` or `mt_answers_status` to distinguish exhaustion from
failure. `mt_first` intentionally abandons the rest.

Enter a multi-call transaction through `mt_transaction` or `mt_speculate`.
These scopes also restore C registrations on rollback. A transaction begun
solely inside a MeTTa expression cannot own a suspended C registration
snapshot; a registration attempt there is refused with the remedy to enter
every enclosing scope through `mt_transaction`. Queries opened inside a closed
scope collect before it ends, because SWI cannot yield across that goal.

`mt_del` removes one unifying occurrence and refuses a bare variable. Use
MeTTa's `remove-atom` for a drain. `mt_space_close` releases the C handle;
`mt_space_drop` retires the engine space and its declarations.

## Subscription and extension lifetime

`mt_subscribe` consumes its pattern and callback data on every path.
`mt_unsubscribe` closes a named registration; a callback may cancel itself.
The current callback keeps its data until it returns. Register and cancel
under the same serialization rule as operations. Callbacks run synchronously
on the writer's thread, after commit; errors there cannot undo the commit.
Subscriptions follow an explicitly named space until cancelled, so cancel
before reusing that name.

Extension initialization now runs in a closed transaction. Failed
initialization restores engine definitions and C registrations. It cannot
undo arbitrary external effects. Loaded handles stay open until process exit,
including on initialization failure, because a callback may have
returned code-bearing values to C.

## Installation and rollback

Run `make install PREFIX=/your/prefix` and obtain consumer flags from
`pkg-config --cflags --libs cmetta`. The install includes the engine sources
and C bridge, and the library bakes that installed engine path.
`METTA_PATH` can select another compatible runtime tree.

For rollback, restore the previous library, header, C extensions and matching
engine installation as one versioned prefix, then relink its consumers.
Mixing an old provider binary with ABI 1 is unsupported. No remote deployment
or package publication is part of this change.
