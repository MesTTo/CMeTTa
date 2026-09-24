% Guarantees: metta_c_error_advice/3 answers the engine's own remedy and
%   ground for a raised ball, which is what mt_remedy() and mt_ground() carry,
%   so the C seat says the same sentence about one refusal as the other two
%   [tested: extensions/cmetta/tests/test_cmetta.c,
%   test_a_refusal_carries_the_engines_remedy_and_ground; commit=f33b7ab0200e6dc74c88fb4c7f827bf545a447ed].
% Guarantees: metta_c_error_text/2 scopes message capture through
%   metta_engine:metta_with_trailed/3
%   [source: extensions/cmetta/bridge.pl:metta_c_error_text/2; commit=40b71fc99571872ca5fc85cdaf7902b467166539].
%
% Purpose: the Prolog half of the C binding. It runs a MeTTa program, holds a
%   query open as a resumable answer stream, publishes C functions as MeTTa
%   operations, and hands each answer back as an ENGINE TERM for the C half to
%   walk directly.
% Assumes:
%   - every engine predicate called here carries a seam:kind/2 in
%     engine/ext_points.pl, service or host_service. The static gate walks all
%     three host bindings and this file is one of their rows
%     [tested: tests/prolog/static_checks.pl,
%     a_host_binding_calls_only_published_surface;
%     commit=0c544dba163996ab34fec1cb574f5f4faf8b53f0]
%   - '$cmetta_dispatch'/3 and '$cmetta_object_live'/1 are foreign predicates the C
%     half registers before consulting the engine. This file LOADS without
%     them, because the static gate consults it directly with no C host in the
%     process, so nothing here may call one at load time.
%   - the C half runs each call inside its own PL_open_foreign_frame, so a
%     term handed out here stays valid exactly as long as that frame
% Guarantees:
%   - outside a closed transaction, metta_c_next/4 computes at most one answer per call, so a host that
%     stops pulling leaves the rest of an infinite stream uncomputed. The case
%     cited walks an ENDLESS generator and breaks after three answers, which
%     an eager door could not return from at all
%     [tested: tests/test_cmetta.c, test_the_walk_closes_its_cursor_on_break;
%     commit=c530ccb8fb7d0a5b2aa53df6e9f981ada9f81be8]
%   - a cursor opened with a positive Inferences stops once its ENGINE has
%     spent that many, cumulatively across pulls, because the budget is built
%     into the engine goal by metta_host_inference_budget/3 [tested:
%     tests/test_cmetta.c, test_a_bound_stops_a_runaway_and_says_so;
%     commit=23082258ab5a278998c967274c5b22e0ce391a47]
%   - metta_c_close/1 erases the recorded owner before destroying its engine.
%     Concurrent closes have one winner; an erased reference closes quietly.
%     Engine destruction happens after unlocking, even if an erase listener
%     raises [tested: sh extensions/cmetta/test.sh; commit=8ca8a387fc61d0918484b19a1a3baf85b6523043].
%   - cursor identifiers are monotone for one runtime's lifetime and opening
%     one takes one atomic flag update rather than a scan of the open-cursor
%     table. The C half carries the runtime generation beside the identifier,
%     so a cursor retained across cleanup cannot close a new runtime's cursor
%     after SWI resets its flags [tested: extensions/cmetta/tests/test_cursor_ids.c,
%     test_cursor_ids_are_monotone_and_constant_cost;
%     commit=b5ddebe73273447caa7c57212d6ee86fc71e0d4a]
%   - no answer is encoded, tagged, or stringified on the way out: the C half
%     receives the engine's own term. This seat is in-process with the engine
%     and has no marshalling boundary to cross, which is the whole reason it
%     exists; see C6 in ai-cmetta-c-constraints.md. The case cited sends a C
%     POINTER through MeTTa and mutates the struct behind it on the way back,
%     which nothing that rendered the value could do
%     [tested: tests/test_cmetta.c, test_a_c_value_crosses_by_reference;
%     commit=c530ccb8fb7d0a5b2aa53df6e9f981ada9f81be8]
%   - an operation published from C is registered through the engine's own
%     four-call host protocol (open, assert, adopt, release), so a name another
%     tier owns is refused rather than clobbered, and refused BEFORE anything
%     is written
%     [tested: tests/test_cmetta.c,
%     test_a_taken_name_is_refused_rather_than_clobbered; commit=c530ccb8fb7d0a5b2aa53df6e9f981ada9f81be8]
%   - a C provider takes its space NAME at the engine's claim door and gives it
%     back when it closes, so a name another provider owns is refused here
%     rather than served by two stores, and the ownership row the engine reads
%     exists exactly while a provider is open
%     [tested: extensions/cmetta/tests/test_seam.c,
%     test_a_c_provider_takes_a_space_name_and_gives_it_back; commit=9ee28a945da58dfdef86119ea082609bd5975aed]
% Owns resources: one recorded owner per open cursor. metta_host_hold/3 owns
%   either a resumable SWI engine or transaction-scoped materialized answers;
%   metta_c_close/1 releases that owner through metta_host_hold_close/1
%   [tested: tests/test_transactions.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c].
% Guarded by: $cmetta_cursors serialises the close winner's reference lookup
%   and erase. The C half registers the owner reference atom
%   across frames and checks their runtime generation before using them.
% Decides: verbosity is set explicitly at boot rather than inherited from argv,
%   because filereader.pl reads the CLI at load time and an embedded host has
%   none. The setting itself is the engine's metta_host_set_silent/1, not a
%   copy here: this seat filed the duplication as C2 and the engine took it.
% Open Obligations:
%   To Do: None
%   Hacks: None
%   Future Enhancements: None

:- use_module(library(time), [call_with_time_limit/2]).

:- dynamic metta_c_op_spec/3.
:- dynamic metta_c_op_effect/2.
% Exception rendering is scratch for ONE caller. A normal dynamic predicate is
% shared, so two attached C threads could retract or read one another's reason.
% thread_local/1 gives each attached engine its own clause list, which SWI
% reclaims when that thread detaches
% [tested: tests/test_threads.c; commit=b339084bb5625996fc88a31608d48ad31c575d1f].
:- thread_local metta_c_captured/1.

%%%%%%%%%% Rendering an exception %%%%%%%%%%
%
% The C half catches the ball itself through PL_exception(), so nothing here
% needs to turn an outcome into data the way the Node bridge does. What it
% cannot do in C is render the ball the way SWI would print it, so it asks
% here. print_message/2 goes through exactly the machinery the console would
% have used, and the hook below takes the lines instead of letting them out.
metta_c_error_text(Ball, Text) :-
    retractall(metta_c_captured(_)),
    % Workaround: swi-cleanup-window - message capture restores its trailed flag.
    metta_engine:metta_with_trailed('$metta_c_capture', true,
                                   catch(print_message(error, Ball), _, true)),
    (   metta_c_captured(Rendered)
    ->  Text = Rendered
    ;   term_string(Ball, Text)
    ),
    retractall(metta_c_captured(_)).

% Deaf outside metta_c_error_text/2, and it has to be: a hook that succeeds
% suppresses the message, so an always-on one would swallow the loader's own
% diagnostics.
:- multifile user:message_hook/3.
user:message_hook(_, _, Lines) :-
    nb_current('$metta_c_capture', true),
    print_message_lines(atom(Text), '', Lines),
    assertz(metta_c_captured(Text)).

% What to DO about a ball, and what says so: the engine's own (refusal ...)
% catalog row for the kind that ball is, with the remedy template's <field>
% holes already filled from this very ball. This seat composes nothing; the
% Python and JavaScript seats read the same two rows, so one refusal has one
% remedy across all three.
%
% It FAILS for a ball whose kind carries no row, which the row lane forbids
% and a program that removed the row can still produce; the C side then leaves
% mt_remedy() and mt_ground() answering NULL.
metta_c_error_advice(Ball, Remedy, Ground) :-
    metta_host_refusal(Ball, _, _, _, [ground, Authority, Citation],
                       [remedy, Remedy|_]),
    format(string(Ground), "~w: ~w", [Authority, Citation]).

%%%%%%%%%% Text, through the engine's own reader and writer %%%%%%%%%%
%
% sread_with_names/3 rather than sread/2, because the C side wants the source
% spellings of the variables it is about to hand back to a caller who wrote
% them.
metta_c_read(Source, Term, Names) :-
    metta_c_text(Source, S),
    sread_with_names(S, Term, Names).

% The source parser owns form boundaries; the atom reader owns each form.
% No directive is executed and a later syntax error returns no prefix.
% [tested: tests/test_native_parity.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
metta_c_read_forms(Source, Terms, Names) :-
    metta_host_read_forms(Source, Pairs),
    maplist(metta_c_read_form, Pairs, Terms, NameLists),
    append(NameLists, Names).
metta_c_read_form([_, Text], Term, Names) :- sread_with_names(Text, Term, Names).

% swrite/2 is the round-trip writer and refuses what it could not read back;
% sdisplay/2 is presentation and renders it anyway. A caller asking to SEE an
% atom gets presentation, which is the same choice the command line makes.
% Names is the caller's own Name-Var pairs, so a variable prints as the name
% its author wrote. With no names the writer numbers them $_0, $_1, which is
% correct and unhelpful to somebody who wrote $x.
metta_c_show(Term, Names, Text) :- sdisplay_with_names(Term, Names, Text).

% Serialization stays separate from display. It either answers reader-inverse
% text or raises the engine's ordinary unwritable-value error; it never falls
% back to a presentation spelling that would read as a different atom.
metta_c_write_atom(Term, Names, Text) :- swrite_with_names(Term, Names, Text).

%%%%%%%%%% Running a program %%%%%%%%%%
%
% The grouping walk, the working-dir defaulting and the load lifecycle are the
% engine's own host run and load surface, shared with every other seat. One
% group per ! directive, in source order.
metta_c_run(Source, Space, Seconds, Inferences, Groups) :-
    metta_c_text(Source, S),
    metta_c_bounded(metta_host_run_source(S, Space, [], Groups),
                    Seconds, Inferences).

metta_c_load(File, Space, Seconds, Inferences, Groups) :-
    metta_c_atom(File, FA),
    metta_c_bounded(metta_host_load_file(FA, Space, Groups),
                    Seconds, Inferences).

%%%%%%%%%% Bounding a call %%%%%%%%%%
%
% A bound that stops a goal stops it MID-WAY, so writes it already made stand.
% That is the honest semantics of every timeout and is not something a binding
% can improve on; a caller who needs all-or-nothing wraps the work in a
% transaction.
%
% Zero means unbounded on both, which is the C side's spelling for "no bound"
% and saves a sentinel.
metta_c_bounded(Goal, Seconds, Inferences) :-
    metta_c_timed(Goal, Seconds, Timed),
    metta_c_counted(Timed, Inferences).

metta_c_timed(Goal, Seconds, Goal) :- Seconds =< 0, !.
metta_c_timed(Goal, Seconds, Timed) :-
    Timed = catch(call_with_time_limit(Seconds, Goal),
                  time_limit_exceeded,
                  throw(error(cmetta_limit(seconds, Seconds), _))).

% The inference bound raises the ENGINE's reserved limit envelope rather than a
% second ball of this seat's own. The cursor door below reaches that envelope
% anyway, because the budget it installs is the engine's, and one bound wearing
% two ball shapes depending on which door produced it is a second name for one
% thing. The wall bound keeps its own ball because it IS this seat's: it is
% applied per pull, which is a policy the engine does not have.
%
% It is the engine's BUILDER and not a second copy of the limiter, for the
% reason that builder exists: SWI disarms the limit before raising its bare
% `inference_limit_exceeded` atom inside the goal, so a recovery catch under
% the goal eats the ball and the limiter reports success for work that never
% stopped. The cumulative counter read the builder pairs with it is what
% refuses then [tested:
% inference_budget:a_swallowed_ball_still_refuses_at_the_c_door].
metta_c_counted(Goal, Inferences) :- Inferences =< 0, !, call(Goal).
metta_c_counted(Goal, Inferences) :-
    metta_host_inference_budget(Goal, Inferences, Bounded),
    call(Bounded).

% The C half asks whether a ball it caught is a bound rather than a fault, so
% a caller can tell "I stopped it" from "it broke". Both sources answer here:
% this seat's own wall ball, and the engine's reserved envelope, which arrives
% from a cursor budget and from a program's own (pragma! max-inferences N)
% alike. Before the envelope was listed, a program that spent its own pragma
% budget reached a C caller as CMETTA_ERROR, a fault.
metta_c_limit_ball(error(cmetta_limit(Kind, Bound), _), Kind, Bound).
metta_c_limit_ball(error(metta_control_signal(Signal, Bound), _), Kind, Bound) :-
    metta_c_limit_kind(Signal, Kind).

metta_c_limit_kind(inference_limit, inferences).
metta_c_limit_kind(time_limit, seconds).

% Only the wall ball is rendered here; engine/metta/registration.pl renders the
% reserved envelope, so the sentence does not exist twice.
:- multifile prolog:error_message//1.
prolog:error_message(cmetta_limit(seconds, Bound)) -->
    [ 'the evaluation passed its ~w second bound and was stopped'-[Bound] ].

% One answer, split into the three things the C half reads: the term, the
% source names of its free variables, and the engine's own rendering.
metta_c_answer_parts('$metta_answer'(Term, NameState), Term, Names, Text) :- !,
    metta_name_pairs(NameState, Names),
    sdisplay_with_names(Term, NameState, Text).
metta_c_answer_parts(Term, Term, [], Text) :-
    sdisplay(Term, Text).

%%%%%%%%%% One query, held open %%%%%%%%%%
%
% An SWI engine is a goal suspended between answers: it "can, if asked,
% resume" after yielding one, which is the answer-stream reading Tarau states
% as design law (A Hitchhiker's Guide to Reinventing a Prolog Machine, ICLP
% 2017, sections 4.5 and 5). The C half wraps this pair in a step cursor for
% the same reason, the shape sqlite3_step() already gave C.
%
% The C half holds the monotone identifier and registered record and engine
% references. Pulls use the engine directly; close claims the recorded owner.
% A bound reference reaches its record directly; a dynamic cursor table
% retained erased clauses in SWI's first-argument index until clause collection.
% One static key avoids retaining a RecordList and key atom for every cursor
% [source: https://github.com/SWI-Prolog/swipl-devel/blob/V10.1.13/src/pl-rec.c,
% lookupRecordList, recorded and remove_record; commit=8ca8a387fc61d0918484b19a1a3baf85b6523043].
%
% with_metta_module/2 runs INSIDE the engine. An engine has its own stack, so
% the module in force outside it is not in force within.
%
% An inference bound on a CURSOR goes INSIDE the engine goal, which is what
% metta_host_inference_budget/3 builds. This file used to meter it here
% instead, with statistics/2 either side of each engine_next/2 and the deltas
% accumulated on the cursor, and that does not work: an engine counts its own
% inferences and this thread cannot see them, so those deltas are the pull
% loop, an order of magnitude or more below what the engine is spending
% [tested: tests/prolog/suites/evaluation/inference_budget.plt,
% an_engine_counts_its_own_work_and_the_creating_thread_does_not;
% commit=23082258ab5a278998c967274c5b22e0ce391a47].
%
% The sweep this file once carried as evidence, 1,000 stopping at 1,004 and
% 100,000 at 100,004, is what that looks like from inside: a constant
% four-inference overshoot at every scale is a fixed charge per pull, so the
% reported total tracks the budget by construction whatever the engine is
% doing. It was a correct measurement of the wrong counter, and the case that
% now rules that shape out asks five times the budget to buy several times the
% answers, which a per-pull charge cannot do [tested:
% tests/prolog/suites/evaluation/inference_budget.plt,
% a_budget_is_cumulative_across_resumes; commit=23082258ab5a278998c967274c5b22e0ce391a47].
%
% The other half of the original diagnosis was sound and mis-explained. An
% engine goal wrapped in call_with_inference_limit/3 ALONE fired at budgets of
% 500, 1,000 and 2,000 and never fired at 5,000, 10,000 or 20,000 over the same
% endless generator, and a cumulative budget cannot behave that way. The reason
% is not that the limiter fails to cross engine_next/2, which is what this note
% concluded; it is that SWI bounds inferences per SOLUTION of the goal, so a
% generator answering cheaply forever is re-armed at every answer and never
% reaches it [tested: tests/prolog/suites/evaluation/inference_budget.plt,
% the_bare_limiter_does_not_bound_a_generator; commit=23082258ab5a278998c967274c5b22e0ce391a47]. The published
% wrapper keeps that limiter, because it is the only one of the two bounds that
% stops a resume which never yields at all, and adds the cumulative check the
% per-solution contract cannot express.
%
% The wall bound stays per pull, so time between pulls, while the host is doing
% something else, cannot count against it.
%
% A goal runs in the evaluation fuel scope every runnable form runs in, and the
% Python seat's evaluation too (metta_py_produce/5 with its default fuel), so
% (pragma! max-stack-depth N) bounds it branch by branch and a branch that runs
% out answers (Error <call> StackOverflow) after the finished ones. Without the
% scope nothing charged the balance and the goal recursed until the host stack
% gave out: (bounded-factorial 5) under a depth of 20 answered 120 and then
% raised a 1Gb stack overflow where the run door answers the error
% [tested: tests/test_cmetta.c,
% test_a_stack_depth_pragma_bounds_an_evaluated_goal; commit=WORKTREE].
metta_c_open_eval(Goal, Space, Inferences, Id) :-
    space_module(Space, Module),
    metta_host_inference_budget(
        metta_run_with_fuel(Out, Answer, with_metta_module(Module, eval(Goal, Out))),
        Inferences, Bounded),
    metta_host_hold(Answer, Bounded, Engine),
    metta_c_new_cursor(Engine, Id).

% The engine owns source masks, operation traversal and effect composition.
% [tested: tests/test_native_parity.c; commit=91eef0753a3d55913cee42a2d385bbbf008f0be5]
metta_c_effect_plan(Space, Goal, ['EffectPlan', Effect, Operations]) :-
    space_module(Space, Module),
    metta_host_source_effect_plan(Module, Goal, Operations, Effect).

% Stored atoms unifying a pattern, which is the primitive door. The language's
% own (match ...) with its template is reached through metta_c_open_eval/4.
metta_c_open_match(Pattern, Space, Inferences, Id) :-
    metta_host_inference_budget(metta_host_stored(Space, Pattern),
                                Inferences, Bounded),
    metta_host_hold(Pattern, Bounded, Engine),
    metta_c_new_cursor(Engine, Id).

% The engine owns algebra selection, carrier validation and annotation flow.
% The pair is data carried by the ordinary answer cursor.
% [tested: test_algebras_are_scoped_engine_data; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
metta_c_open_under([Algebra, Goal], Space, Inferences, Id) :-
    space_module(Space, Module),
    metta_host_inference_budget(
        with_metta_module(Module,
            metta_with_under(Algebra, metta_c_annotated(Space, Algebra, Goal, Out, K))),
        Inferences, Bounded),
    metta_host_hold([Out, K], Bounded, Engine),
    metta_c_new_cursor(Engine, Id).

% The fuel scope is metta_c_open_eval/4's, opened inside the algebra's context,
% so a branch that runs out answers its error with an annotation as every other
% answer does, which is the shape the Python seat's under path answers.
metta_c_annotated(Space, Algebra, Goal, Out, K) :-
    ( metta_algebra_one(Space, One)
    -> metta_with_trailed('$metta_answer_k', One,
                         (metta_run_with_fuel(Value, Out, eval(Goal, Value)),
                          metta_annotation(Space, K)))
    ; throw(error(existence_error(algebra, Algebra),
                  context(mt_eval_under,
                          'declare an (algebra ...) row in &metta before selecting it')))
    ).

% Cursor owners are records rather than dynamic rows: close erases the owner
% at once, where a retracted row stays in the clause index until clause
% collection under SWI's logical update view.
metta_c_new_cursor(Engine, cursor(Id, Ref, Engine)) :-
    setup_call_catcher_cleanup(
        true,
        ( flag(metta_c_cursor_id, Previous, Previous + 1),
          Id is Previous + 1,
          recorda('$cmetta_cursors', Engine, Ref) ),
        Catcher,
        ( Catcher == exit -> true ; metta_host_hold_close(Engine) )).

% [] is exhaustion and [Answer] is one answer, so the C half needs no
% sentinel. A closed cursor is a caller bug rather than an empty stream, so it
% raises. The budget needs nothing here: it rides in the engine goal, so a
% spent cursor raises from the held-cursor service on the pull that spends it, and an
% unbounded cursor carries no wrapper and pays nothing.
metta_c_next(Id, Ref, Seconds, Answer) :-
    (   recorded('$cmetta_cursors', Engine, Ref)
    ->  true
    ;   throw(error(existence_error(cmetta_cursor, Id),
                    context(metta_c_next/4, 'this cursor is closed')))
    ),
    metta_c_pull(Engine, Seconds, Answer).

metta_c_pull(Engine, Seconds, Answer) :-
    metta_c_timed(metta_host_hold_next(Engine, Term), Seconds, Pull),
    (   call(Pull)
    ->  Answer = [Term]
    ;   Answer = []
    ).

% erase/1 removes the record even when its erase listener raises. Preserve the
% outcome under the mutex, then destroy the claimed engine outside it. Setup
% protects the ownership transfer from asynchronous interrupts until cleanup
% is installed. Cleanup callbacks may themselves open or close cursors.
metta_c_close(Ref) :-
    setup_call_cleanup(
        with_mutex('$cmetta_cursors',
                   ( recorded('$cmetta_cursors', Engine, Ref)
                   -> catch((erase(Ref) -> Outcome = true ; Outcome = fail),
                            Error, Outcome = throw(Error))
                   ;  Engine = none, Outcome = true )),
        Outcome,
        ( Engine == none -> true
        ; metta_host_hold_close(Engine) )).

%%%%%%%%%% Spaces %%%%%%%%%%

% Callback scopes use the published coordinator. The notification distinguishes
% rollback from a failure in observation after the database already committed.
% [tested: tests/test_transactions.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
metta_c_transaction(Ticket) :-
    metta_transaction_notified('$cmetta_tx_body'(Ticket),
                               '$cmetta_tx_outcome'(Ticket, true),
                               '$cmetta_tx_outcome'(Ticket, false)).
metta_c_speculate(Ticket) :- metta_speculate('$cmetta_tx_body'(Ticket)).

% Observe only the innermost transaction. C roots this goal during its callback
% and compares compound identity before mutating its non-Prolog registry.
metta_c_transaction_scope(Scope) :-
    ( current_transaction(Goal) -> Scope = Goal ; Scope = [] ).
%
% metta_add_atoms/2 rather than a per-atom write: it is where the rule that a
% batch may be judged, journalled and announced once lives, and bypassing it
% is how a seat grows its own half of the write path.
metta_c_add(Space, Term) :- metta_add_atoms(Space, [Term]).

% The batch form preserves the engine door's transaction and announcement
% boundary instead of paying it once per term.
metta_c_add_all(Space, Terms) :- metta_add_atoms(Space, Terms).

% The engine's verdict is the plain boolean `true` when the atom was there
% [measured 2026-08-27]; anything else means it was not, and the C half is
% told which rather than being left to infer it from a count.
metta_c_remove(Space, Term, Removed) :-
    'subtract-atom'(Space, Term, Verdict),
    ( Verdict == true -> Removed = true
    ; Verdict == false -> Removed = false
    ; Verdict = ['Error', _, Reason]
    -> throw(error(cmetta_operation_failed(mt_del, Reason), none))
    ; throw(error(type_error(boolean, Verdict), context(metta_c_remove/3, 'subtraction verdict')))
    ).

metta_c_count(Space, Count) :-
    aggregate_all(count, metta_host_stored(Space, _), Count).

metta_c_clear(Space) :- metta_host_clear_space(Space).
metta_c_drop_space(Space) :-
    metta_assert_space_releasable(Space),
    metta_release_space(Space).

% The engine's own counters: statistics/2 inferences and cputime, the
% garbage_collection triple, and the thread's answer-table bytes. The C half
% samples this twice and subtracts, because C has no with-block and two
% samples with a subtraction is what getrusage and clock_gettime already
% taught it. Same six the Python seat reads, so a measurement taken in one
% seat is comparable to one taken in the other.
metta_c_stats([Inferences, CpuTime, GcCount, GcFreed, GcTimeMs, TableBytes]) :-
    statistics(inferences, Inferences),
    statistics(cputime, CpuTime),
    statistics(garbage_collection, [GcCount, GcFreed, GcTimeMs|_]),
    statistics(table_space_used, TableBytes).

% Whether this atom is a space, asked of every atom the C half decodes and of
% the atom itself. metta_space_operand/1 is the test the engine's own species
% classifier consults (engine/metta/types.pl, get_type_candidate/2 answering
% SpaceType), and the wire codec's `p` tag asks the same one, so this seat and
% the Python seat classify an atom alike. get-metatype is a DIFFERENT question
% since 2026-09-05, upstream PeTTa's one about whether a function carries the
% name, and it calls `&self` a Symbol. CODEC.md's "The question p asks"
% section states the rule and its price.
%
% It used to be metta_space_names/1, the same set as a sorted LIST: the C half
% rebuilt two findalls, an append and a sort for every answer it decoded and
% then scanned the strings. This is one indexed lookup and nothing to go
% stale.
metta_c_space_operand(Name) :- metta_space_operand(Name).

% A rational's two halves as integers, so the C half can carry an exact ratio
% without linking GMP or parsing the 1r3 spelling itself.
metta_c_rational_parts(Rational, Numerator, Denominator) :-
    Numerator is numerator(Rational),
    Denominator is denominator(Rational).

%%%%%%%%%% Publishing a C function %%%%%%%%%%
%
% The engine's four-call host protocol, the same one the Python seat performs:
% prove the name is free, assert the dispatch clause, then make the engine
% treat the name as a function. The compiled predicate carries one extra
% output argument, which is the engine's own convention.
metta_c_register_op(Name0, Arity, Kind) :-
    metta_c_atom(Name0, Name),
    PredArity is Arity + 1,
    metta_host_open_function(Name, c, PredArity),
    metta_c_retract_op(Name, Arity),
    length(Args, Arity),
    append(Args, [Result], HeadArgs),
    Head =.. [Name | HeadArgs],
    % Into &self's module, which every other space inherits, so the operation
    % is callable from all of them.
    space_module('&self', Base),
    assertz(Base:(Head :- metta_c_dispatch(Name, Args, Result))),
    assertz(metta_c_op_spec(Name, Arity, Kind)),
    metta_c_sync_effect(Name),
    % Adopt AFTER the dispatch clause is in place: the engine marks the name a
    % function of the base tier, refreshes dependents against the clause that
    % already exists, and claims the name for the c tier last.
    metta_host_adopt_function(Name, c, Kind, PredArity).

% The foreign predicate lives in the C half. Reaching it through one named
% predicate keeps every generated clause identical and gives the engine a
% single goal shape to recognise below.
metta_c_dispatch(Name, Args, Result) :- '$cmetta_dispatch'(Name, Args, Result).

% The engine asks who a dispatch goal really is, so a purity refusal names the
% operation rather than this file's dispatcher.
:- multifile seam:effect_operation_name/3.
seam:effect_operation_name(metta_c_dispatch(Name, Args, _), Name, Arity) :-
    length(Args, Arity).

metta_c_unregister_op(Name0) :-
    metta_c_atom(Name0, Name),
    forall(metta_c_op_spec(Name, Arity, _), metta_c_retract_op(Name, Arity)),
    ( metta_host_forget_function(Name) -> true ; true ).

metta_c_retract_op(Name, Arity) :-
    PredArity is Arity + 1,
    space_module('&self', Base),
    functor(Head, Name, PredArity),
    retractall(Base:Head),
    retractall(metta_c_op_spec(Name, Arity, _)),
    metta_c_sync_effect(Name),
    ( metta_host_drop_function(Name, PredArity) -> true ; true ).

% Publishing a classification only as function ownership left the shared
% planner treating C callbacks as oracleIO. The catalog effect is the common
% classifier input; overloads compose through the engine's own lattice.
% [tested: tests/test_native_parity.c; commit=91eef0753a3d55913cee42a2d385bbbf008f0be5]
metta_c_sync_effect(Name) :-
    forall(retract(metta_c_op_effect(Name, Previous)),
           metta_host_remove_reported('&metta', [effect, Name, Previous], _)),
    findall(Kind, metta_c_op_spec(Name, _, Kind), Classes),
    ( Classes == [] -> true
    ; metta_effect_compose(Classes, Effect),
      metta_add_atoms('&metta', [[effect, Name, Effect]]),
      assertz(metta_c_op_effect(Name, Effect))
    ).

% A C operation's refusal, rendered the way every other engine diagnostic is.
% Without this SWI answers "Unknown message: cmetta_operation_failed(...)",
% which tells a caller the shape of the complaint rather than the complaint
% [measured 2026-08-27].
% error_message//1 rather than message//1: SWI dispatches the FORMAL half of
% an error(Formal, Context) pair through this hook, and a message//1 clause
% for the formal is never reached [measured 2026-08-27: SWI answered
% "Unknown error term: cmetta_operation_failed(...)" with the message//1
% clause in place].
:- multifile prolog:error_message//1.
prolog:error_message(cmetta_operation_failed(Name, Why)) -->
    [ 'the C operation ~w refused this application: ~w'-[Name, Why] ].

%%%%%%%%%% A C value crossing MeTTa untouched %%%%%%%%%%
%
% A cmetta_object is a blob the C half owns. It reaches MeTTa as an ordinary
% grounded value, compares by identity and prints through the C write
% callback. One carrying a function pointer is APPLICABLE, which is how C
% answers what a Python callable answers. Type names and liveness come from
% the same owned box; inference, subtyping and dispatch stay in the engine.
% [tested: test_native_object_types_reach_engine_dispatch; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
:- multifile seam:host_object/1.
seam:host_object(Obj) :-
    blob(Obj, cmetta_object),
    '$cmetta_object_live'(Obj).

:- multifile seam:grounded_class_type/2.
seam:grounded_class_type(Obj, Type) :-
    blob(Obj, cmetta_object),
    '$cmetta_object_type'(Obj, Type).

:- multifile seam:grounded_applicable/1.
seam:grounded_applicable(Obj) :-
    blob(Obj, cmetta_object),
    '$cmetta_object_callable'(Obj).

% A C function takes positional arguments only, so a `(Kwargs ...)` written
% after one names keywords it cannot receive and the application stays
% unreduced, the way any value that is not an operation stays.
:- multifile seam:grounded_apply/4.
seam:grounded_apply(Obj, Args, [], Result) :-
    blob(Obj, cmetta_object),
    '$cmetta_apply'(Obj, Args, Result).

% Candidates cross through the retained callback cursor; structural unification
% binds the original operand's variables. The engine decides which value owns
% matching and bypasses this hook when the other operand is a free variable.
% [tested: tests/test_matchers.c; commit=91eef0753a3d55913cee42a2d385bbbf008f0be5]
:- multifile seam:matchable_value/1.
seam:matchable_value(Obj) :-
    blob(Obj, cmetta_object),
    '$cmetta_object_matchable'(Obj).

:- multifile seam:custom_match/2.
seam:custom_match(Obj, Other) :-
    blob(Obj, cmetta_object),
    '$cmetta_match'(Obj, Other, Candidate),
    Other = Candidate.

%%%%%%%%%% Text coercion %%%%%%%%%%

% A grounded callable returns one value. Explicit iteration consumes that
% value, preserving the engine's grounded_apply ownership decision.
% [tested: tests/test_iterators.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
:- multifile seam:extension_builtin/2.
seam:extension_builtin('c-iter', writesState).
'c-iter'(Stream, Out) :- '$cmetta_stream'(Stream, Out).
%
% A C string reaches Prolog as whichever of atom or string the C half chose;
% both spellings arrive here so neither side has to care.
metta_c_text(In, Out) :- string(In), !, Out = In.
metta_c_text(In, Out) :- atom_string(In, Out).

metta_c_atom(In, Out) :- atom(In), !, Out = In.
metta_c_atom(In, Out) :- atom_string(Out, In).

%%%%%%%%%% Extending this seat %%%%%%%%%%
%
% The Prolog half of the three doors a library outside this repository
% registers through. Each one dispatches into C, and the C half holds the
% ROWS: this file knows only that a space has a C provider, never which
% library opened it, so nothing here names one.

% Which spaces a C provider backs. The ownership guard every clause below
% leads with, and a pure lookup, so anything may ask it without performing an
% operation, which is what the foreign-space protocol requires.
:- dynamic metta_c_provider/1.
:- dynamic metta_c_capability/2.
:- dynamic metta_c_transactional_provider/1.

% The engine's ownership seam is answered by a ROW asserted when a provider
% opens, not by a resident clause reading the registry above. The engine asks
% seam:foreign_space/1 once per space operation, so a resident clause is tried
% on every operation in every process that loads this seat, whether or not a C
% provider was ever opened. Measured on benchmarks/cases.c's space-pair case,
% 20,000 add-and-match pairs: 1,140,032 inferences with the row asserted at the
% door against 1,160,032 with a resident clause, one inference per pair
% [measured 2026-09-07: 1140032 inferences against 1160032 over 20,000
% pairs; command=CHECK_PY=$VENV/bin/python sh extensions/cmetta/bench.sh
% space-pair; fixture=extensions/cmetta/benchmarks/cases.c, case
% space-pair; commit=9ee28a945da58dfdef86119ea082609bd5975aed].
% engine/spaces/foreign.pl states the same rule for the claim door beside its
% own measurement: the ownership question is answered off the operation path or
% not at all. The declaration is guarded because a seat that has already made
% the predicate static leaves nothing to convert, which is the configuration
% extensions/node/bridge.pl names in its own refusal.
:- catch(dynamic(seam:foreign_space/1), _, true).

% Taking a name. The engine's claim door goes first, so a space another
% provider already owns is refused BY NAME here instead of resolving by clause
% order later, which is what cmetta.h promises mt_provider_open() does. Then
% this seat's two rows: its own registry, which every hook below guards on, and
% the engine's ownership row. A space this seat already backs is refused rather
% than re-pointed at a second store, because the atoms live in the FIRST
% provider's memory and nothing would move them.
metta_c_open_provider(Space, Capabilities, Transactional) :-
    (   metta_c_provider(Space)
    ->  throw(error(permission_error(open, metta_space, Space),
                    context(metta_c_open_provider/1,
                            'a space already backed by a C provider')))
    ;   metta_c_require_space_name(Space),
        metta_claim_space(Space, cmetta),
        assertz(metta_c_provider(Space)),
        assertz(seam:foreign_space(Space)),
        forall(member(Capability, Capabilities),
               ( metta_require_foreign_capability(Space, Capability),
                 assertz(metta_c_capability(Space, Capability)) )),
        ( Transactional == true
        -> assertz(metta_c_transactional_provider(Space)),
           metta_add_atoms('&metta', [[writes, Space, transactional]])
        ; true )
    ).

% Giving it back, in the reverse order and quietly, because a teardown path may
% run twice. retract/1 rather than retractall/1: the row this seat wrote is a
% FACT, and retractall/1 unifies HEADS alone, so it would take another seat's
% bridging clause for the same name with it.
metta_c_close_provider(Space) :-
    ( retract(metta_c_transactional_provider(Space))
    -> metta_host_remove_reported('&metta', [writes, Space, transactional], _)
    ; true ),
    retractall(metta_c_capability(Space, _)),
    retractall(metta_c_provider(Space)),
    ( retract(seam:foreign_space(Space)) -> true ; true ),
    metta_disclaim_space(Space, cmetta).

% The ampersand rule, at the door. tests/prolog/static_checks.pl reads
% seam:foreign_space/1 clause HEADS in the source to enforce it and this seat
% writes none, so the refusal lives where the Python and Node seats put theirs.
% metta_space_name/1 is the engine's own test, so a parametric name is admitted
% on the same terms as everywhere else rather than on this seat's guess.
metta_c_require_space_name(Space) :-
    (   metta_space_name(Space)
    ->  true
    ;   throw(error(cmetta_bad_space_name(Space),
                    context(metta_c_open_provider/1,
                            'a space name begins with an ampersand')))
    ).

:- multifile prolog:error_message//1.
prolog:error_message(cmetta_bad_space_name(Name)) -->
    [ 'a space a C library backs must be named with a leading ampersand, and \c
       ~w is not'-[Name] ].

% The C vtable is the source of capabilities. Missing callbacks are refused by
% the engine before dispatch, while a callback's error crosses as an exception.
% [tested: tests/test_providers.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
:- multifile seam:foreign_capability/2.
seam:foreign_capability(Space, Capability) :-
    metta_c_provider(Space),
    metta_c_capability(Space, Capability).

seam:foreign_add(Space, Atom) :-
    metta_c_provider(Space), !,
    '$cmetta_provider'(Space, add, Atom, _).

seam:foreign_remove(Space, Atom, Removed) :-
    metta_c_provider(Space), !,
    '$cmetta_provider'(Space, remove, Atom, Removed).

seam:foreign_atoms(Space, Atom) :-
    metta_c_provider(Space), !,
    '$cmetta_provider_query'(Space, [_, 0], Atom).

% The provider may over-approximate; C's nondeterministic dispatch unifies each
% candidate against Pattern before yielding. The bound remains advisory.
seam:foreign_match(Space, Pattern, Options) :-
    metta_c_provider(Space), !,
    ( memberchk(limit(Limit), Options) -> true ; Limit = 0 ),
    '$cmetta_provider_query'(Space, [Pattern, Limit], Pattern).

seam:foreign_clear(Space) :-
    metta_c_provider(Space), !,
    '$cmetta_provider'(Space, clear, 0, _).

% Capture once before begin. Completion retains the registration itself, so
% replacing a name cannot redirect its commit or rollback to another backend.
% [source: engine/ext_points.pl:foreign_participant/3; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
:- multifile seam:foreign_participant/3.
seam:foreign_participant(Space, Identity, user:metta_c_capture_provider(Space, Identity)) :-
    metta_c_provider(Space),
    '$cmetta_provider_identity'(Space, Identity).

metta_c_capture_provider(Space, Identity,
        transaction(user:'$cmetta_provider_finish'(Held, begin),
                    user:'$cmetta_provider_finish'(Held, commit),
                    user:'$cmetta_provider_finish'(Held, rollback))) :-
    '$cmetta_provider_capture'(Space, Identity, Held).

% How a C object renders. An ownership seam: the C half fails when no row
% names that object's type, and the display renderer falls back to the term's
% own text exactly as it does with no provider at all.
:- multifile seam:grounded_text/2.
seam:grounded_text(Obj, Text) :-
    blob(Obj, cmetta_object),
    '$cmetta_repr'(Obj, Text).

% A directory of MeTTa or Prolog sources a library ships, under an alias.
metta_c_library_path(Alias, Directory, Ok) :-
    register_metta_library_path(Alias, Directory, Ok).

% Clauses are the engine's event subscribers, including its commit buffering.
% Each clause head carries its space and pattern, so the engine can index it.
% Registration and erasure are trailed by the same transaction as C rows.
% [source: engine/ext_points.pl:atom_added/2, atom_removed/2; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
:- dynamic metta_c_subscription/3.
metta_c_subscribe(Name, Token, Space, Pattern) :-
    metta_c_require_space_name(Space),
    metta_require_events(Space, 'be subscribed to'),
    assertz((seam:atom_added(Space, Pattern) :-
              user:'$cmetta_notify'(Name, Token, true, Pattern)), Added),
    assertz((seam:atom_removed(Space, Pattern) :-
              user:'$cmetta_notify'(Name, Token, false, Pattern)), Removed),
    assertz(metta_c_subscription(Name, Added, Removed)).

metta_c_unsubscribe(Name) :-
    retract(metta_c_subscription(Name, Added, Removed)),
    erase(Added), erase(Removed).
