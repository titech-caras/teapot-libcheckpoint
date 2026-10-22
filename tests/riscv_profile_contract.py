"""Check PURE-only facts and ABI fingerprint, without running target code."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--manifest", type=Path, required=True)
parser.add_argument("--profile", choices=("default", "lp64-pure"), required=True)
args = parser.parse_args()
contract = json.loads(args.manifest.read_text())
abi = contract["abi"]
facts = {"riscv.calling_abi": "lp64", "riscv.profile": "lp64-pure",
         "riscv.fp_policy": "fp-free-fail-closed"}
if args.profile == "lp64-pure":
    assert abi["isa"] == "riscv64"
    assert all(type(abi.get(key)) is type(value) and abi[key] == value for key, value in facts.items())
    assert contract["capabilities"]["riscv64_float_state"] is False
else:
    assert not any(key in abi for key in facts), "Legacy contract must not acquire PURE fields"
lines = "".join(f"{key}={abi[key]}\n" for key in sorted(abi))
assert hashlib.sha256(lines.encode()).hexdigest()[:16] == contract["fingerprint"]
print("Runtime profile contract accepted:", args.profile, contract["fingerprint"])
