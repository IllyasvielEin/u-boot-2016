# RC01 write-safety integration

The three write-safety fixes were rebased onto Viking's public
`be10000-four-port` revision `54a2eb299d190abfabca58fe061bd7dcaa2e6c19`.
The original development import had identical source content except that
it did not track the upstream GitHub Actions workflow. This integration
retains that workflow without changing it.

| Module | Rebased commit | Behavior |
| --- | --- | --- |
| UB01 | `e2b58fad47cf4e88a80a85a9d952b39c74bdbbe6` | Check good-block capacity before erasing, enforce partition bounds in NAND I/O, propagate erase and transfer errors, validate wide partition arithmetic. |
| UB02 | `fe4d491dafe7ad01ff3e0196496539cb071f59e1` | Preserve command and web upgrade failures, choose existing physical raw UBI targets, reject RC01 tar backup before UBI operations, check selected UBI operations and final volume IDs. |
| UB03 | `1467b770e76f9d61f042c1ebaf00eb2ed18042e9` | Bound ENV saves and loads, independently read back saves, report web persistence failures, and print failed ENV deletion diagnostics. |

Each module passed independent Astra source and host acceptance before
integration. UB01's partition multiplication issue and UB03's physical
range rounding and silent deletion issues were corrected and rechecked.
The original development handoffs in this directory retain their historical
local commit IDs and review descriptions. They are development records,
not flashing instructions.

After the rebase, only test baseline references and this documentation were
adapted for the public history; production source remains identical to the
accepted implementation. Test counterexamples now refer to revisions
reachable from this public branch instead of the unrelated import history.

Run the host checks from the repository root with C compiler, Node.js,
Python, and a full Git history available:

```sh
uv run --offline --no-project python -B test/rc01/test_nand_write_safety.py
uv run --offline --no-project python -B test/rc01/test_upgrade_results.py
uv run --offline --no-project python -B test/rc01/test_env_save.py
```

Expected fixed-tree coverage is UB01 29/30 scenarios, UB02 24/28 scenarios,
and UB03 35/35 ENV scenarios, two redundant cases, three compile-only
configurations, 14 HTTP cases, 37 page cases, and 10 real hush cases.
Original-defect and mutation comparisons must be reported separately;
a missing baseline is not a successful negative control.

The existing Actions workflow does not yet run these host checks. Its
single-platform dispatch, release permissions, packaging failure handling,
and build/publish separation are under review; this integration does not
claim those CI issues are fixed.

Host fixtures use the real C functions and JavaScript with simulated
storage, network, DOM, and other device backends. Prior target validation
reached an ARM32 ELF and embedded web resources, without MBN packaging or
device acceptance. Single-copy ENV remains vulnerable to power loss;
multi-target updates have no transaction or rollback. Device boot,
QPIC/ECC, actual bad-block timing, Secure Boot compatibility, and recovery
remain unverified. No device operations are part of this integration.
