; Purpose: test both Unicode filenames and source through mt_load.
; Guarantees: the single answer preserves all UTF-8 bytes
; [tested: tests/test_native_parity.c; commit=WORKTREE].
!(quote "λ 😀")
