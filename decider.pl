% Purpose: tell the engine where the C host lives and when it is usable.
%   This file is the whole of what the engine knows about C, and it is not in
%   the engine.
% Assumes:
%   - a C host registers '$cetta_present'/0 as a foreign predicate BEFORE it
%     consults engine/metta.pl, so this glob can see it. That is the C seat's
%     equivalent of the Python seat asking exists_source(library(janus)): the
%     substrate is not a library on disk, it is whether this process is the
%     C host at all [source: bindings/cetta/cetta.c, cetta_open].
% Guarantees:
%   - a tree loaded by swipl, by the Python host, or by any process that is
%     not the C host loads this file and nothing happens
%   - a process that IS the C host loads the bridge.pl beside this file, which
%     raises if the bridge is broken. The split every host wants: not present
%     is not an error, half present is.
% Open Obligations:
%   To Do: None
%   Hacks: None
%   Future Enhancements: None

:- prolog_load_context(directory, Dir),
   (   current_predicate('$cetta_present'/0)
   ->  directory_file_path(Dir, 'bridge.pl', Bridge),
       ensure_loaded(Bridge)
   ;   true
   ).
