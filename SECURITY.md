# Security Policy

## Supported versions

This repository currently tracks a single rolling firmware line from `main`.

## Reporting a vulnerability

Please do not open a public issue for security-sensitive reports.

Report privately by email:

- `developer@pedromartinezweb.com`

Include:

- Affected component and firmware version
- Reproduction steps
- Potential impact
- Suggested mitigation if available

You will receive an acknowledgment and triage response as soon as possible.

## Security boundaries

Current design assumptions:

- Device and operator are on a trusted local network
- OTA endpoint is intended for local trusted use
- Wi-Fi credentials are provided at build time and are not committed

If your deployment model requires stronger hardening, open an issue to discuss requirements and roadmap.
