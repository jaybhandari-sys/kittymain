# Augentix cross-toolchain — fetched by CI

Cross-compilation of `provider_srt` for the camera (`armv7l, uClibc-gnueabihf`) needs the Augentix toolchain.  It's too big to vendor in this repo (324 MB unpacked, 58 MB compressed), so we keep it in GCS and let CI fetch it on demand.

## Where it lives

| | |
|---|---|
| GCS bucket | `gs://arcisai-build-toolchains` (project `arcisai-iot-platform`, region `asia-south1`, **private**) |
| Object | `arm-augentix-linux-uclibcgnueabihf-2025.tar.xz` |
| Compressed size | ~58 MB |
| Unpacked size | ~324 MB |
| Service account with read access | `kitty-ci-fetcher@arcisai-iot-platform.iam.gserviceaccount.com` |
| What's inside | Self-contained GCC 9.x + uClibc-ng + binutils for `arm-augentix-linux-uclibcgnueabihf`.  Drops cleanly into `/opt/augentix-tc/`. |

## How CI fetches it (when we tag v1.13.0+)

The `build-cross` job in `.github/workflows/build-augentix.yml` looks for `vars.TOOLCHAIN_S3_URL`.  Two patterns work:

### Pattern A — short-lived signed URL (used today)

Right before tagging, generate a fresh signed URL:

```sh
gcloud storage sign-url \
    gs://arcisai-build-toolchains/arm-augentix-linux-uclibcgnueabihf-2025.tar.xz \
    --duration=12h \
    --impersonate-service-account=kitty-ci-fetcher@arcisai-iot-platform.iam.gserviceaccount.com \
    --project=arcisai-iot-platform
```

Set the URL as a repo Variable: **Settings → Secrets and variables → Actions → Variables → `TOOLCHAIN_S3_URL`**.

Tag immediately (URL is good for 12 h).

### Pattern B — Workload Identity Federation (one-time setup, no expiring URLs)

Better long-term: bind GitHub Actions OIDC to the `kitty-ci-fetcher` SA.  CI workflows then fetch with `gcloud storage cp` using short-lived federated credentials, no signed URL needed.

Steps (to be done when we're ready for fleet-wide tagging cadence):

```sh
# 1. Create the WIF pool
gcloud iam workload-identity-pools create github \
    --location=global --display-name="GitHub Actions"

# 2. Add GitHub as a provider, restricted to Adiance-Technologies org
gcloud iam workload-identity-pools providers create-oidc github-actions \
    --workload-identity-pool=github --location=global \
    --issuer-uri=https://token.actions.githubusercontent.com \
    --attribute-mapping=google.subject=assertion.sub,attribute.repository=assertion.repository \
    --attribute-condition='assertion.repository_owner == "Adiance-Technologies"'

# 3. Let the kitty-ci-fetcher SA be impersonated from the kitty-augentix-camera repo
gcloud iam service-accounts add-iam-policy-binding \
    kitty-ci-fetcher@arcisai-iot-platform.iam.gserviceaccount.com \
    --role=roles/iam.workloadIdentityUser \
    --member="principalSet://iam.googleapis.com/projects/372817526950/locations/global/workloadIdentityPools/github/attribute.repository/Adiance-Technologies/kitty-augentix-camera"

# 4. In .github/workflows/build-augentix.yml, replace the curl/signed-URL step with:
#   - uses: google-github-actions/auth@v2
#     with:
#       workload_identity_provider: projects/372817526950/locations/global/workloadIdentityPools/github/providers/github-actions
#       service_account: kitty-ci-fetcher@arcisai-iot-platform.iam.gserviceaccount.com
#   - run: gcloud storage cp gs://arcisai-build-toolchains/arm-augentix-...tar.xz - | sudo tar -xJ -C /opt/augentix-tc
```

## How to update the toolchain in the bucket

If we move to a newer Augentix SDK:

```sh
# Pack
cd /path/to/new-toolchain
tar -cJf arm-augentix-linux-uclibcgnueabihf-YYYY.tar.xz arm-augentix-linux-uclibcgnueabihf/

# Upload alongside the existing one (objects are immutable; use a date suffix)
gcloud storage cp arm-augentix-linux-uclibcgnueabihf-YYYY.tar.xz \
    gs://arcisai-build-toolchains/

# Update vars.TOOLCHAIN_S3_URL repo variable to point at the new object.
```

## Why not just commit the toolchain into the repo

324 MB unpacked is a lot of git history bloat (would dominate the repo for the next decade).  Git LFS solves the size but adds bandwidth quota concerns.  GCS object storage is the standard pattern for "big, slowly-changing binary artifact CI needs to fetch" — same pattern Buildkite, CircleCI, and most production shops use.
