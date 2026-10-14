# Source from /etc/portage/bashrc; call these from the compile phase hooks.
# Enable per package with DISTCC_PUMP=1 in a package.env file.

distcc_pump_start() {
    [[ ${DISTCC_PUMP:-0} == 1 ]] || return 0
    [[ -z ${INCLUDE_SERVER_PORT} ]] || return 0
    has distcc ${FEATURES} || die "DISTCC_PUMP requires FEATURES=distcc"
    local settings original_path=${PATH}
    export DISTCC_CLIENT_TMP=${T}/distcc-pump
    mkdir -p "${DISTCC_CLIENT_TMP}" || die
    settings=$(TMPDIR="${T}" pump --startup) || die "Cannot start distcc-pump"
    eval "${settings}" || die "Cannot configure distcc-pump"
    # Keep Portage's compiler wrappers ahead of the real compiler directory.
    export PATH=${original_path}
    DISTCC_PUMP_ACTIVE=1
}

distcc_pump_stop() {
    [[ ${DISTCC_PUMP_ACTIVE:-0} == 1 ]] || return 0
    pump --shutdown
    unset DISTCC_PUMP_ACTIVE INCLUDE_SERVER_PID INCLUDE_SERVER_DIR INCLUDE_SERVER_PORT
}
