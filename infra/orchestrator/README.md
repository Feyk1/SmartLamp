# Orchestrator on Fly.io

The configuration lives in `/fly.toml` at the repository root because flyctl
uses the directory containing `fly.toml` as the build context, and the image
needs `backend/` and `infra/Dockerfile.orchestrator`.

## First deployment

```bash
fly auth login
fly apps create lamp-demo-orchestrator
fly secrets set \
  FLY_ORG_TOKEN=... \
  JWT_SECRET=... \
  DEVICE_TOKEN=... \
  SIM_PIN=2025 \
  DASHBOARD_ORIGINS=https://feyk1.github.io
fly deploy
```

`FLY_ORG_TOKEN` must be an organization token allowed to create and delete
Apps/Machines; the orchestrator uses it for every visit. It is not the
`FLY_API_TOKEN` used by CI, which only deploys the orchestrator.

## Instance image

`infra/instance/Dockerfile` builds the image that runs each visit. It is
published to the Fly registry by the `deploy-instance-image.yml` workflow or
manually:

```bash
fly auth docker
docker build -f infra/instance/Dockerfile -t registry.fly.io/lamp-instance-base:latest .
docker push registry.fly.io/lamp-instance-base:latest
```

## Cleanup

The orchestrator expires instances every minute and sweeps orphan Apps every
10 minutes. To inspect them manually:

```bash
fly apps list | grep lamp-inst-
```
