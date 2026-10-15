# Source from /etc/portage/bashrc. Leave MAKEOPTS empty in make.conf to
# discover parallelism; package.env can still provide an explicit job limit.
distcc_configure_jobs() {
    local jobs variable marker
    # Portage saves the phase environment. Refresh only values we generated;
    # package.env and ebuild overrides remain authoritative. Portage removes
    # DISTCC_* when saving phases, so keep provenance outside that namespace.
    for variable in MAKEOPTS NINJAOPTS NINJAFLAGS CMAKE_BUILD_PARALLEL_LEVEL; do
        marker=distcc_auto_${variable}
        if [[ -n ${!marker} && ${!variable} == "${!marker}" ]]; then
            unset "${variable}"
        fi
        unset "${marker}"
    done
    if [[ -z ${MAKEOPTS} ]]; then
        if has distcc ${FEATURES} && ! has distcc ${RESTRICT}; then
            jobs=$(DISTCC_RECONNECT_TIMEOUT=2 timeout 5 distcc -j 2>/dev/null) || jobs=
            # A reachable but temporarily exhausted pool must not turn into
            # an unlimited -j0 or a larger local fallback.
            [[ ${jobs} == 0 ]] && jobs=1
        fi
        [[ ${jobs} =~ ^[1-9][0-9]*$ ]] || jobs=$(nproc)
        export MAKEOPTS="-j${jobs}"
        distcc_auto_MAKEOPTS=${MAKEOPTS}
    fi
    if [[ ${MAKEOPTS} =~ (^|[[:space:]])(-j[[:space:]]*|--jobs[=[:space:]]+)([1-9][0-9]*)([[:space:]]|$) ]]; then
        jobs=${BASH_REMATCH[3]}
        # Ninja otherwise synthesizes a load-average option from MAKEOPTS.
        for variable in NINJAOPTS NINJAFLAGS CMAKE_BUILD_PARALLEL_LEVEL; do
            if [[ -z ${!variable} ]]; then
                marker=distcc_auto_${variable}
                if [[ ${variable} == CMAKE_BUILD_PARALLEL_LEVEL ]]; then
                    export "${variable}=${jobs}"
                    printf -v "${marker}" '%s' "${jobs}"
                else
                    export "${variable}=-j${jobs}"
                    printf -v "${marker}" '%s' "-j${jobs}"
                fi
            fi
        done
    fi
}

case ${EBUILD_PHASE} in
    configure|compile|test|install) distcc_configure_jobs ;;
esac
