#!/bin/bash
# =============================================================================
#  ops/setup-selfhosted-runner.sh
#
#  One-time setup: install and register a GitHub Actions self-hosted runner
#  on this machine so the deploy-camera workflow can reach the LAN camera.
#
#  Run as your normal user (not root).  Sudo is used only where needed.
#
#  Usage:
#    chmod +x ops/setup-selfhosted-runner.sh
#    GITHUB_TOKEN=<PAT> REPO=jaybhandari-sys/kittymain ./ops/setup-selfhosted-runner.sh
#
#  The PAT needs:  repo scope  (Settings → Developer settings → Personal access tokens)
#  After running this script, start the runner with:
#    cd ~/actions-runner && ./run.sh
#  Or install as a systemd service (option prompted at the end).
# =============================================================================
set -euo pipefail

REPO="${REPO:-jaybhandari-sys/kittymain}"
RUNNER_DIR="${HOME}/actions-runner"
RUNNER_VERSION="2.317.0"
RUNNER_LABEL="camera-network"
RUNNER_NAME="${HOSTNAME:-selfhosted-runner}"

if [ -z "${GITHUB_TOKEN:-}" ]; then
  echo "ERROR: set GITHUB_TOKEN to a repo-scoped PAT before running this script."
  echo "  export GITHUB_TOKEN=ghp_xxxxxxxxxxxxxxxxxxxx"
  exit 1
fi

echo "=== [1/5] Install system deps ==="
if command -v apt-get &>/dev/null; then
  sudo apt-get update -qq
  sudo apt-get install -y --no-install-recommends \
    curl jq python3 python3-pip telnet expect
  pip3 install --quiet pexpect
fi

echo "=== [2/5] Download runner ($RUNNER_VERSION) ==="
mkdir -p "$RUNNER_DIR"
cd "$RUNNER_DIR"

ARCH=$(uname -m)
case "$ARCH" in
  x86_64) RUNNER_ARCH="x64" ;;
  aarch64) RUNNER_ARCH="arm64" ;;
  *) echo "Unknown arch $ARCH"; exit 1 ;;
esac

RUNNER_TARBALL="actions-runner-linux-${RUNNER_ARCH}-${RUNNER_VERSION}.tar.gz"
if [ ! -f "$RUNNER_TARBALL" ]; then
  curl -sSLO "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/${RUNNER_TARBALL}"
fi
tar -xzf "$RUNNER_TARBALL"

echo "=== [3/5] Fetch runner registration token ==="
REG_TOKEN=$(curl -sSL \
  -X POST \
  -H "Authorization: token ${GITHUB_TOKEN}" \
  -H "Accept: application/vnd.github+json" \
  "https://api.github.com/repos/${REPO}/actions/runners/registration-token" \
  | jq -r '.token')

if [ "$REG_TOKEN" = "null" ] || [ -z "$REG_TOKEN" ]; then
  echo "ERROR: failed to get registration token. Check GITHUB_TOKEN permissions."
  exit 1
fi

echo "=== [4/5] Configure runner ==="
./config.sh \
  --url "https://github.com/${REPO}" \
  --token "$REG_TOKEN" \
  --name "$RUNNER_NAME" \
  --labels "$RUNNER_LABEL" \
  --unattended \
  --replace

echo "=== [5/5] Install as systemd service? ==="
read -rp "Install runner as a systemd service (auto-start on boot)? [y/N] " ans
if [[ "${ans,,}" == "y" ]]; then
  sudo ./svc.sh install
  sudo ./svc.sh start
  sudo ./svc.sh status
  echo "Runner installed as systemd service and started."
else
  echo ""
  echo "Runner configured. Start it manually with:"
  echo "  cd $RUNNER_DIR && ./run.sh"
fi

echo ""
echo "======================================================"
echo " Self-hosted runner setup complete."
echo " Label:  $RUNNER_LABEL"
echo " Repo:   https://github.com/$REPO/settings/actions/runners"
echo "======================================================"
echo ""
echo "Next steps:"
echo "  1. Verify the runner shows as 'Idle' at:"
echo "     https://github.com/$REPO/settings/actions/runners"
echo ""
echo "  2. Add these GitHub Actions secrets at:"
echo "     https://github.com/$REPO/settings/secrets/actions"
echo "       CAMERA_HOST = <camera LAN IP>"
echo "       CAMERA_USER = <telnet username>"
echo "       CAMERA_PASS = <telnet password>"
echo ""
echo "  3. Push a v2.x tag to trigger a full build + deploy:"
echo "     git tag v2.0.0 && git push origin v2.0.0"
echo ""
echo "  4. Or trigger manually at:"
echo "     https://github.com/$REPO/actions/workflows/deploy-camera.yml"
