# Orquestador en Fly.io

La configuración vive en `/fly.toml` (raíz del repositorio) porque flyctl usa
el directorio del `fly.toml` como contexto de build y la imagen necesita
`backend/` y `infra/Dockerfile.orchestrator`.

## Primer despliegue

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

`FLY_ORG_TOKEN` debe ser un token de organización con permiso para crear y
borrar Apps/Machines (lo usa el orquestador para cada visita). No lo confundas
con el `FLY_API_TOKEN` de CI, que solo despliega el orquestador.

## Imagen de instancia

`infra/instance/Dockerfile` construye la imagen que ejecuta cada visita. Se
publica en el registro de Fly con el workflow `deploy-instance-image.yml` o a
mano:

```bash
fly auth docker
docker build -f infra/instance/Dockerfile -t registry.fly.io/lamp-instance-base:latest .
docker push registry.fly.io/lamp-instance-base:latest
```

## Limpieza

El orquestador expira instancias cada minuto y barre Apps huérfanas cada 10
minutos. Para revisar a mano:

```bash
fly apps list | grep lamp-inst-
```
