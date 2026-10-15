# Source from /etc/portage/bashrc. Leave MAKEOPTS empty in make.conf to
# discover parallelism; package.env can still provide an explicit job limit.
distcc_configure_jobs() {
    local jobs
    if [[ -z ${MAKEOPTS} ]]; then
        if has distcc ${FEATURES} && ! has distcc ${RESTRICT}; then
            jobs=$(DISTCC_RECONNECT_TIMEOUT=2 timeout 5 distcc -j 2>/dev/null) || jobs=
        fi
        [[ ${jobs} =~ ^[1-9][0-9]*$ ]] || jobs=$(nproc)
        export MAKEOPTS="-j${jobs}"
    fi
    if [[ ${MAKEOPTS} =~ (^|[[:space:]])(-j[[:space:]]*|--jobs[=[:space:]]+)([1-9][0-9]*)([[:space:]]|$) ]]; then
        jobs=${BASH_REMATCH[3]}
        # Ninja otherwise synthesizes a load-average option from MAKEOPTS.
        export NINJAOPTS=${NINJAOPTS:--j${jobs}}
        export NINJAFLAGS=${NINJAFLAGS:--j${jobs}}
        export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-${jobs}}
    fi
}

case ${EBUILD_PHASE} in
    configure|compile|test|install) distcc_configure_jobs ;;
esac
