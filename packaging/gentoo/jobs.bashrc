# Source from /etc/portage/bashrc. Discover parallelism when MAKEOPTS has no
# job option; package.env can still provide an explicit job limit.
distcc_configure_jobs() {
    local jobs variable marker compiler resolved distributed
    # Portage saves the phase environment. Refresh only values we generated;
    # package.env and ebuild overrides remain authoritative. Portage removes
    # DISTCC_* when saving phases, so keep provenance outside that namespace.
    for variable in MAKEOPTS NINJAOPTS NINJAFLAGS CMAKE_BUILD_PARALLEL_LEVEL; do
        marker=distcc_auto_${variable}
        if [[ -n ${!marker} && ${!variable} == "${!marker}" ]]; then
            if [[ ${variable} == MAKEOPTS ]]; then
                MAKEOPTS=${distcc_auto_make_options}
            else
                unset "${variable}"
            fi
        fi
        unset "${marker}"
    done
    unset distcc_auto_make_options
    # GCC adds --output-sync before phases run. Such options do not specify
    # concurrency; a bare -j/--jobs, however, is an explicit unlimited choice.
    if [[ ! ${MAKEOPTS} =~ (^|[[:space:]])(-j([[:space:]]|[0-9]|$)|--jobs([=[:space:]]|$)) ]]; then
        if has distcc ${FEATURES} && ! has distcc ${RESTRICT}; then
            distributed=1
            # Absolute compiler paths bypass Portage's PATH wrappers. Only an
            # explicit distcc executable (or a symlink to it) opts back in.
            for compiler in "${CC}" "${CXX}"; do
                compiler=${compiler%%[[:space:]]*}
                if [[ ${compiler} == */* ]]; then
                    resolved=$(readlink -f -- "${compiler}") || resolved=
                    [[ ${resolved##*/} == distcc ]] || distributed=0
                fi
            done
        fi
        if [[ ${distributed} == 1 ]]; then
            jobs=$(DISTCC_RECONNECT_TIMEOUT=2 timeout 5 distcc -j 2>/dev/null) || jobs=
            # A reachable but temporarily exhausted pool must not turn into
            # an unlimited -j0 or a larger local fallback.
            [[ ${jobs} == 0 ]] && jobs=1
        fi
        [[ ${jobs} =~ ^[1-9][0-9]*$ ]] || jobs=$(nproc)
        distcc_auto_make_options=${MAKEOPTS}
        export MAKEOPTS="${MAKEOPTS:+${MAKEOPTS} }-j${jobs}"
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
