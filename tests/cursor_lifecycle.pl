% Purpose: exercise the C cursor's ownership boundary inside its embedded SWI.
% Owns resources: each test destroys its engines, joins its workers and removes
%   its erase listener even when the assertion fails.
% Guarded by: the bridge's $cmetta_cursors mutex claims each recorded owner;
%   flag/3 counts completed cleanup callbacks atomically across workers.
% Guarantees: competing closes, erase exceptions and queued interrupts destroy
%   the engine exactly once [tested: sh check.sh c-binding; commit=WORKTREE].

:- module(cmetta_cursor_tests,
          [ cursor_census/3,
            arm_erasure_fault/2,
            finish_erasure_fault/2,
            close_live_cursor_twice/0
          ]).
:- use_module(library(plunit)).
:- use_module(library(aggregate), [aggregate_all/3]).
:- use_module(library(thread), [concurrent/3]).

cursor_census(Bytes, Records, Atoms) :-
    (   current_predicate(user:metta_c_cursor/2)
    ->  predicate_property(user:metta_c_cursor(_, _), size(Bytes))
    ;   Bytes = 0
    ),
    aggregate_all(count, recorded('$cmetta_cursors', _, _), Records),
    garbage_collect,
    garbage_collect_atoms,
    statistics(atoms, Atoms).

on_record(Goal, Expected, Ref) :-
    ( Ref == Expected -> call(Goal) ; true ).

arm_erasure_fault(Engine, Ref) :-
    once(recorded('$cmetta_cursors', Engine, Ref)),
    prolog_listen(erase, on_record(throw(error(cursor_close_probe, _)), Ref)).

finish_erasure_fault(Engine, Ref) :-
    prolog_unlisten(erase, on_record(throw(error(cursor_close_probe, _)), Ref)),
    \+ is_engine(Engine),
    \+ recorded('$cmetta_cursors', _, Ref),
    user:metta_c_close(Ref).

close_live_cursor_twice :-
    once(recorded('$cmetta_cursors', _, Ref)),
    user:metta_c_close(Ref),
    user:metta_c_close(Ref).

count_destroy :-
    flag(cmetta_cursor_destroyed, N, N + 1),
    (   mutex_property('$cmetta_cursors', status(locked(_, _)))
    ->  flag(cmetta_cursor_destroyed_locked, K, K + 1)
    ;   true
    ).

open_counted(Ref) :-
    engine_create(_, setup_call_cleanup(true,
                                       (engine_yield(first), engine_yield(last)),
                                       count_destroy), Engine),
    user:metta_c_new_cursor(Engine, cursor(Id, Ref, Engine)),
    user:metta_c_next(Id, Engine, 0, [first]).

:- begin_tests(cmetta_cursor_lifecycle).

test(close_has_one_winner_and_destroys_after_unlock) :-
    flag(cmetta_cursor_destroyed, _, 0),
    flag(cmetta_cursor_destroyed_locked, _, 0),
    setup_call_cleanup(
        open_counted(Cursor),
        ( length(Goals, 16),
          maplist(=(user:metta_c_close(Cursor)), Goals),
          concurrent(16, Goals, []) ),
        user:metta_c_close(Cursor)),
    flag(cmetta_cursor_destroyed, Destroyed, Destroyed),
    assertion(Destroyed == 1).

% A different losing closer may acquire the mutex during the winner's cleanup.
% Check lock release separately, with no other thread acquiring it.
test(close_destroys_after_unlock) :-
    flag(cmetta_cursor_destroyed_locked, _, 0),
    open_counted(Cursor),
    user:metta_c_close(Cursor),
    flag(cmetta_cursor_destroyed_locked, Locked, Locked),
    assertion(Locked == 0).

test(an_erase_exception_still_destroys_after_unlock) :-
    flag(cmetta_cursor_destroyed, _, 0),
    flag(cmetta_cursor_destroyed_locked, _, 0),
    setup_call_cleanup(
        open_counted(Cursor),
        setup_call_cleanup(
            arm_erasure_fault(Engine, Ref),
            ( catch(user:metta_c_close(Cursor), Error, true),
              assertion(nonvar(Error)),
              assertion(Error = error(cursor_close_probe, _)) ),
            finish_erasure_fault(Engine, Ref)),
        user:metta_c_close(Cursor)),
    flag(cmetta_cursor_destroyed, Destroyed, Destroyed),
    flag(cmetta_cursor_destroyed_locked, Locked, Locked),
    assertion(Destroyed == 1),
    assertion(Locked == 0).

test(an_erase_signal_cannot_interrupt_the_ownership_transfer) :-
    flag(cmetta_cursor_destroyed, _, 0),
    thread_self(Thread),
    Signal = thread_signal(Thread, throw(error(cursor_close_probe, _))),
    setup_call_cleanup(
        open_counted(Ref),
        setup_call_cleanup(
            prolog_listen(erase, on_record(Signal, Ref)),
            ( catch(user:metta_c_close(Ref), Error, true),
              assertion(nonvar(Error)),
              assertion(Error = error(cursor_close_probe, _)) ),
            prolog_unlisten(erase, on_record(Signal, Ref))),
        user:metta_c_close(Ref)),
    flag(cmetta_cursor_destroyed, Destroyed, Destroyed),
    assertion(Destroyed == 1).

:- end_tests(cmetta_cursor_lifecycle).
