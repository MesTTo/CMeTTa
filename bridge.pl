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
%   - '$cmetta_dispatch'/3 and '$cmetta_object'/1 are foreign predicates the C
%     half registers before consulting the engine. This file LOADS without
%     them, because the static gate consults it directly with no C host in the
%     process, so nothing here may call one at load time.
%   - the C half runs each call inside its own PL_open_foreign_frame, so a
%     term handed out here stays valid exactly as long as that frame
% Guarantees:
%   - metta_c_next/3 computes at most one answer per call, so a host that
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
%   - metta_c_close/1 leaves nothing behind and says nothing: it retracts the
%     row before it destroys the engine, so a cursor that reached the end of
%     its answers closes without arming the host's error state
%     [tested: tests/test_cmetta.c, test_closing_an_exhausted_cursor_is_quiet;
%     commit=c530ccb8fb7d0a5b2aa53df6e9f981ada9f81be8]
%   - metta_c_close/1 on an Id that is no longer in the table succeeds
%     quietly, so a host may close before it frees
%     [assumed 2026-08-31: nothing in the tree closes one twice. cmetta.h has
%     no door that closes a cursor without also freeing it, so the C suite
%     cannot reach the second close; the branch is here so a host that grows
%     one is not punished for it]
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
% Owns: one SWI engine per open cursor, released by metta_c_close/1, which the
%   C half calls from cmetta_answers_free().
% Decides: verbosity is set explicitly at boot rather than inherited from argv,
%   because filereader.pl reads the CLI at load time and an embedded host has
%   none. The setting itself is the engine's metta_host_set_silent/1, not a
%   copy here: this seat filed the duplication as C2 and the engine took it.
% Open Obligations:
%   To Do: None
%   Hacks: None
%   Future Enhancements: None

:- use_module(library(time), [call_with_time_limit/2]).

:- dynamic metta_c_cursor/2.
:- dynamic metta_c_op_spec/3.
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
    setup_call_cleanup(nb_setval('$metta_c_capture', true),
                       catch(print_message(error, Ball), _, true),
                       nb_setval('$metta_c_capture', false)),
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

%%%%%%%%%% Text, through the engine's own reader and writer %%%%%%%%%%
%
% sread_with_names/3 rather than sread/2, because the C side wants the source
% spellings of the variables it is about to hand back to a caller who wrote
% them.
metta_c_read(Source, Term, Names) :-
    metta_c_text(Source, S),
    sread_with_names(S, Term, Names).

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
metta_c_counted(Goal, Inferences) :- Inferences =< 0, !, call(Goal).
metta_c_counted(Goal, Inferences) :-
    call_with_inference_limit(Goal, Inferences, Result),
    (   Result == inference_limit_exceeded
    ->  throw(error(metta_control_signal(inference_limit, Inferences),
                    context(metta, inference_limit)))
    ;   true
    ).

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
% The engine handle stays HERE and the C half holds an integer. A blob would
% work, but an integer is what survives being stored in a C struct across
% frames without a record, and the cursor table is this file's to own anyway.
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
metta_c_open_eval(Goal, Space, Inferences, Id) :-
    space_module(Space, Module),
    metta_host_inference_budget(with_metta_module(Module, eval(Goal, Out)),
                                Inferences, Bounded),
    engine_create(Out, Bounded, Engine),
    metta_c_new_cursor(Engine, Id).

% Stored atoms unifying a pattern, which is the primitive door. The language's
% own (match ...) with its template is reached through metta_c_open_eval/4.
metta_c_open_match(Pattern, Space, Inferences, Id) :-
    metta_host_inference_budget(metta_host_stored(Space, Pattern),
                                Inferences, Bounded),
    engine_create(Pattern, Bounded, Engine),
    metta_c_new_cursor(Engine, Id).

metta_c_new_cursor(Engine, Id) :-
    flag(metta_c_cursor_id, Previous, Previous + 1),
    Id is Previous + 1,
    assertz(metta_c_cursor(Id, Engine)).

% [] is exhaustion and [Answer] is one answer, so the C half needs no
% sentinel. A closed cursor is a caller bug rather than an empty stream, so it
% raises. The budget needs nothing here: it rides in the engine goal, so a
% spent cursor raises out of engine_next/2 on the pull that spends it, and an
% unbounded cursor carries no wrapper and pays nothing.
metta_c_next(Id, Seconds, Answer) :-
    (   metta_c_cursor(Id, Engine)
    ->  true
    ;   throw(error(existence_error(cmetta_cursor, Id),
                    context(metta_c_next/3, 'this cursor is closed')))
    ),
    metta_c_pull(Engine, Seconds, Answer).

metta_c_pull(Engine, Seconds, Answer) :-
    metta_c_timed(engine_next(Engine, Term), Seconds, Pull),
    (   call(Pull)
    ->  Answer = [Term]
    ;   Answer = []
    ).

% Idempotent: a host that closes after exhaustion, and again from
% cmetta_answers_free(), finds nothing the second time and is at peace.
metta_c_close(Id) :-
    (   retract(metta_c_cursor(Id, Engine))
    ->  catch(engine_destroy(Engine), error(existence_error(_, _), _), true)
    ;   true
    ).

%%%%%%%%%% Spaces %%%%%%%%%%
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
    metta_host_remove_reported(Space, Term, Verdict),
    ( Verdict == true -> Removed = true ; Removed = false ).

metta_c_count(Space, Count) :-
    aggregate_all(count, metta_host_stored(Space, _), Count).

metta_c_clear(Space) :- metta_host_clear_space(Space).

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
% classifier consults (engine/metta/types.pl, metatype_of/2), and the wire
% codec's `p` tag asks the same one, so this seat, the Python seat and
% get-metatype classify an atom alike. CODEC.md's "The question p asks"
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
    ( metta_host_drop_function(Name, PredArity) -> true ; true ).

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
% answers what a Python callable answers: '$cmetta_object'(Blob) succeeds for
% any of ours, and '$cmetta_apply'/3 refuses one with no function.
:- multifile seam:grounded_applicable/1.
seam:grounded_applicable(Obj) :-
    blob(Obj, cmetta_object),
    '$cmetta_object_callable'(Obj).

:- multifile seam:grounded_apply/3.
seam:grounded_apply(Obj, Args, Result) :-
    blob(Obj, cmetta_object),
    '$cmetta_apply'(Obj, Args, Result).

%%%%%%%%%% Text coercion %%%%%%%%%%
%
% A C string reaches Prolog as whichever of atom or string the C half chose;
% both spellings arrive here so neither side has to care.
metta_c_text(In, Out) :- string(In), !, Out = In.
metta_c_text(In, Out) :- atom_string(In, Out).

metta_c_atom(In, Out) :- atom(In), !, Out = In.
metta_c_atom(In, Out) :- atom_string(Out, In).
