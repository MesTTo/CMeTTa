; Purpose: give the C seat a real file whose equation proves mt_load reaches
;   the engine's reload-aware file door.
; Guarantees: (cmetta-loaded-value) reduces to 73 after this file is loaded
;   [tested: test_catalog_and_file_load_are_live_runtime_doors; commit=b339084bb5625996fc88a31608d48ad31c575d1f].
; Guarded by: no mutable state; the fixture is read-only test input.

(= (cmetta-loaded-value) 73)
