% Purpose: the Prolog half of the C binding. It runs a MeTTa program, holds a
%   query open as a resumable answer stream, publishes C functions as MeTTa
%   operations, and hands each answer back as an ENGINE TERM for the C half to
%   walk directly.
% Assumes:
%   - every engine predicate called here carries a seam:kind/2 in
%     engine/ext_points.pl, service or host_service [tested:
%     tests/prolog/static_checks.pl, a_host_binding_calls_only_published_surface,
%     which reports "every one of 3 host bindings" with this file's row present;
%     commit=0c544dba163996ab34fec1cb574f5f4faf8b53f0]
%   - '$cetta_dispatch'/3 and '$cetta_object'/1 are foreign predicates the C
%     half registers before consulting the engine. This file LOADS without
%     them, because the static gate consults it directly with no C host in the
%     process, so nothing here may call one at load time.
%   - the C half runs each call inside its own PL_open_foreign_frame, so a
%     term handed out here stays valid exactly as long as that frame
% Guarantees:
%   - petta_c_next/3 computes at most one answer per call, so a host that
%     stops pulling leaves the rest of an infinite stream uncomputed
%   - a cursor opened with a positive Inferences stops once its ENGINE has
%     spent that many, cumulatively across pulls, because the budget is built
%     into the engine goal by metta_host_inference_budget/3 [tested:
%     tests/test_cetta.c, test_a_bound_stops_a_runaway_and_says_so;
%     commit=WORKTREE]
%   - petta_c_close/1 is idempotent
%   - no answer is encoded, tagged, or stringified on the way out: the C half
%     receives the engine's own term. This seat is in-process with the engine
%     and has no marshalling boundary to cross, which is the whole reason it
%     exists; see C6 in ai-cetta-c-constraints.md
%   - an operation published from C is registered through the engine's own
%     four-call host protocol (open, assert, adopt, release), so a name another
%     tier owns is refused rather than clobbered
% Owns: one SWI engine per open cursor, released by petta_c_close/1, which the
%   C half calls from cetta_answers_free().
% Decides: verbosity is set here rather than inherited from argv, because
%   filereader.pl reads the CLI at load time and an embedded host has none.
% Open Obligations:
%   To Do: None
%   Hacks: None
%   Future Enhancements: None

:- use_module(library(time), [call_with_time_limit/2]).

:- dynamic petta_c_cursor/2.
:- dynamic petta_c_op_spec/3.
:- dynamic petta_c_captured/1.

%%%%%%%%%% Verbosity %%%%%%%%%%
%
% engine/filereader.pl decides silent/1 from the CLI argv at load time. A C
% host has no CLI, so this is the door. retractall first: two contradictory
% silent/1 clauses would leave the engine on whichever is first.
petta_c_set_silent(Silent) :-
    retractall(silent(_)),
    assertz(silent(Silent)).

%%%%%%%%%% Rendering an exception %%%%%%%%%%
%
% The C half catches the ball itself through PL_exception(), so nothing here
% needs to turn an outcome into data the way the Node bridge does. What it
% cannot do in C is render the ball the way SWI would print it, so it asks
% here. print_message/2 goes through exactly the machinery the console would
% have used, and the hook below takes the lines instead of letting them out.
petta_c_error_text(Ball, Text) :-
    retractall(petta_c_captured(_)),
    setup_call_cleanup(nb_setval('$petta_c_capture', true),
                       catch(print_message(error, Ball), _, true),
                       nb_setval('$petta_c_capture', false)),
    (   petta_c_captured(Rendered)
    ->  Text = Rendered
    ;   term_string(Ball, Text)
    ),
    retractall(petta_c_captured(_)).

% Deaf outside petta_c_error_text/2, and it has to be: a hook that succeeds
% suppresses the message, so an always-on one would swallow the loader's own
% diagnostics.
:- multifile user:message_hook/3.
user:message_hook(_, _, Lines) :-
    nb_current('$petta_c_capture', true),
    print_message_lines(atom(Text), '', Lines),
    assertz(petta_c_captured(Text)).

%%%%%%%%%% Text, through the engine's own reader and writer %%%%%%%%%%
%
% sread_with_names/3 rather than sread/2, because the C side wants the source
% spellings of the variables it is about to hand back to a caller who wrote
% them.
petta_c_read(Source, Term, Names) :-
    petta_c_text(Source, S),
    sread_with_names(S, Term, Names).

% swrite/2 is the round-trip writer and refuses what it could not read back;
% sdisplay/2 is presentation and renders it anyway. A caller asking to SEE an
% atom gets presentation, which is the same choice the command line makes.
% Names is the caller's own Name-Var pairs, so a variable prints as the name
% its author wrote. With no names the writer numbers them $_0, $_1, which is
% correct and unhelpful to somebody who wrote $x.
petta_c_show(Term, Names, Text) :- sdisplay_with_names(Term, Names, Text).

%%%%%%%%%% Running a program %%%%%%%%%%
%
% The grouping walk, the working-dir defaulting and the load lifecycle are the
% engine's own host run and load surface, shared with every other seat. One
% group per ! directive, in source order.
petta_c_run(Source, Space, Seconds, Inferences, Groups) :-
    petta_c_text(Source, S),
    petta_c_bounded(metta_host_run_source(S, Space, [], Groups),
                    Seconds, Inferences).

petta_c_load(File, Space, Seconds, Inferences, Groups) :-
    petta_c_atom(File, FA),
    petta_c_bounded(metta_host_load_file(FA, Space, Groups),
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
petta_c_bounded(Goal, Seconds, Inferences) :-
    petta_c_timed(Goal, Seconds, Timed),
    petta_c_counted(Timed, Inferences).

petta_c_timed(Goal, Seconds, Goal) :- Seconds =< 0, !.
petta_c_timed(Goal, Seconds, Timed) :-
    Timed = catch(call_with_time_limit(Seconds, Goal),
                  time_limit_exceeded,
                  throw(error(cetta_limit(seconds, Seconds), _))).

% The inference bound raises the ENGINE's reserved limit envelope rather than a
% second ball of this seat's own. The cursor door below reaches that envelope
% anyway, because the budget it installs is the engine's, and one bound wearing
% two ball shapes depending on which door produced it is a second name for one
% thing. The wall bound keeps its own ball because it IS this seat's: it is
% applied per pull, which is a policy the engine does not have.
petta_c_counted(Goal, Inferences) :- Inferences =< 0, !, call(Goal).
petta_c_counted(Goal, Inferences) :-
    call_with_inference_limit(Goal, Inferences, Result),
    (   Result == inference_limit_exceeded
    ->  throw(error(metta_control_signal(inference_limit, Inferences),
                    context(petta, inference_limit)))
    ;   true
    ).

% The C half asks whether a ball it caught is a bound rather than a fault, so
% a caller can tell "I stopped it" from "it broke". Both sources answer here:
% this seat's own wall ball, and the engine's reserved envelope, which arrives
% from a cursor budget and from a program's own (pragma! max-inferences N)
% alike. Before the envelope was listed, a program that spent its own pragma
% budget reached a C caller as CETTA_ERROR, a fault.
petta_c_limit_ball(error(cetta_limit(Kind, Bound), _), Kind, Bound).
petta_c_limit_ball(error(metta_control_signal(Signal, Bound), _), Kind, Bound) :-
    petta_c_limit_kind(Signal, Kind).

petta_c_limit_kind(inference_limit, inferences).
petta_c_limit_kind(time_limit, seconds).

% Only the wall ball is rendered here; engine/metta/registration.pl renders the
% reserved envelope, so the sentence does not exist twice.
:- multifile prolog:error_message//1.
prolog:error_message(cetta_limit(seconds, Bound)) -->
    [ 'the evaluation passed its ~w second bound and was stopped'-[Bound] ].

% One answer, split into the three things the C half reads: the term, the
% source names of its free variables, and the engine's own rendering.
petta_c_answer_parts('$petta_answer'(Term, NameState), Term, Names, Text) :- !,
    petta_name_pairs(NameState, Names),
    sdisplay_with_names(Term, NameState, Text).
petta_c_answer_parts(Term, Term, [], Text) :-
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
% loop. Replayed against a workload costing about 402 inferences per answer,
% the meter reported 1,001 spent under a 1,000 budget while the engine had
% really spent 201,507, and 100,002 under 100,000 against 20,150,410
% [measured 2026-08-27: ai-tmp/proto_cetta_design.pl; commit=WORKTREE].
%
% The sweep recorded here as evidence, 1,000 stopping at 1,004 and 100,000 at
% 100,004, is what that looks like from inside: a constant four-inference
% overshoot at every scale is a fixed charge per pull, so the reported total
% tracks the budget by construction whatever the engine is doing. It was a
% correct measurement of the wrong counter.
%
% The earlier sweep in the same note is sound and still worth keeping: an
% engine goal wrapped in call_with_inference_limit/3 ALONE fired at budgets of
% 500, 1,000 and 2,000 and never fired at 5,000, 10,000 or 20,000 over the same
% endless generator. A cumulative budget cannot behave that way. The reason is
% not that the limiter fails to cross engine_next/2, which is what this note
% concluded; it is that SWI bounds inferences per SOLUTION of the goal, so a
% generator answering cheaply forever is re-armed at every answer and never
% reaches it. The published wrapper keeps that limiter, because it is the only
% one of the two bounds that stops a resume which never yields at all, and adds
% the cumulative check the per-solution contract cannot express.
%
% The wall bound stays per pull, so time between pulls, while the host is doing
% something else, cannot count against it.
petta_c_open_eval(Goal, Space, Inferences, Id) :-
    space_module(Space, Module),
    metta_host_inference_budget(with_metta_module(Module, eval(Goal, Out)),
                                Inferences, Bounded),
    engine_create(Out, Bounded, Engine),
    petta_c_new_cursor(Engine, Id).

% Stored atoms unifying a pattern, which is the primitive door. The language's
% own (match ...) with its template is reached through petta_c_open_eval/4.
petta_c_open_match(Pattern, Space, Inferences, Id) :-
    metta_host_inference_budget(metta_host_stored(Space, Pattern),
                                Inferences, Bounded),
    engine_create(Pattern, Bounded, Engine),
    petta_c_new_cursor(Engine, Id).

petta_c_new_cursor(Engine, Id) :-
    (   aggregate_all(max(N), petta_c_cursor(N, _), Highest)
    ->  Id is Highest + 1
    ;   Id = 1
    ),
    assertz(petta_c_cursor(Id, Engine)).

% [] is exhaustion and [Answer] is one answer, so the C half needs no
% sentinel. A closed cursor is a caller bug rather than an empty stream, so it
% raises. The budget needs nothing here: it rides in the engine goal, so a
% spent cursor raises out of engine_next/2 on the pull that spends it, and an
% unbounded cursor carries no wrapper and pays nothing.
petta_c_next(Id, Seconds, Answer) :-
    (   petta_c_cursor(Id, Engine)
    ->  true
    ;   throw(error(existence_error(cetta_cursor, Id),
                    context(petta_c_next/3, 'this cursor is closed')))
    ),
    petta_c_pull(Engine, Seconds, Answer).

petta_c_pull(Engine, Seconds, Answer) :-
    petta_c_timed(engine_next(Engine, Term), Seconds, Pull),
    (   call(Pull)
    ->  Answer = [Term]
    ;   Answer = []
    ).

% Idempotent: a host that closes after exhaustion, and again from
% cetta_answers_free(), finds nothing the second time and is at peace.
petta_c_close(Id) :-
    (   retract(petta_c_cursor(Id, Engine))
    ->  catch(engine_destroy(Engine), error(existence_error(_, _), _), true)
    ;   true
    ).

%%%%%%%%%% Spaces %%%%%%%%%%
%
% metta_add_atoms/2 rather than a per-atom write: it is where the rule that a
% batch may be judged, journalled and announced once lives, and bypassing it
% is how a seat grows its own half of the write path.
petta_c_add(Space, Term) :- metta_add_atoms(Space, [Term]).

% The engine's verdict is the plain boolean `true` when the atom was there
% [measured 2026-08-27]; anything else means it was not, and the C half is
% told which rather than being left to infer it from a count.
petta_c_remove(Space, Term, Removed) :-
    metta_host_remove_reported(Space, Term, Verdict),
    ( Verdict == true -> Removed = true ; Removed = false ).

petta_c_count(Space, Count) :-
    aggregate_all(count, metta_host_stored(Space, _), Count).

petta_c_clear(Space) :- metta_host_clear_space(Space).

% The engine's own counters: statistics/2 inferences and cputime, the
% garbage_collection triple, and the thread's answer-table bytes. The C half
% samples this twice and subtracts, because C has no with-block and two
% samples with a subtraction is what getrusage and clock_gettime already
% taught it. Same six the Python seat reads, so a measurement taken in one
% seat is comparable to one taken in the other.
petta_c_stats([Inferences, CpuTime, GcCount, GcFreed, GcTimeMs, TableBytes]) :-
    statistics(inferences, Inferences),
    statistics(cputime, CpuTime),
    statistics(garbage_collection, [GcCount, GcFreed, GcTimeMs|_]),
    statistics(table_space_used, TableBytes).

% Which names are executable spaces RIGHT NOW. The C half asks this before it
% calls an ampersand-prefixed atom a space reference, because the prefix alone
% does not decide: `&bar` reads as an ordinary atom and is no space at all
% [measured 2026-08-27, and C5 in ai-cetta-c-constraints.md has the probe].
petta_c_space_names(Names) :- metta_space_names(Names).

% A rational's two halves as integers, so the C half can carry an exact ratio
% without linking GMP or parsing the 1r3 spelling itself.
petta_c_rational_parts(Rational, Numerator, Denominator) :-
    Numerator is numerator(Rational),
    Denominator is denominator(Rational).

%%%%%%%%%% Publishing a C function %%%%%%%%%%
%
% The engine's four-call host protocol, the same one the Python seat performs:
% prove the name is free, assert the dispatch clause, then make the engine
% treat the name as a function. The compiled predicate carries one extra
% output argument, which is the engine's own convention.
petta_c_register_op(Name0, Arity, Kind) :-
    petta_c_atom(Name0, Name),
    PredArity is Arity + 1,
    metta_host_open_function(Name, c, PredArity),
    petta_c_retract_op(Name, Arity),
    length(Args, Arity),
    append(Args, [Result], HeadArgs),
    Head =.. [Name | HeadArgs],
    % Into &self's module, which every other space inherits, so the operation
    % is callable from all of them.
    space_module('&self', Base),
    assertz(Base:(Head :- petta_c_dispatch(Name, Args, Result))),
    assertz(petta_c_op_spec(Name, Arity, Kind)),
    % Adopt AFTER the dispatch clause is in place: the engine marks the name a
    % function of the base tier, refreshes dependents against the clause that
    % already exists, and claims the name for the c tier last.
    metta_host_adopt_function(Name, c, Kind, PredArity).

% The foreign predicate lives in the C half. Reaching it through one named
% predicate keeps every generated clause identical and gives the engine a
% single goal shape to recognise below.
petta_c_dispatch(Name, Args, Result) :- '$cetta_dispatch'(Name, Args, Result).

% The engine asks who a dispatch goal really is, so a purity refusal names the
% operation rather than this file's dispatcher.
:- multifile seam:effect_operation_name/3.
seam:effect_operation_name(petta_c_dispatch(Name, Args, _), Name, Arity) :-
    length(Args, Arity).

petta_c_unregister_op(Name0) :-
    petta_c_atom(Name0, Name),
    forall(petta_c_op_spec(Name, Arity, _), petta_c_retract_op(Name, Arity)),
    ( metta_host_forget_function(Name) -> true ; true ).

petta_c_retract_op(Name, Arity) :-
    PredArity is Arity + 1,
    space_module('&self', Base),
    functor(Head, Name, PredArity),
    retractall(Base:Head),
    retractall(petta_c_op_spec(Name, Arity, _)),
    ( metta_host_drop_function(Name, PredArity) -> true ; true ).

% A C operation's refusal, rendered the way every other engine diagnostic is.
% Without this SWI answers "Unknown message: cetta_operation_failed(...)",
% which tells a caller the shape of the complaint rather than the complaint
% [measured 2026-08-27].
% error_message//1 rather than message//1: SWI dispatches the FORMAL half of
% an error(Formal, Context) pair through this hook, and a message//1 clause
% for the formal is never reached [measured 2026-08-27: SWI answered
% "Unknown error term: cetta_operation_failed(...)" with the message//1
% clause in place].
:- multifile prolog:error_message//1.
prolog:error_message(cetta_operation_failed(Name, Why)) -->
    [ 'the C operation ~w refused this application: ~w'-[Name, Why] ].

%%%%%%%%%% A C value crossing MeTTa untouched %%%%%%%%%%
%
% A cetta_object is a blob the C half owns. It reaches MeTTa as an ordinary
% grounded value, compares by identity and prints through the C write
% callback. One carrying a function pointer is APPLICABLE, which is how C
% answers what a Python callable answers: '$cetta_object'(Blob) succeeds for
% any of ours, and '$cetta_apply'/3 refuses one with no function.
:- multifile seam:grounded_applicable/1.
seam:grounded_applicable(Obj) :-
    blob(Obj, cetta_object),
    '$cetta_object_callable'(Obj).

:- multifile seam:grounded_apply/3.
seam:grounded_apply(Obj, Args, Result) :-
    blob(Obj, cetta_object),
    '$cetta_apply'(Obj, Args, Result).

%%%%%%%%%% Text coercion %%%%%%%%%%
%
% A C string reaches Prolog as whichever of atom or string the C half chose;
% both spellings arrive here so neither side has to care.
petta_c_text(In, Out) :- string(In), !, Out = In.
petta_c_text(In, Out) :- atom_string(In, Out).

petta_c_atom(In, Out) :- atom(In), !, Out = In.
petta_c_atom(In, Out) :- atom_string(Out, In).
