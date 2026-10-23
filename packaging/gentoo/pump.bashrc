# Source from /etc/portage/bashrc; call these from the compile phase hooks.
# Enable per package with DISTCC_PUMP=1 in a package.env file.

distcc_pump_remove_root() {
    # Keep the published lease until all substantial contents are gone, even
    # when this cleanup itself is interrupted.
    find "$1" -mindepth 1 -maxdepth 1 ! -name lease -exec rm -rf -- {} + || return
    rm -f -- "$1/lease" && rmdir -- "$1"
}

distcc_pump_start() {
    [[ ${DISTCC_PUMP:-0} == 1 ]] || return 0
    [[ -z ${INCLUDE_SERVER_PORT} ]] || return 0
    has distcc ${FEATURES} || die "DISTCC_PUMP requires FEATURES=distcc"
    register_die_hook distcc_pump_stop
    local settings original_path=${PATH}
    # PID namespaces can reuse the analyzer PID across simultaneous builds.
    # Give automatic tmpfs storage a private root so PID-based cleanup stays local.
    if [[ ! ${DISTCC_CLIENT_TMP+x} && -d /dev/shm && -w /dev/shm ]]; then
        local directory lease_fd
        for directory in /dev/shm/distcc-pump.*; do
            [[ -d ${directory} && -O ${directory} && -f ${directory}/lease ]] || continue
            if exec {lease_fd}>"${directory}/lease"; then
                if flock -n "${lease_fd}"; then
                    distcc_pump_remove_root "${directory}"
                fi
                exec {lease_fd}>&-
            fi
        done
        DISTCC_PUMP_CLIENT_TMP=$(mktemp -d /dev/shm/distcc-pump.XXXXXX) || die
        exec {DISTCC_PUMP_CLIENT_FD}>"${DISTCC_PUMP_CLIENT_TMP}/.lease" || die
        flock "${DISTCC_PUMP_CLIENT_FD}" || die
        # Publish only after locking; another startup must not collect a root
        # between its creation and acquisition of the lifetime lease.
        mv -- "${DISTCC_PUMP_CLIENT_TMP}/.lease" "${DISTCC_PUMP_CLIENT_TMP}/lease" || die
        export DISTCC_CLIENT_TMP=${DISTCC_PUMP_CLIENT_TMP}
    fi
    # Package work paths can exceed the UNIX socket path limit. Reuse the
    # private, lifetime-managed tmpfs root when automatic storage is active.
    settings=$(TMPDIR="${DISTCC_PUMP_CLIENT_TMP:-${T}}" pump --startup) || die "Cannot start distcc-pump"
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
        distcc_pump_remove_root "${DISTCC_PUMP_CLIENT_TMP}"
        unset DISTCC_PUMP_CLIENT_TMP DISTCC_CLIENT_TMP
    fi
    if [[ -n ${DISTCC_PUMP_CLIENT_FD:-} ]]; then
        exec {DISTCC_PUMP_CLIENT_FD}>&-
        unset DISTCC_PUMP_CLIENT_FD
    fi
}
