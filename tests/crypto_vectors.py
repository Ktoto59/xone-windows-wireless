# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent fixture generation with Python integers/hashlib; no external packages."""
import hashlib
import hmac
p = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
x = int('6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296', 16)
y = int('4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5', 16)
s = ((3*x*x+p-3)*pow(2*y, -1, p)) % p
xx = (s*s-2*x) % p
yy = (s*(x-xx)-y) % p
print('2G', f'{xx:064x}{yy:064x}')
print('ECDH_SHA256', hashlib.sha256(xx.to_bytes(32, 'big')).hexdigest())
key = bytes(range(48))
seed = b'Master Secret' + bytes(range(64))
a = hmac.digest(key, seed, 'sha256')
out = b''
while len(out) < 48:
    out += hmac.digest(key, a+seed, 'sha256')
    a = hmac.digest(key, a, 'sha256')
print('PRF', out[:48].hex())
