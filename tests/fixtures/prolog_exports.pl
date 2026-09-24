% Purpose: a Prolog source that declares its own export, registered with no
%   names, for tests/test_cmetta.c, test_prolog_registers_as_metta_functions.
:- metta_export("(: cmetta-prolog-exported (-> Number Number))").
'cmetta-prolog-exported'(X, Y) :- Y is X * 7.
