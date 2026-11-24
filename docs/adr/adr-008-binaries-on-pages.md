# ADR-008 — `.ota` binaries served from GitHub Pages, not from Releases

- **Date:** 2026-11-18
- **Status:** Accepted
- **Decision makers:** Project maintainer
- **Project context:** The dashboard needs to download the `.ota` binaries from the browser to show them in the Simulation Panel and so that the firmware can download them over REST.

## Context

The firmware and the dashboard need to access the `.ota` binaries:

- **Firmware:** downloads the `.ota` via `GET /ota/{version}/download` to verify and apply it.
- **Dashboard:** lists the available versions (`GET /ota/versions`) and allows the visitor to select one to deploy.

The `.ota` binaries are generated in CI and published as artifacts. There are two natural places to publish them:

1. **GitHub Releases:** GitHub's standard mechanism for versioned artifacts.
2. **GitHub Pages:** the static site that already hosts the dashboard.

Three options were evaluated:

1. **Releases only** (the dashboard `fetch`es the Release asset URL).
2. **Pages only** (the binaries are copied to `docs/firmware/<version>/`).
3. **Both** (Releases for versioning, Pages for download from the dashboard).

## Decision

**"Both" is chosen, with Pages as the download source for the dashboard and Releases as the source of truth/versioning.**

Specifically:

- The release workflow copies the `.ota` to `docs/firmware/<version>/firmware.ota` and deploys Pages.
- The same `.ota` is published as a GitHub Release asset (for versioning and so that anyone can download it manually).
- The dashboard consumes the binary from **Pages** (same origin), not from Releases.
- The firmware downloads the `.ota` via the orchestrator endpoint (`GET /ota/{version}/download`), which in turn redirects to Pages or serves the file from its own storage.

## Alternatives considered

### Alternative A: Releases only

**Advantages:**
- **Native versioning:** GitHub Releases is designed for this.
- **No duplication:** a single place for the binaries.
- **History:** every Release has its own changelog and assets.
- **Public API:** `GET /repos/{owner}/{repo}/releases` lists the versions.

**Drawbacks:**
- **CORS blocks download from the browser:** Release assets are served from `release-assets.githubusercontent.com`, which **does not send CORS headers**. A `fetch()` from the dashboard fails.
- **Redirect without CORS:** the `github.com/.../releases/download/...` URL redirects to `release-assets.githubusercontent.com`, and neither the redirect nor the final response includes `Access-Control-Allow-Origin`.
- **Complex workaround:** the GitHub API endpoint (`/repos/.../releases/assets/{id}`) can be used with `Accept: application/octet-stream`, which **does** follow the redirect with CORS, but it requires first obtaining the `asset_id` (an extra request) and handling the blob in JavaScript.

**Discarded as the main source because:** CORS adds unnecessary complexity to the dashboard and an extra request per download.

### Alternative B: Pages only

**Advantages:**
- **No CORS:** the dashboard and the binaries are served from the same origin (`feyk1.github.io`).
- **Direct download:** `fetch('/firmware/v1.1.0/firmware.ota')` works without workarounds.
- **Simple:** a single place for the dashboard, the documentation and the binaries.

**Drawbacks:**
- **No native versioning:** Pages has no concept of a "release"; a manual index (`docs/firmware/index.json`) with the available versions must be maintained.
- **No changelog:** there is no natural place for release notes.
- **Size limit:** GitHub Pages has a 1 GB limit per site; for a ~250 KB firmware, that is more than enough.
- **No download history:** you cannot know how many times a version was downloaded (Releases does allow it).

**Discarded as the only source because:** native versioning and the Releases changelog are lost.

### Alternative C: Both (chosen)

**Advantages:**
- **No CORS:** the dashboard downloads from Pages (same origin).
- **Native versioning:** Releases still exists for versioning, changelog and download history.
- **Redundancy:** if Pages fails, the binaries are still in Releases (and vice versa).
- **Flexibility:** the firmware can download from the orchestrator, which can redirect to either of the two.
- **Documentation:** the README can link to Releases so the binary can be downloaded manually.

**Drawbacks:**
- **Duplication:** the same binary lives in two places (~250 KB × 2 = 500 KB per version; negligible).
- **Two steps in the workflow:** copy to Pages and create the Release.
- **Risk of desynchronization:** if one step fails, the two places might not match. Mitigation: the workflow performs both steps in the same job, and `verify` validates before publishing.

**Chosen because:** the balance between download simplicity (Pages), native versioning (Releases) and redundancy is the best.

## Consequences

### Positive

- **Download without CORS:** the dashboard does `fetch('/firmware/v1.1.0/firmware.ota')` without workarounds.
- **Native versioning:** Releases still exists with changelog and assets.
- **Redundancy:** two sources of truth for the binaries.
- **Flexibility:** the firmware can download from the orchestrator, which redirects to Pages.

### Negative

- **Duplication:** the binary lives in two places.
- **Two steps in the workflow:** copy to Pages and create the Release.
- **Risk of desynchronization:** mitigated by the workflow order and `verify`.

### Neutral

- **The orchestrator can redirect to Pages:** `GET /ota/{version}/download` returns a `302` to `https://feyk1.github.io/SmartLamp/firmware/<version>/firmware.ota`.
- **The dashboard lists versions from a static index:** `docs/firmware/index.json` generated in CI, or from the orchestrator's `GET /ota/versions` (which reads `OTA_DIR`).
- **The firmware does not need to know where it downloads from:** it only follows the URL the orchestrator gives it.

## Implementation notes

### Release workflow

```yaml
- name: Copy to Pages
  run: |
    mkdir -p docs/firmware/${{ github.ref_name }}
    cp firmware.ota docs/firmware/${{ github.ref_name }}/
    # Update index
    python tools/update_firmware_index.py \
      --version ${{ github.ref_name }} \
      --sha256 $(sha256sum firmware.ota | cut -d' ' -f1) \
      --size $(stat -c%s firmware.ota)
- uses: softprops/action-gh-release@v2
  with:
    files: firmware.ota
    generate_release_notes: true
- uses: actions/upload-pages-artifact@v3
  with:
    path: docs/
- uses: actions/deploy-pages@v4
```

### Version index (`docs/firmware/index.json`)

```json
{
  "versions": [
    { "version": "v1.1.0", "sha256": "...", "size": 245760,
      "released_at": "2026-11-18T...", "url": "/firmware/v1.1.0/firmware.ota" },
    { "version": "v1.0.3", "sha256": "...", "size": 241664,
      "released_at": "2026-11-10T...", "url": "/firmware/v1.0.3/firmware.ota" }
  ]
}
```

### Orchestrator endpoint

```python
@app.get("/api/v1/device/ota/{version}/download")
async def download_ota(version: str):
    # Redirect to Pages
    return RedirectResponse(
        url=f"https://feyk1.github.io/SmartLamp/firmware/{version}/firmware.ota",
        status_code=302,
    )
```

## References

- [`06-deployment.md`](../06-deployment.md) — section 5.2
- [`05-ci-cd.md`](../05-ci-cd.md) — section 4.5
- [GitHub Releases — CORS issue](https://github.com/orgs/community/discussions/24795)
- [GitHub Pages — CORS](https://docs.github.com/en/pages)
