; Purpose: test both Unicode filenames and source through mt_load.
; Guarantees: the single answer preserves all UTF-8 bytes
; [tested: tests/test_native_parity.c; commit=6e91a33be09722c403ae665dd7affd608a067437].
!(quote "λ 😀")
