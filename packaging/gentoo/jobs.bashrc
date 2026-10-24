# Source from /etc/portage/bashrc. Discover parallelism when MAKEOPTS has no
# job option; package.env can still provide an explicit job limit.
distcc_compiler_is_distributed() {
    local compiler=${1%%[[:space:]]*} resolved configured prefix search directory candidate
    local -a directories
    # Bare compiler names use Portage's distcc PATH wrappers.
    [[ ${compiler} == */* ]] || return 0
    resolved=$(readlink -f -- "${compiler}") || return 1
    [[ ${resolved##*/} == distcc ]] && return 0
    [[ ${resolved##*/} == ccache && ${compiler##*/} != ccache ]] || return 1

    # An absolute ccache masquerade link still searches for its compiler.
    # Ask ccache for effective settings so config files and environment
    # overrides follow the same route as the actual compilation.
    configured=$("${resolved}" --get-config compiler 2>/dev/null) || return 1
    prefix=$("${resolved}" --get-config prefix_command 2>/dev/null) || return 1
    if [[ -n ${prefix} ]]; then
        prefix=${prefix%%[[:space:]]*}
        candidate=$(type -P -- "${prefix}") || return 1
        candidate=$(readlink -f -- "${candidate}") || return 1
        [[ ${candidate##*/} == distcc ]]
        return
    fi
    compiler=${configured:-${compiler##*/}}
    if [[ ${compiler} == */* ]]; then
        candidate=$(readlink -f -- "${compiler}") || return 1
        [[ ${candidate##*/} == distcc ]]
        return
    fi
    search=$("${resolved}" --get-config path 2>/dev/null) || return 1
    IFS=: read -r -a directories <<< "${search:-${PATH}}:"
    for directory in "${directories[@]}"; do
        candidate=${directory:-.}/${compiler}
        [[ -f ${candidate} && -x ${candidate} ]] || continue
        candidate=$(readlink -f -- "${candidate}") || return 1
        [[ ${candidate} == "${resolved}" ]] && continue
        [[ ${candidate##*/} == distcc ]]
        return
    done
    return 1
}

distcc_configure_jobs() {
    local jobs variable marker compiler distributed
    # Cargo uses an explicit compiler wrapper, not the C/C++ masquerade PATH.
    # Preserve user/package wrappers and clear only our saved-phase value when
    # a package restricts distribution.
    if [[ ${distcc_auto_RUSTC_WRAPPER} && ${RUSTC_WRAPPER} == "${distcc_auto_RUSTC_WRAPPER}" ]]; then
        unset RUSTC_WRAPPER distcc_auto_RUSTC_WRAPPER
    fi
    if has distcc ${FEATURES} && ! has distcc ${RESTRICT} && [[ ! ${RUSTC_WRAPPER+x} ]]; then
        export RUSTC_WRAPPER=distcc
        distcc_auto_RUSTC_WRAPPER=${RUSTC_WRAPPER}
    fi
    # Portage saves the phase environment. Refresh only values we generated;
    # package.env and ebuild overrides remain authoritative. Portage removes
    # DISTCC_* when saving phases, so keep provenance outside that namespace.
    for variable in MAKEOPTS NINJAOPTS NINJAFLAGS CMAKE_BUILD_PARALLEL_LEVEL CARGO_BUILD_JOBS; do
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
            for compiler in "${CC}" "${CXX}"; do
                distcc_compiler_is_distributed "${compiler}" || distributed=0
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
        # Cargo's config may have been generated in unpack, before MAKEOPTS
        # was computed. The environment overrides that stale jobs = 1 value.
        for variable in NINJAOPTS NINJAFLAGS CMAKE_BUILD_PARALLEL_LEVEL CARGO_BUILD_JOBS; do
            if [[ -z ${!variable} ]]; then
                marker=distcc_auto_${variable}
                if [[ ${variable} == CMAKE_BUILD_PARALLEL_LEVEL || ${variable} == CARGO_BUILD_JOBS ]]; then
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
