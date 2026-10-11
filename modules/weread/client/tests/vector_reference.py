"""Independent protocol vector generator; no network or file I/O."""
import base64
import hashlib
import json


def content_id(value):
    digest = hashlib.md5(value.encode()).hexdigest()
    if value.isascii() and value.isdecimal():
        chunks = [format(int(value[i:i+9]), "x") for i in range(0, len(value), 9)]
        tail = "g".join(f"{len(chunk):02x}{chunk}" for chunk in chunks)
        result = digest[:3] + "32" + digest[-2:] + tail
    else:
        raw = value.encode().hex()
        result = digest[:3] + "42" + digest[-2:] + f"{len(raw):02x}" + raw
    if len(result) < 20:
        result += digest[:20-len(result)]
    return result + hashlib.md5(result.encode()).hexdigest()[:3]


def signature(query):
    a = b = 0x15051505
    for i in range(len(query), 1, -2):
        a = (a ^ (ord(query[i-1]) << ((len(query)-i+1) % 30))) & 0x7fffffff
        b = (b ^ (ord(query[i-2]) << ((i-1) % 30))) & 0x7fffffff
    return (a+b) & 0xffffffff


def encode_content(text):
    encoded = bytearray(base64.b64encode(text.encode()))
    n = len(encoded)
    if n < 4:
        positions = []
    elif n < 11:
        positions = [0, 2]
    else:
        tail = min((n+9)//10, 4)
        digits = "".join(str(int(f"{v:08b}", 4)) for v in encoded[-tail:][::-1])
        m = n-tail-2
        step = len(str(m))
        positions = []
        for i in range(0, len(digits)-step, step):
            positions += [int(digits[i:i+step]) % m, int(digits[i+1:i+1+step]) % m]
            if len(positions) >= 10:
                break
    for a, b in zip(positions[::2], positions[1::2]):
        for offset in (0, 1):
            encoded[a+offset], encoded[b+offset] = encoded[b+offset], encoded[a+offset]
    return "0" + encoded.decode()


for value in ("1", "1234567890123", "book-alpha", "0"):
    print("ID", value, content_id(value))
query = "b=test&c=1&ct=1700000000&pc=p&prevChapter=false&ps=s&r=49&sc=0&st=0"
print("SIGN", signature(query), hex(signature(query)))
for text in ("hello", "<html><body><p>Test &amp; text.</p></body></html>", "line one\nline two\n", "\u4e2d\u6587\U0001f600"):
    wire = encode_content(text)
    parts = [wire[:len(wire)//3], wire[len(wire)//3:2*len(wire)//3], wire[2*len(wire)//3:]]
    shards = [hashlib.md5(p.encode()).hexdigest()+p for p in parts]
    print(json.dumps({"text": text, "wire": wire, "shards": shards}, ensure_ascii=True))
