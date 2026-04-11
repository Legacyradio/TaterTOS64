# WiFi Boot Checkpoint

Reference:
- Also follow the `RULES (must follow exactly)` section in `planv3.txt`.

After boot reaches the shell on bare metal, run:

```bash
ws
```

What to look for:

- The old failure was:
  - `FAIL @ alive (step 9) rc=-1`
- The expected improvement is:
  - it gets past `alive` to `hcmd` (step 10) or later
- Better progress is:
  - `mac-init` (step 11)
  - `scan` (step 12)
  - `done` (step 13)
- Best case is:
  - `wifi: driver=ready`
  - no `FAIL @ ...`
  - `link=associated secure=yes dhcp=configured`
  - real `ssid`, `bssid`, `channel`
  - non-zero `ip`, `gw`, `dns`

WiFi init step mapping:

- `1` = `pci`
- `2` = `nic-reset`
- `3` = `fw-load`
- `4` = `tlv-parse`
- `5` = `dma-alloc`
- `6` = `tfh-init`
- `7` = `msi`
- `8` = `fw-upload`
- `9` = `alive`
- `10` = `hcmd`
- `11` = `mac-init`
- `12` = `scan`
- `13` = `done`

Short version:

- Boot
- Run `ws`
- If it still fails, capture the exact `FAIL @ ... (step N) rc=X` line
- The specific target for the current fix is getting past step 9 `alive` to step 10 `hcmd` or beyond

Workflow memory for this repo:

- When the task is a WiFi fix or boot-path fix, do not stop at source edits or
  `kernel.elf` unless the user explicitly says not to build.
- Normal expected deliverables are:
  - update the relevant `logs/fryNNN.txt` note
  - rebuild the real boot artifact (`build_iso.sh` / ISO), not just the object
    or kernel link step
- Do not pad the close-out with future-session promises; report what was built
  and what was not built.
