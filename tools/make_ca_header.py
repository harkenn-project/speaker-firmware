"""Regenerate src/ca_certs.h from PEM files.

    curl -o x1.pem https://letsencrypt.org/certs/isrgrootx1.pem
    curl -o x2.pem https://letsencrypt.org/certs/isrg-root-x2.pem
    python tools/make_ca_header.py x1.pem x2.pem > src/ca_certs.h
"""
import sys

BACKSLASH_N = chr(92) + "n"

lines = []
for path in sys.argv[1:]:
    with open(path, encoding="ascii") as f:
        lines += [line.strip() for line in f if line.strip()]

out = [
    "// Let's Encrypt root certificates (ISRG Root X1 RSA, ISRG Root X2 ECDSA), from https://letsencrypt.org/certificates/",
    "// kotodama.mylife.ninja is served with a Let's Encrypt certificate by the cluster gateway.",
    "// Regenerate with tools/make_ca_header.py.",
    "#pragma once",
    "",
    "static const char CA_PEM[] =",
]
out += ['    "' + line + BACKSLASH_N + '"' for line in lines]
out[-1] += ";"
sys.stdout.write("\n".join(out) + "\n")
