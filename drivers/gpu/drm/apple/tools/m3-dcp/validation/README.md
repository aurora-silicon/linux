# Validation evidence policy

Keep concise validation summaries, checksums and helper source in Git. Raw
screenshots, CTS result files, generated case catalogs, kernel logs and telemetry
remain local and are ignored. Existing published evidence is retained unchanged;
this policy removes generated data only from the previously unpublished work.

On 2026-09-21, 52 unpublished M3 commits were rebuilt to omit generated evidence.
Source code was verified unchanged. Older commit IDs in summaries can be mapped
through `history-map.md` in the M3 lab validation directory. Raw artifact paths in
those summaries refer to local retained evidence, not files shipped in this tree.

The original history is preserved locally in
`refs/backup/m3-before-artifact-cleanup-20260921` and in the verified incremental
bundle `build/m3-push-audit/unpublished-with-raw-evidence.bundle` in the source
collection workspace. Its prerequisite is the published commit
`4ddd13c6bf5941dc23a298a054b390850cb2294d`. No raw evidence was discarded, and no
published history was rewritten.
