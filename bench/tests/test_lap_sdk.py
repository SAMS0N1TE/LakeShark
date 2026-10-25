"""Native SDK, C/Python manifest cross-checks and real test-generated P-256 signatures."""
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('lap',ROOT/'app-sdk/lap.py')
lap=importlib.util.module_from_spec(spec);spec.loader.exec_module(lap)
INSPECT=Path(sys.argv.pop(1)).resolve()
BUILD=Path(sys.argv.pop(1)).resolve()
VERIFY=Path(sys.argv.pop(1)).resolve() if len(sys.argv)>1 else None

class NativeSDK(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.file=lap.build(ROOT/'bench/fixtures/lap',BUILD/'sdk-probe.lap')
        cls.data=cls.file.read_bytes()

    def test_exports(self):
        text=(ROOT/'components/apps/tui/ls_lap_exports.inc').read_text()
        table=re.findall(r'EXPORT\("([^"]+)",\s*(\w+)\)',text)
        names=[n for n,f in table]
        self.assertEqual(names,sorted(set(names)))
        self.assertEqual(names,(ROOT/'app-sdk/exports.txt').read_text().splitlines())
        h=(ROOT/'app-sdk/include/lakeshark_app.h').read_text()
        clean=re.sub(r'/\*.*?\*/','',h,flags=re.S)
        api=set(re.findall(r'\b(lap_\w+|malloc|calloc|realloc|free)\s*\([^;{}]*\)\s*;',clean))
        api.discard('lap_main')
        self.assertTrue(api<=set(names),api-set(names))
        for symbol in 'memcpy memset memmove memcmp strlen strnlen strcmp strncmp strcpy strncpy strcat strchr strrchr strstr snprintf vsnprintf atoi strtol strtoul strtof abs qsort sinf cosf tanf atan2f atanf sqrtf fabsf floorf ceilf roundf powf expf logf fmodf'.split():
            self.assertIn(symbol,names)
        for symbol in ('malloc','calloc','realloc','free'):
            self.assertEqual(dict(table)[symbol],'ls_lap_'+symbol)

    def test_cross_check(self):
        e=lap.Elf(self.data);self.assertEqual(e.manifest['id'],'probe')
        r=subprocess.run([str(INSPECT),str(self.file)],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertEqual(r.stdout.splitlines(),['probe','PROBE','1.0'])
        # Header and section-table mutations must agree between implementations.
        for pos in [*range(52),*range(int.from_bytes(self.data[32:36],'little'),len(self.data))]:
            b=bytearray(self.data);b[pos]^=0xff
            f=BUILD/'mutant.lap';f.write_bytes(b)
            try:
                candidate=lap.Elf(b);candidate.check(set((ROOT/'app-sdk/exports.txt').read_text().splitlines()));py_ok=True
            except (ValueError,UnicodeError,IndexError):py_ok=False
            c=subprocess.run([str(INSPECT),str(f)],capture_output=True)
            self.assertEqual(py_ok,c.returncode==0,f'offset {pos}: {c.stdout!r}')

    def test_relocations_and_compiler_helpers(self):
        e=lap.Elf(self.data);exports=set((ROOT/'app-sdk/exports.txt').read_text().splitlines());e.check(exports)
        imports=set()
        for s in e.sections:
            if s[1]==11:
                for o in range(s[4],s[4]+s[5],16):
                    ni,val,size,info,other,ndx=lap.struct.unpack_from('<IIIBBH',e.data,o)
                    if not ndx and ni:imports.add(e.string(e.sections[s[6]],ni))
        for name in ('__wrap___divdf3','__wrap___udivdi3','snprintf','malloc','sinf'):
            self.assertIn(name,imports)
        with self.assertRaisesRegex(ValueError,'needs'):
            e.check(set())

    def test_signing(self):
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives import hashes,serialization
        if not VERIFY:self.skipTest('configure LAP_MBEDTLS_DIR to cross-check firmware verifier')
        # Generate test keys at runtime; only temporary public keys/files enter the build dir.
        key=ec.generate_private_key(ec.SECP256R1())
        pub=BUILD/'test-public.txt';pub.write_bytes(key.public_key().public_bytes(serialization.Encoding.PEM,serialization.PublicFormat.SubjectPublicKeyInfo))
        signed=BUILD/'signed-probe.lap';signed.write_bytes(self.data)
        with tempfile.TemporaryDirectory(prefix='ls-lap-key-') as tmp:
            private=Path(tmp)/'test.pem';private.write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
            lap.sign(signed,private)
            self.assertEqual(subprocess.run([str(VERIFY),str(signed),str(pub)],capture_output=True).returncode,0)
            lap.sign(signed,private) # re-sign replaces the previous trailer
            e=lap.Elf(signed.read_bytes());self.assertEqual(e.data,self.data);key.public_key().verify(e.sig,e.data,ec.ECDSA(hashes.SHA256()))
        raw=signed.read_bytes()
        for pos in (100,len(raw)-8,len(raw)-6,len(raw)-1):
            mutant=bytearray(raw);mutant[pos]^=1;bad=BUILD/'bad-signature.lap';bad.write_bytes(mutant)
            self.assertNotEqual(subprocess.run([str(VERIFY),str(bad),str(pub)],capture_output=True).returncode,0)
        for size in range(len(self.data)+1,len(raw)):
            bad=BUILD/'bad-signature.lap';bad.write_bytes(raw[:size])
            self.assertNotEqual(subprocess.run([str(VERIFY),str(bad),str(pub)],capture_output=True).returncode,0)
        wrong=ec.generate_private_key(ec.SECP256R1());pub.write_bytes(wrong.public_key().public_bytes(serialization.Encoding.PEM,serialization.PublicFormat.SubjectPublicKeyInfo))
        self.assertNotEqual(subprocess.run([str(VERIFY),str(signed),str(pub)],capture_output=True).returncode,0)

    def test_private_key_policy(self):
        with self.assertRaisesRegex(ValueError,'outside'):
            lap.keygen(ROOT/'bench/build-lap/forbidden.pem')
        with tempfile.TemporaryDirectory(prefix='ls-lap-keygen-') as tmp:
            p=Path(tmp)/'key.pem';lap.keygen(p);self.assertIn(b'PRIVATE KEY',p.read_bytes())
            with self.assertRaises(FileExistsError):lap.keygen(p)

if __name__=='__main__':unittest.main()
