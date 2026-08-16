#!/bin/sh
# gen_corpus.sh — generate the conformance-sequence corpus files.
# Run from tools/. Writes into tools/corpus/.

OUT=$(cd "$(dirname "$0")" && pwd)/corpus

# gen <name> <cols> <rows> <bytes>  — bytes uses printf %b escapes
gen() {
    name=$1; cols=$2; rows=$3; bytes=$4
    {
        printf '#gc:cols=%s rows=%s\n' "$cols" "$rows"
        printf '%b' "$bytes"
    } > "$OUT/$name.seq"
    echo "wrote $name.seq"
}

# ---- basic printing ----
gen basic 80 24 "Hello world\n"

# ---- SGR colors/attributes ----
gen sgr 80 24 "red: \x1b[31mRED\x1b[0m, bold: \x1b[1mBOLD\x1b[0m\nbg: \x1b[41mBG\x1b[0m\n256: \x1b[38;5;196mX\x1b[0m\ntrue: \x1b[38;2;10;20;30mY\x1b[0m\n"

# ---- cursor movement ----
gen cursor_move 80 24 "\x1b[5;10HX\x1b[2AY\x1b[2BZ\x1b[3CW\x1b[3DV\x1b[12H\x1b[2\`Q\x1b[3dR\x1b[4aS\x1b[2eT"

# ---- ED / EL ----
gen erase_line 80 24 "\x1b[1;5Habcdefghijklmnop\x1b[0K!\x1b[1K\x1b[2K\x1b[3;1HHELLO\x1b[1X\n"
gen erase_display 80 24 "\x1b[5;10HX\x1b[2JZ\nrest\n\x1b[0JEND"

# ---- scroll regions ----
gen scroll_region 80 24 "\x1b[5;10rA\nB\nC\nD\nE\nF\nG\nH\nI\nJ\nK\nL\n\x1b[3;1HEND"
gen scroll_region_move 80 24 "\x1b[3;10r\x1b[5;1H\x1b[2SIN\x1b[2T"

# ---- insert/delete lines ----
gen insert_delete_lines 80 24 "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8\nL9\nL10\n\x1b[2;1H\x1b[2M\x1b[4;1H\x1b[3L\x1b[4;1H\x1b[3M"

# ---- insert/delete chars ----
gen insert_delete_chars 80 24 "ABCDEFGHIJ\n\x1b[1;3H\x1b[3@\x1b[1;6H\x1b[2P\x1b[1;4H\x1b[3X"

# ---- tab stops ----
gen tabs 80 24 "A\tB\tC\n\x1b[0gA\tB\n\x1b[5;1H\x1b[2I\x1b[2Z"

# ---- REP ----
gen rep 80 24 "XYZ\x1b[3D\x1b[5b\n"

# ---- wide + combining ----
gen wide 80 24 "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e WIDE\x1b[1G\n"
gen combining 80 24 "a\xcc\x80e\xcc\x81i\xcc\x88o\xcc\x82\n"

# ---- DECSC / DECRC ----
gen decsc 80 24 "\x1b[5;10H\x1b7WRITTEN\x1b8END"

# ---- DECOM origin mode ----
gen origin_mode 80 24 "\x1b[?6h\x1b[5;10HORIGIN\x1b[?6l\x1b[5;10HPLAIN"

# ---- alternate screen ----
gen alt_screen 80 24 "\x1b[?1049hALT MODE\nsecond line\x1b[?1049lMAIN"

# ---- DECSCUSR ----
gen cursor_style 80 24 "\x1b[?0c\x1b[2 q\x1b[4 q\x1b[6 q"

# ---- scrollback overflow ----
gen scrollback 80 24 "LINE1\nLINE2\nLINE3\nLINE4\nLINE5\nLINE6\nLINE7\nLINE8\nLINE9\nLINE10\nLINE11\nLINE12\nLINE13\nLINE14\nLINE15\nLINE16\nLINE17\nLINE18\nLINE19\nLINE20\nLINE21\nLINE22\nLINE23\nLINE24\nLINE25\nLINE26\nLINE27\nLINE28\nLINE29\nLINE30\n"

# ---- synchronized output ----
gen sync_out 80 24 "\x1b[?2026h\x1b[2J\x1b[1;1Hhello\x1b[?2026l"

# ---- bracketed paste ----
gen bracketed_paste 80 24 "\x1b[?2004h\x1b[200~pasted\x1b[201~\x1b[?2004l"

# ---- insert mode ----
gen insert_mode 80 24 "ABCDE\x1b[1;2H\x1b[4hXYZ\x1b[4l\x1b[2D"

# ---- protected cells ----
gen protected 80 24 "12345\x1b[1;2H\x1b[2\"q6789\x1b[?J\x1b[?0q"

# ---- XTSAVE / XTRESTORE ----
gen xtsave 80 24 "\x1b[?6h\x1b[?1h\x1b[?7l\x1b[?s\x1b[?6l\x1b[?7h\x1b[?r"

# ---- soft wrap ----
gen soft_wrap 80 24 "01234567890123456789012345678901234567890123456789012345678901234567890123456789012345\n"

# ---- OSC title (no-op, but should not crash) ----
gen osc_title 80 24 "\x1b]0;hello world\x07title? \x1b[1;1H"

# ---- CSI param robustness ----
gen csi_params 80 24 "\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25;26;27;28;29;30HX"
gen csi_empty 80 24 "\x1b[H\x1b[;H\x1b[1;;2H\x1b[3;;4rZ"

# ---- ambiguous/intermediate finals ----
gen intermediates 80 24 "\x1b[>c\x1b[=c\x1b[c\x1b[?1;2c\x1b[4\$p\x1b[?69\$p\x1b[!p\x1b[\"p\x1b[?s\x1b[?r"

# ---- full reset ----
gen reset 80 24 "\x1b[31mRED\x1b[?6h\x1b[1;1H\x1b[5;10H\x1b[2J\x1b[!p\x1b[c"

# ---- long wrapped paragraph ----
gen paragraph 80 24 "The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog.\nEnd."

# ---- unicode box drawing ----
gen box 80 24 "┌─┐\n│x│\n└─┘\n"

# ---- emoji (wide) ----
gen emoji 80 24 "🚀 launch\n"

# ---- reverse index ----
gen reverse_index 80 24 "A\nB\nC\nD\nE\n\x1b[3M\x1b[3;1H\x1b[1MX"

# ---- DECOM homes cursor (set and reset) ----
gen dec_origin_home 80 24 "\x1b[5;10H\x1b[?6h\x1b[5;10H\x1b[?6l"

# ---- DECSCA 2 is OFF in Ghostty (not ISO) ----
gen decsca2_off 80 24 "\x1b[1\"qABC\x1b[2\"qXY\x1b[?0J"

# ---- param-count strictness ----
gen param_strict 80 24 "A\x1b[1;2;3H\x1b[1;2;3A\x1b[2;3B\x1b[3J\x1b[1;2;3;4H\x1b[30H"

# ---- EL-right resets soft-wrap on a wrapped row ----
gen el_right_wrap 80 24 "A"$(printf 'B%.0s' $(seq 1 79))"\x1b[0KX"

# ---- ED clears wrap flags ----
gen ed_clears_wrap 80 24 "A"$(printf 'B%.0s' $(seq 1 79))"\x1b[2JY"

# ---- soft-wrap then scroll (wrap row leaves view) ----
gen scroll_wrap_off 80 24 "A"$(printf 'B%.0s' $(seq 1 79))"\nC"

# ---- multi-mode DECSET ----
gen multimode_decset 80 24 "\x1b[?6;7h\x1b[?6;7l"

# ---- emoji variety ----
gen emoji_wide 80 24 "🚀🎉😀🔧⬛🈁\n"

echo "done"
