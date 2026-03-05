#!/bin/bash
# First-time server setup for Anitra Collab Server.
# Idempotent: safe to run multiple times.
set -euo pipefail

echo "=== Anitra Collab Server: First-time setup ==="

# Create dedicated system user (no home dir, no login shell)
if ! id -u anitra &>/dev/null; then
    sudo useradd --system --no-create-home --shell /usr/sbin/nologin anitra
    echo "   Created system user: anitra"
else
    echo "   User anitra already exists"
fi

# Create application directory
sudo mkdir -p /opt/anitra
sudo chown anitra:anitra /opt/anitra
echo "   Directory: /opt/anitra"

# Install systemd service
sudo cp /tmp/anitra-deploy/server/collab_server.service \
    /etc/systemd/system/collab_server.service
sudo systemctl daemon-reload
sudo systemctl enable collab_server
echo "   Service installed and enabled"

# Open firewall port (if ufw is active)
if command -v ufw &>/dev/null && sudo ufw status | grep -q "Status: active"; then
    sudo ufw allow 7777/tcp comment "Anitra collab server"
    echo "   Firewall: port 7777/tcp opened"
else
    echo "   Firewall: ufw not active, skipping (ensure port 7777 is open)"
fi

echo ""
echo "=== Setup complete ==="
echo "   Binary:  /opt/anitra/collab_server"
echo "   Service: collab_server"
echo "   Commands:"
echo "     sudo systemctl start collab_server"
echo "     sudo systemctl status collab_server"
echo "     journalctl -u collab_server -f"
