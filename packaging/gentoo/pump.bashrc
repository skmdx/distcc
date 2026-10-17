# Source from /etc/portage/bashrc; call these from the compile phase hooks.
# Enable per package with DISTCC_PUMP=1 in a package.env file.

distcc_pump_start() {
    [[ ${DISTCC_PUMP:-0} == 1 ]] || return 0
    [[ -z ${INCLUDE_SERVER_PORT} ]] || return 0
    has distcc ${FEATURES} || die "DISTCC_PUMP requires FEATURES=distcc"
    register_die_hook distcc_pump_stop
    local settings original_path=${PATH}
    # PID namespaces can reuse the analyzer PID across simultaneous builds.
    # Give automatic tmpfs storage a private root so PID-based cleanup stays local.
    if [[ ! ${DISTCC_CLIENT_TMP+x} && -d /dev/shm && -w /dev/shm ]]; then
        DISTCC_PUMP_CLIENT_TMP=$(mktemp -d /dev/shm/distcc-pump.XXXXXX) || die
        export DISTCC_CLIENT_TMP=${DISTCC_PUMP_CLIENT_TMP}
    fi
    settings=$(TMPDIR="${T}" pump --startup) || die "Cannot start distcc-pump"
    eval "${settings}" || die "Cannot configure distcc-pump"
    # Keep Portage's compiler wrappers ahead of the real compiler directory.
    export PATH=${original_path}
    DISTCC_PUMP_ACTIVE=1
}

distcc_pump_stop() {
    if [[ ${DISTCC_PUMP_ACTIVE:-0} == 1 ]]; then
        pump --shutdown
        unset DISTCC_PUMP_ACTIVE INCLUDE_SERVER_PID INCLUDE_SERVER_DIR INCLUDE_SERVER_PORT
    fi
    if [[ -n ${DISTCC_PUMP_CLIENT_TMP:-} ]]; then
        rm -rf -- "${DISTCC_PUMP_CLIENT_TMP}"
        unset DISTCC_PUMP_CLIENT_TMP DISTCC_CLIENT_TMP
    fi
}
