#!/bin/sh

usage="\
Usage: $0 [OPTION]...

    --builddir  The build directory
    --debug     Build with symbols
"

append_cache_entry () {
    CMakeCacheEntries="$CMakeCacheEntries -D $1:$2=$3"
}

# set defaults
tool=cmake
sourcedir="$( cd "$( dirname "$0" )" && pwd )"
builddir=build
buildtype=Release
CMakeCacheEntries=""

# parse arguments
while [ $# -ne 0 ]; do
    case "$1" in
        -*=*) optarg=`echo "$1" | sed 's/[-_a-zA-Z0-9]*=//'` ;;
        *) optarg= ;;
    esac

    case "$1" in
        --help|-h)
            echo "${usage}" 1>&2
            exit 1
            ;;
        --builddir=*)
            builddir=$optarg
            ;;
        --define=*)
            CMakeCacheEntries="$CMakeCacheEntries -D$optarg"
            ;;
        --debug)
            buildtype=Debug
            ;;
        --debugrelease)
            buildtype=RelWithDebInfo
            ;;
        #--test)
        #    append_cache_entry ENABLE_TESTS BOOL true
        #    ;;
        *)
            echo "Invalid option '$1'.  Try $0 --help to see available options."
            exit 1
            ;;
    esac
    shift
done

if [ -d $builddir ]; then
    # If build directory exists, check if it has a CMake cache
    if [ -f $builddir/CMakeCache.txt ]; then
        # If the CMake cache exists, delete it so that this configuration
        # is not tainted by a previous one
        rm -f $builddir/CMakeCache.txt
    fi
else
    # Create build directory
    mkdir -p $builddir
fi

echo "Build Directory : $builddir"
echo "Source Directory: $sourcedir"
cd $builddir

$tool \
    -DCMAKE_BUILD_TYPE="$buildtype" \
    $CMakeCacheEntries "$sourcedir"
