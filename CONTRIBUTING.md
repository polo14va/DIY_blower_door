# Contributing

Thanks for your interest in improving this project.

## Before opening a PR

1. Open an issue first for non-trivial changes.
2. Keep PRs focused and small.
3. Include validation steps and expected behavior.
4. Verify local build succeeds.

## Local validation

```bash
./scripts/doctor.sh
SKIP_FLASH=1 ./scripts/build_flash_rp2350.sh
```

If your change touches OTA flow:

```bash
python3 ./scripts/ota_update.py --help
```

## Branch and commit guidelines

- Branch naming: `feat/<topic>`, `fix/<topic>`, `docs/<topic>`, `chore/<topic>`
- Commit style: short imperative summary, for example:
  - `fix: handle missing FREERTOS_KERNEL_PATH fallback`
  - `docs: add replication checklist`

## Pull request checklist

- [ ] Build passes locally (`SKIP_FLASH=1`).
- [ ] No secrets added (`.wifi-secrets` stays local only).
- [ ] Documentation updated if behavior changed.
- [ ] API/OTA changes are reflected in `docs/web_endpoint_mapping.md`.

## Scope expectations

Good PRs for this repository:

- Reliability and reproducibility improvements
- Build/tooling improvements
- Web UI clarity and operator workflow
- Documentation quality

Out of scope for drive-by PRs:

- Large architecture rewrites without prior discussion
- Breaking changes to API/OTA contract without migration notes
