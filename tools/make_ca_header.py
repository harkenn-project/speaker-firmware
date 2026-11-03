"""Regenerate src/ca_certs.h from PEM files: the roots the speaker trusts for https.

    python tools/make_ca_header.py isrg-x1.pem isrg-x2.pem gts-r1.pem gts-r3.pem gts-r4.pem sslcom-rsa.pem         sslcom-ecc.pem > src/ca_certs.h

Let's Encrypt (ISRG Root X1, X2) for servers with its certificates, and the CAs Cloudflare issues its edge
certificates from (Google Trust Services GTS Root R1, R3, R4; SSL.com RSA and ECC), for the relay behind Cloudflare.
The PEMs come from the CAs' sites, or from Python's certifi bundle.
"""
import sys

BACKSLASH_N = chr(92) + "n"

lines = []
for path in sys.argv[1:]:
    with open(path, encoding="ascii") as f:
        lines += [line.strip() for line in f if line.strip()]

out = [
    "// Root certificates the speaker trusts for https: Let's Encrypt (ISRG Root X1, X2), and for the relay behind",
    "// Cloudflare, Google Trust Services (GTS Root R1, R3, R4) and SSL.com (RSA, ECC).",
    "// Regenerate with tools/make_ca_header.py.",
    "#pragma once",
    "",
    "static const char CA_PEM[] =",
]
out += ['    "' + line + BACKSLASH_N + '"' for line in lines]
out[-1] += ";"
sys.stdout.write("\n".join(out) + "\n")
