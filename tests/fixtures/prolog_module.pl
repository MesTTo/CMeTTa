% Purpose: a Prolog module whose export registers under a new name, for
%   tests/test_cmetta.c, test_prolog_registers_as_metta_functions.
:- module(cmetta_prolog_module, [cmetta_prolog_export/2]).
cmetta_prolog_export(X, Y) :- Y is X * 7.
