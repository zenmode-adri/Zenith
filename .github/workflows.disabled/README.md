# Disabled upstream workflows

These GitHub Actions workflows come from the upstream ROCKNIX/AURKNIX projects. They build every device with
secrets (`GH_PAT`, scraper API keys) that this fork does not have, and some run on a schedule or on every push.
They are kept here for reference and are **not** executed (GitHub only runs files in `.github/workflows/`).
Zenith images are built locally with `scripts/build_distro` and published by hand.
