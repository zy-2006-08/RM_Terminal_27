# Superseded evidence

- `superseded-final-harness.log`: Pre-fix run; receiver pid 41206, no `recoveries` field, and decoder failure values 1, 2, and 3 belong to the superseded build.
- `superseded-final-complete-harness.log`: Post-fix run, but its restart phase keeps `decoded_frames` frozen at 11, so restart recovery is not established.
- `superseded-final-complete-receipt.txt`: Summarises the post-fix run but overstates restart recovery because `decoded_frames` stays frozen at 11 through that phase.
- `superseded-final-verification-receipt.txt`: Superseded receipt; its numeric claims do not meet the evidence-directory requirement that each claim be greppable from the adjacent log.
- `superseded-verification-receipt.txt`: Superseded receipt; its numeric claims do not meet the evidence-directory requirement that each claim be greppable from the adjacent log.
- `superseded-bounded-harness.log`: Superseded harness evidence; retained as a prior QA record and not current authoritative evidence.

- `superseded-notepad-findings.txt`: Claims loss/restart can enter `decoder_no_frame_timeout` and that truncated H.265 injection is missing. Both statements predate the watchdog fix and the real-UDP truncated scenario now in `authoritative-run.log`.
- `superseded-truncated-h265.log`: Older truncated run with receiver rc=-2 and a wrong 8-byte header layout. The protocol header is `>HHI` (frame_id, index, total_bytes); the authoritative run uses it correctly.
- `superseded-e2e-normal-loss-jitter.log`: Records `failure="decoder_exit=255"` and states live restart recovery was still pending. Both are pre-fix conditions.
- `superseded-loss-with-fix.log`, `superseded-restart-fixed.log`, `superseded-restart-failing-first.log`: Intermediate debugging runs captured while root-causing; not evidence.

A fresh authoritative run is produced as `authoritative-run.log` with `authoritative-receipt.txt`, plus `decode-ceiling.log` from `decode_ceiling_analysis.py`.
