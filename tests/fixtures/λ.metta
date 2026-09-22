; Purpose: test both Unicode filenames and source through mt_load.
; Guarantees: the single answer preserves all UTF-8 bytes
; [tested: tests/test_native_parity.c; commit=1a60e2a3cce69d5d6bda67100186939d707f4397].
!(quote "λ 😀")
