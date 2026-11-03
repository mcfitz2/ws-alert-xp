#!/bin/sh
# Turn PipeWire's Bluetooth audio monitor off or on for the current user, so
# hfp_ag.py can own the HFP/HSP AG profile. "on" restores the default.
#   wp-bt.sh off|on|status
CONF="$HOME/.config/wireplumber/wireplumber.conf.d/80-peltor-no-bluez.conf"
case "$1" in
  off)
    mkdir -p "$(dirname "$CONF")"
    cat > "$CONF" <<'EOF'
wireplumber.profiles = {
  main = {
    monitor.bluez = disabled
  }
}
EOF
    systemctl --user restart wireplumber ;;
  on)
    rm -f "$CONF"
    systemctl --user restart wireplumber ;;
  status)
    [ -f "$CONF" ] && echo "bluez monitor: off" || echo "bluez monitor: on" ;;
  *)
    echo "usage: $0 off|on|status" >&2; exit 2 ;;
esac
