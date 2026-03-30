#!/bin/sh
# gen.sh - microser IDL compiler
# Originally from Tiny IDL (public domain / MIT-0)
# Extended with bytes/string type support for lumimux.
# Usage: ./gen.sh input.idl output_basename
# Generates output_basename.h and output_basename.c

set -e

INPUT="$1"
BASE="$2"

if [ -z "$INPUT" ] || [ -z "$BASE" ]; then
    echo "usage: $0 input.idl output_basename" >&2
    exit 1
fi

awk -v base="$BASE" '
function to_snake(s,    r, i, c) {
    r = ""
    for (i = 1; i <= length(s); i++) {
        c = substr(s, i, 1)
        if (c >= "A" && c <= "Z") {
            if (i > 1) r = r "_"
            r = r tolower(c)
        } else {
            r = r c
        }
    }
    return r
}

function c_type(t) {
    if (t ~ /^u?int(8|16|32|64)$/) return t "_t"
    if (t == "bytes" || t == "string") return ""
    return to_snake(t) "_t"
}

function is_bytes(t) {
    return (t == "bytes" || t == "string")
}

function rw_suffix(t) {
    if (t == "uint8")  return "u8"
    if (t == "int8")   return "i8"
    if (t == "uint16") return "u16"
    if (t == "int16")  return "i16"
    if (t == "uint32") return "u32"
    if (t == "int32")  return "i32"
    if (t == "uint64") return "u64"
    if (t == "int64")  return "i64"
    return "u8"
}

# strip comments and whitespace
{
    sub(/#.*/, "")
    gsub(/^[ \t]+|[ \t]+$/, "")
    if ($0 == "") next
}

# --- enum ---
$1 == "enum" {
    ne++
    en[ne] = $2
    enc[ne] = 0
    state = "enum"
    next
}
state == "enum" && $1 == "end" { state = ""; next }
state == "enum" {
    split($0, p, /[ \t]*=[ \t]*/)
    enc[ne]++
    j = enc[ne]
    ee_name[ne, j] = p[1]
    ee_val[ne, j] = p[2] + 0
    enum_lookup[p[1]] = p[2] + 0
    next
}

# --- message ---
$1 == "message" {
    nm++
    mn[nm] = $2
    mrf[nm] = 0
    maf[nm] = 0
    mcase[nm] = 0
    state = "msg"
    next
}
state == "msg" && $1 == "end" { state = ""; next }

# case block start
state == "msg" && $1 == "case" {
    mcase[nm] = 1
    mdt[nm] = $2; mdn[nm] = $3; mdtag[nm] = $5 + 0
    mnv[nm] = 0
    # add discriminant to all-fields list
    maf[nm]++; j = maf[nm]
    af_type[nm, j] = $2; af_name[nm, j] = $3; af_tag[nm, j] = $5 + 0
    state = "case"
    next
}

# regular field
state == "msg" {
    mrf[nm]++; j = mrf[nm]
    rf_type[nm, j] = $1; rf_name[nm, j] = $2; rf_tag[nm, j] = $4 + 0
    maf[nm]++; k = maf[nm]
    af_type[nm, k] = $1; af_name[nm, k] = $2; af_tag[nm, k] = $4 + 0
    next
}

# --- case variants ---
state == "case" && $1 == "end" { state = "msg"; next }

# variant label
(state == "case" || state == "var") && $0 ~ /:$/ {
    label = $1; sub(/:$/, "", label)
    mnv[nm]++; vi = mnv[nm]
    if (label in enum_lookup)
        vv[nm, vi] = enum_lookup[label]
    else
        vv[nm, vi] = label + 0
    vlabel[nm, vi] = label
    vnf[nm, vi] = 0
    state = "var"
    next
}

state == "var" && $1 == "end" { state = "msg"; next }

# variant field
state == "var" {
    vnf[nm, vi]++; vfi = vnf[nm, vi]
    vf_type[nm, vi, vfi] = $1
    vf_name[nm, vi, vfi] = $2
    vf_tag[nm, vi, vfi] = $4 + 0
    tag = $4 + 0
    key = nm SUBSEP tag
    if (!(key in tag_seen)) {
        tag_seen[key] = 1
        maf[nm]++; k = maf[nm]
        af_type[nm, k] = $1; af_name[nm, k] = $2; af_tag[nm, k] = tag
    }
    next
}

# --- code generation ---
END {
    h = base ".h"
    c = base ".c"
    guard = toupper(base) "_H"
    gsub(/[^A-Z0-9_]/, "_", guard)

    # ---- header ----
    printf "/* %s : generated from %s -- do not edit */\n", h, FILENAME > h
    printf "/* Copyright (c) 2026 Jon Mayo\n" > h
    printf " * Licensed under MIT-0 OR PUBLIC DOMAIN */\n\n" > h
    printf "#ifndef %s\n#define %s\n\n", guard, guard > h
    printf "#include <stdint.h>\n\n" > h

    for (i = 1; i <= ne; i++) {
        sn = to_snake(en[i]); un = toupper(sn)
        printf "typedef uint8_t %s_t;\n", sn > h
        for (j = 1; j <= enc[i]; j++) {
            ename = toupper(to_snake(ee_name[i, j]))
            printf "#define %s_%s\t%d\n", un, ename, ee_val[i, j] > h
        }
        printf "\n" > h
    }

    for (i = 1; i <= nm; i++) {
        sn = to_snake(mn[i])
        printf "struct %s {\n", sn > h
        for (j = 1; j <= maf[i]; j++) {
            if (af_type[i, j] == "bytes") {
                printf "\tconst uint8_t\t*%s;\n", af_name[i, j] > h
                printf "\tuint16_t\t %s_len;\n", af_name[i, j] > h
            } else if (af_type[i, j] == "string") {
                printf "\tconst char\t*%s;\n", af_name[i, j] > h
                printf "\tuint16_t\t %s_len;\n", af_name[i, j] > h
            } else {
                printf "\t%s\t%s;\n", c_type(af_type[i, j]), af_name[i, j] > h
            }
        }
        printf "};\n\n" > h
        printf "int %s_encode(const struct %s *msg, uint8_t *buf, int len);\n", sn, sn > h
        printf "int %s_decode(struct %s *msg, const uint8_t *buf, int len);\n\n", sn, sn > h
    }

    printf "#endif /* %s */\n", guard > h
    close(h)

    # ---- source ----
    printf "/* %s : generated from %s -- do not edit */\n", c, FILENAME > c
    printf "/* Copyright (c) 2026 Jon Mayo\n" > c
    printf " * Licensed under MIT-0 OR PUBLIC DOMAIN */\n\n" > c
    printf "#include \"%s\"\n", base ".h" > c
    printf "#include \"microser.h\"\n\n" > c
    printf "#include <string.h>\n\n" > c

    for (i = 1; i <= nm; i++) {
        sn = to_snake(mn[i])

        # encode
        printf "int\n%s_encode(const struct %s *msg, uint8_t *buf, int len)\n", sn, sn > c
        printf "{\n\tint pos = 2;\n\n" > c

        for (j = 1; j <= mrf[i]; j++) {
            if (is_bytes(rf_type[i, j])) {
                if (rf_type[i, j] == "string") {
                    printf "\tpos = ms_write_tag_bytes(buf, pos, len, %d,\n", rf_tag[i, j] > c
                    printf "\t    (const void *)msg->%s, msg->%s_len);\n", rf_name[i, j], rf_name[i, j] > c
                } else {
                    printf "\tpos = ms_write_tag_bytes(buf, pos, len, %d,\n", rf_tag[i, j] > c
                    printf "\t    msg->%s, msg->%s_len);\n", rf_name[i, j], rf_name[i, j] > c
                }
            } else {
                printf "\tpos = ms_write_tag_%s(buf, pos, len, %d, msg->%s);\n", \
                    rw_suffix(rf_type[i, j]), rf_tag[i, j], rf_name[i, j] > c
            }
            printf "\tif (pos < 0)\n\t\treturn -1;\n" > c
        }

        if (mcase[i]) {
            printf "\tpos = ms_write_tag_%s(buf, pos, len, %d, msg->%s);\n", \
                rw_suffix(mdt[i]), mdtag[i], mdn[i] > c
            printf "\tif (pos < 0)\n\t\treturn -1;\n" > c
            printf "\tswitch (msg->%s) {\n", mdn[i] > c
            for (vi = 1; vi <= mnv[i]; vi++) {
                printf "\tcase %d: /* %s */\n", vv[i, vi], vlabel[i, vi] > c
                for (vfi = 1; vfi <= vnf[i, vi]; vfi++) {
                    if (is_bytes(vf_type[i, vi, vfi])) {
                        if (vf_type[i, vi, vfi] == "string") {
                            printf "\t\tpos = ms_write_tag_bytes(buf, pos, len, %d,\n", vf_tag[i, vi, vfi] > c
                            printf "\t\t    (const void *)msg->%s, msg->%s_len);\n", vf_name[i, vi, vfi], vf_name[i, vi, vfi] > c
                        } else {
                            printf "\t\tpos = ms_write_tag_bytes(buf, pos, len, %d,\n", vf_tag[i, vi, vfi] > c
                            printf "\t\t    msg->%s, msg->%s_len);\n", vf_name[i, vi, vfi], vf_name[i, vi, vfi] > c
                        }
                    } else {
                        printf "\t\tpos = ms_write_tag_%s(buf, pos, len, %d, msg->%s);\n", \
                            rw_suffix(vf_type[i, vi, vfi]), vf_tag[i, vi, vfi], vf_name[i, vi, vfi] > c
                    }
                    printf "\t\tif (pos < 0)\n\t\t\treturn -1;\n" > c
                }
                printf "\t\tbreak;\n" > c
            }
            printf "\t}\n" > c
        }

        printf "\n\tbuf[0] = (uint8_t)((pos - 2) & 0xff);\n" > c
        printf "\tbuf[1] = (uint8_t)(((pos - 2) >> 8) & 0xff);\n" > c
        printf "\treturn pos;\n}\n\n" > c

        # decode
        printf "int\n%s_decode(struct %s *msg, const uint8_t *buf, int len)\n", sn, sn > c
        printf "{\n\tint end, pos = 2;\n\n" > c
        printf "\tif (len < 2)\n\t\treturn -1;\n" > c
        printf "\tend = (int)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8)) + 2;\n" > c
        printf "\tif (end > len)\n\t\treturn -1;\n" > c
        printf "\tmemset(msg, 0, sizeof(*msg));\n\n" > c
        printf "\twhile (pos < end) {\n" > c
        printf "\t\tuint8_t tag = buf[pos++];\n\n" > c
        printf "\t\tswitch (tag >> 3) {\n" > c

        for (j = 1; j <= maf[i]; j++) {
            printf "\t\tcase %d:\n", af_tag[i, j] > c
            if (is_bytes(af_type[i, j])) {
                if (af_type[i, j] == "string") {
                    printf "\t\t\t{\n" > c
                    printf "\t\t\t\tconst uint8_t *_tmp;\n\n" > c
                    printf "\t\t\t\tpos = ms_read_bytes(buf, pos, end,\n" > c
                    printf "\t\t\t\t    &_tmp, 65535, &msg->%s_len);\n", af_name[i, j] > c
                    printf "\t\t\t\tmsg->%s = (const char *)_tmp;\n", af_name[i, j] > c
                    printf "\t\t\t}\n" > c
                } else {
                    printf "\t\t\tpos = ms_read_bytes(buf, pos, end,\n" > c
                    printf "\t\t\t    &msg->%s, 65535, &msg->%s_len);\n", af_name[i, j], af_name[i, j] > c
                }
            } else {
                printf "\t\t\tpos = ms_read_%s(buf, pos, end, &msg->%s);\n", \
                    rw_suffix(af_type[i, j]), af_name[i, j] > c
            }
            printf "\t\t\tbreak;\n" > c
        }

        printf "\t\tdefault:\n" > c
        printf "\t\t\tpos = ms_skip(buf, pos, end, tag & 7);\n" > c
        printf "\t\t\tbreak;\n" > c
        printf "\t\t}\n" > c
        printf "\t\tif (pos < 0)\n\t\t\treturn -1;\n" > c
        printf "\t}\n" > c
        printf "\treturn end;\n}\n\n" > c
    }

    close(c)
}
' "$INPUT"
