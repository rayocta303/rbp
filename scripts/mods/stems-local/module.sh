# SPDX-License-Identifier: MPL-2.0
# MAKE STEMS (LOCAL): la console espone sul cavo USB la coda dei brani da separare e
# riceve gli stem dall'app sul Mac, che li scrive in RX3_STEMS sulla chiavetta.
# Protocollo e logica ispirati a "Remote Stems" di AZ-MODS (Kyle Hosman, MIT): qui la
# console fa da server (busybox httpd) perché la RX3 non ha curl.
module_begin stems-local stems_local

STEMS_LOCAL_DIR=/tmp/stems-local
STEMS_LOCAL_PORT=8028
# Indirizzo fisso link-local sul cavo USB: il Mac (169.254.x.x da solo) lo raggiunge
# senza permessi di amministratore. STEMS_LOCAL_BIND solo per le prove nell'emulatore.
STEMS_LOCAL_ADDR=${STEMS_LOCAL_BIND:-169.254.28.28}

stems_local_iface()
{
    for stems_local_if in usb0 usb1 eth1 eth0; do
        ifconfig "$stems_local_if" > /dev/null 2>&1 && { echo "$stems_local_if"; return 0; }
    done
    return 1
}

stems_local_start()
{
    stems_local_conf="$USB/RX3_RUNTIME/stems-local"
    mkdir -p "$stems_local_conf/coda" 2>/dev/null || { say "stems-local: drive not writable"; return 0; }
    # Abbinamento: la password resta sulla chiavetta; l'app la legge quando la
    # chiavetta è collegata al Mac (una volta, come l'AZ).
    if [ ! -s "$stems_local_conf/token" ]; then
        dd if=/dev/urandom bs=16 count=1 2>/dev/null | hexdump -v -e '1/1 "%02x"' \
            > "$stems_local_conf/token" 2>/dev/null
    fi
    stems_local_token=$(tr -cd '0-9a-f' < "$stems_local_conf/token" 2>/dev/null)
    [ ${#stems_local_token} -ge 16 ] || { say "stems-local: no pairing token"; return 0; }

    # Reinserting the stick finds the previous server still bound to the port
    # (RX3, 2026-10-14: "bind: Address already in use"): stop it and wait.
    for stems_local_proc in /proc/[0-9]*; do
        grep -q "$STEMS_LOCAL_DIR/httpd.conf" "$stems_local_proc/cmdline" 2>/dev/null &&
            kill "${stems_local_proc##*/}" 2>/dev/null
    done
    stems_local_wait=0
    while [ $stems_local_wait -lt 20 ] &&
          grep -qs "$STEMS_LOCAL_DIR/httpd.conf" /proc/[0-9]*/cmdline; do
        sleep 0.1 2>/dev/null || sleep 1
        stems_local_wait=$((stems_local_wait + 1))
    done
    rm -rf "$STEMS_LOCAL_DIR"
    mkdir -p "$STEMS_LOCAL_DIR/www/cgi-bin" || return 0
    cp /mnt/iso/modules/stems-local/www/cgi-bin/* "$STEMS_LOCAL_DIR/www/cgi-bin/" || return 0
    chmod 755 "$STEMS_LOCAL_DIR"/www/cgi-bin/*
    printf '%s\n' "$USB" > "$STEMS_LOCAL_DIR/usb"
    printf '/cgi-bin:ventotto:%s\n' "$stems_local_token" > "$STEMS_LOCAL_DIR/httpd.conf"

    if [ -z "${STEMS_LOCAL_BIND:-}" ]; then
        stems_local_if=$(stems_local_iface) || {
            say "stems-local: no network interface on the USB cable"
            say "stems-local: interfaces: $(ls /sys/class/net 2>/dev/null | tr '\n' ' ')"
            return 0
        }
        ifconfig "$stems_local_if:28" "$STEMS_LOCAL_ADDR" netmask 255.255.0.0 up 2>/dev/null ||
            say "stems-local: could not add $STEMS_LOCAL_ADDR on $stems_local_if"
        say "stems-local: $stems_local_if ($(ls /sys/class/net 2>/dev/null | tr '\n' ' '))"
    fi
    # Su tutte le interfacce (la password protegge): il cavo USB può comparire
    # dopo l'avvio e con un nome diverso (sulla RX3 reale: eth0). Il Mac trova
    # la console dal mittente dei messaggi now-playing (UDP 50123).
    stems_local_listen=${STEMS_LOCAL_BIND:+$STEMS_LOCAL_BIND:}$STEMS_LOCAL_PORT
    if stems_local_err=$(busybox httpd -p "$stems_local_listen" -h "$STEMS_LOCAL_DIR/www" \
            -c "$STEMS_LOCAL_DIR/httpd.conf" -r ventotto 2>&1); then
        pidof httpd | awk '{print $1}' > "$STEMS_LOCAL_DIR/httpd.pid"
        say "stems-local: listening on port $STEMS_LOCAL_PORT"
    else
        say "WARNING: stems-local: httpd did not start: $stems_local_err"
    fi
}
register_prepare_hook stems_local_start
