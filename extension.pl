% This seat's control file; see extensions/python/extension.pl for the model.
%
% A C host registers '$cmetta_present'/0 as a foreign predicate BEFORE it
% consults engine/metta.pl, so the loader can see it. That is the C seat's
% equivalent of the Python seat needing library(janus): the substrate is not a
% library on disk, it is whether this process is the C host at all
% [source: extensions/cmetta/cmetta.c, cmetta_open]. A process that is not the C
% host -- plain swipl, the Python host -- loads nothing and says nothing.

title('MeTTa in C: the engine embedded through libcmetta').
needs(predicate('$cmetta_present'/0)).

% One file plays both roles here: the engine consults it when the marker is
% present, and it is also the transport the C host's calls arrive through.
entry(engine, 'bridge.pl').
entry(host, 'bridge.pl').
