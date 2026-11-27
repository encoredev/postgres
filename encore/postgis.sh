#!/usr/bin/env bash
# Build PostGIS for a build of the Postgres that Encore bundles, for one
# platform, as the optional Encore component postgres/postgis: a directory
# laid out as an installation's extensions are, which runs from wherever it's
# moved to:
#
#   encore/postgis.sh [--target <os>-<arch>] [--work-dir <dir>] [--no-check]
#                     [--postgres-source <dir>] --postgres-build <dir>
#                     --postgres <dir> <output dir>
#
# --postgres-build is the build directory encore/dist.sh built the Postgres in
# (its --build-dir), whose installation has the headers, pg_config and PGXS
# that PostGIS builds with, and whose deps/ has the libxml2 it links with.
# --postgres is that build's output, or the archive published of it, which
# the check runs PostGIS in and whose VERSION names the build.
# --postgres-source is the source it's a build of, the tree this script is in
# unless given, which is checked to be of the build's version and catalog
# version.
#
# On Windows, PostGIS and its libraries are built with MinGW-w64's GCC, in
# MSYS2's UCRT64 environment, as PostGIS's own builds for Windows are: it
# doesn't build with MSVC.  It builds against a MinGW build of the same
# source rather than the MSVC one, installed in the work directory for its
# headers, pg_config, PGXS and libpostgres.a, and loads in the MSVC one, whose
# postgres.exe exports the same names.  libxml2 is built too, at the version
# encore/deps.sh pins, and the Postgres build's libxml2.dll is the one loaded.
# Windows finds a module's libraries in PATH, not beside it, so Postgres needs
# lib/ in PATH there.
#
# The output holds:
#
#   share/extension/  the control and script files of postgis,
#                     postgis_topology, postgis_raster and postgis_sfcgal
#   lib/              their modules, and the libraries those load, which are
#                     found relative to them
#   LICENSES/         the licenses of PostGIS and of each library
#   VERSION           key=value lines: version (PostGIS's), postgres (the
#                     build's version, as in its VERSION), and the version of
#                     each library built, as in geos=3.15.1
#
# Postgres finds them when share/ is in extension_control_path and lib/ in
# dynamic_library_path.  PROJ's database is built into its library, and so
# are GDAL's data files where the compiler can (see build_gdal), so nothing
# needs to know where the output is.
#
# The libraries are each a release pinned below and checked against its
# SHA-256:
#
#   GEOS              geometry operations
#   PROJ, SQLite      ST_Transform; PROJ keeps its database in SQLite
#   json-c            GeoJSON
#   protobuf-c        ST_AsMVT and ST_AsGeobuf.  The code protoc-c generates
#                     from PostGIS's .proto files is in encore/postgis/, so
#                     that protoc-c and protobuf aren't built (see its README)
#   GDAL              postgis_raster
#   SFCGAL            postgis_sfcgal, with CGAL, Boost, GMP and MPFR
#   libxml2           GML and KML; the Postgres build's (encore/deps.sh)
#
# Unless given --no-check, a copy of the Postgres and of the output, each in
# another directory, creates every extension and runs a function of each
# library; for darwin-amd64 on Apple Silicon, only where Rosetta runs it.
#
# Needs curl, perl, make, cmake, ninja and pkg-config, and a C and C++
# compiler with C++17; on Linux, patchelf; on Windows, MSYS2's UCRT64
# toolchain, cmake, ninja and pkgconf, and its m4, bison, flex, perl, bzip2
# and xz.
set -eEuo pipefail
# What failed, for a command that says nothing when it does
trap 'echo "encore/postgis.sh: line $LINENO failed: $BASH_COMMAND" >&2' ERR

postgis_version=3.7.0
postgis_url=https://download.osgeo.org/postgis/source/postgis-$postgis_version.tar.gz
postgis_sha256=832bbf50f2b6f57fab053bc9676b3bfe641e5bf966cdcb28c7926f8d4b3c5e04

geos_version=3.15.1
geos_url=https://download.osgeo.org/geos/geos-$geos_version.tar.bz2
geos_sha256=8eb6dc2ea78b00c2f5109c6b0dba9e028c60bad42d7b1b0cc9952b12dc71f571

sqlite_version=3.54.1
sqlite_url=https://www.sqlite.org/2026/sqlite-autoconf-3540100.tar.gz
sqlite_sha256=926ef8d3eab93fbf6e06011901273eec076b25594a4e1cdadd50c987f4f6d64e

proj_version=9.9.0
proj_url=https://download.osgeo.org/proj/proj-$proj_version.tar.gz
proj_sha256=791a0610547eeabb17006cfd49cdbd2034f3240f47ed5e88a1031811f4e2bcf3

json_c_version=0.19
json_c_url=https://s3.amazonaws.com/json-c_releases/releases/json-c-$json_c_version.tar.gz
json_c_sha256=37ad0249902e301bd9052bf712e511fcc6acff4ecaad4b5900aad9ce564e26de

protobuf_c_version=1.5.2
protobuf_c_url=https://github.com/protobuf-c/protobuf-c/releases/download/v$protobuf_c_version/protobuf-c-$protobuf_c_version.tar.gz
protobuf_c_sha256=e2c86271873a79c92b58fef7ebf8de1aa0df4738347a8bd5d4e65a80a16d0d24

gdal_version=3.13.3
gdal_url=https://github.com/OSGeo/gdal/releases/download/v$gdal_version/gdal-$gdal_version.tar.gz
gdal_sha256=5e0c388d83da2d686cc00a40272882432cdb54edff43d4af173e532844a0a0ea

gmp_version=6.3.0
gmp_url=https://ftp.gnu.org/gnu/gmp/gmp-$gmp_version.tar.xz
gmp_sha256=a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898

mpfr_version=4.2.2
mpfr_url=https://ftp.gnu.org/gnu/mpfr/mpfr-$mpfr_version.tar.xz
mpfr_sha256=b67ba0383ef7e8a8563734e2e889ef5ec3c3b898a01d00fa0a6869ad81c6ce01

boost_version=1.92.0
boost_url=https://archives.boost.io/release/$boost_version/source/boost_${boost_version//./_}.tar.bz2
boost_sha256=5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c

cgal_version=6.2.1
cgal_url=https://github.com/CGAL/cgal/releases/download/v$cgal_version/CGAL-$cgal_version.tar.xz
cgal_sha256=b6be77c60765a8456335de991eeaf6ffec55256984e4a9ecc6a97c37bbfe85bf

nlohmann_json_version=3.12.0
nlohmann_json_url=https://github.com/nlohmann/json/releases/download/v$nlohmann_json_version/json.tar.xz
nlohmann_json_sha256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa

sfcgal_version=2.3.0
sfcgal_url=https://gitlab.com/sfcgal/SFCGAL/-/archive/v$sfcgal_version/SFCGAL-v$sfcgal_version.tar.gz
sfcgal_sha256=5f6aa1838e5ae31523ebf410cde0240b7a88d7e062b7ffff945e4fae2aaba0fa

# What CREATE EXTENSION creates from the output
extensions=(postgis postgis_topology postgis_raster postgis_sfcgal)

usage() {
	sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//' >&2
	exit 2
}

here=$(cd "$(dirname "$0")" && pwd)
target=
work=
check=1
pg_src=$here/..
pg_build=
pg_out=
out=
while [ $# -gt 0 ]; do
	case $1 in
		--target) target=$2; shift 2 ;;
		--work-dir) work=$2; shift 2 ;;
		--no-check) check=; shift ;;
		--postgres-source) pg_src=$2; shift 2 ;;
		--postgres-build) pg_build=$2; shift 2 ;;
		--postgres) pg_out=$2; shift 2 ;;
		-h|--help) usage ;;
		-*) echo "unknown option $1" >&2; usage ;;
		*) [ -z "$out" ] || usage; out=$1; shift ;;
	esac
done
[ -n "$out" ] && [ -n "$pg_build" ] && [ -n "$pg_out" ] || usage

case $(uname -s) in
	Darwin) host_os=darwin ;;
	Linux) host_os=linux ;;
	MINGW*|MSYS*|UCRT*)
		host_os=windows
		if [ "${MSYSTEM:-}" != UCRT64 ]; then
			echo "on Windows, run in MSYS2's UCRT64 environment, not ${MSYSTEM:-another}" >&2
			exit 1
		fi
		;;
	*) echo "unsupported host $(uname -s)" >&2; exit 1 ;;
esac
case $(uname -m) in
	arm64|aarch64) host_arch=arm64 ;;
	x86_64|amd64) host_arch=amd64 ;;
	*) echo "unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac
host=$host_os-$host_arch
target=${target:-$host}
os=${target%-*}
arch=${target#*-}
cross=
if [ "$target" != "$host" ]; then
	if [ "$target" != darwin-amd64 ] || [ "$host" != darwin-arm64 ]; then
		echo "can't build $target on $host" >&2
		exit 1
	fi
	cross=1
fi

pg_src=$(cd "$pg_src" && pwd)
pg_build=$(cd "$pg_build" && pwd)
pg_out=$(cd "$pg_out" && pwd)
pg_deps=$pg_build/deps
postgres_version=$(sed -n 's/^version=//p' "$pg_out/VERSION")
[ -n "$postgres_version" ] || { echo "$pg_out has no VERSION with a version" >&2; exit 1; }
# The source is of the build: its version and catalog version
src_version=$(sed -n "s/^  version: '\(.*\)',$/\1/p" "$pg_src/meson.build" | head -n 1)
src_catalog=$(awk '/^#define CATALOG_VERSION_NO/ {print $3}' "$pg_src/src/include/catalog/catversion.h")
if [ "$src_version" != "$(sed -n 's/^pg_version=//p' "$pg_out/VERSION")" ] ||
	[ "$src_catalog" != "$(sed -n 's/^catalog_version=//p' "$pg_out/VERSION")" ]; then
	echo "$pg_src is of PostgreSQL $src_version (catalog $src_catalog), $pg_out of another" >&2
	exit 1
fi
if [ "$os" = windows ]; then
	# The MinGW build's, which build_postgres_mingw installs
	pg_config=
else
	pg_config=$pg_build/install/pgsql/bin/pg_config
	[ -x "$pg_config" ] || { echo "$pg_build has no installation with a pg_config" >&2; exit 1; }
	[ -x "$pg_deps/bin/xml2-config" ] || { echo "$pg_build has no deps/ with libxml2" >&2; exit 1; }
	built_version=$("$pg_config" --version | awk '{print $2}')
	[ "$built_version" = "$src_version" ] || {
		echo "$pg_build is of PostgreSQL $built_version, $pg_out of $src_version" >&2
		exit 1
	}
fi

if [ -e "$out" ] && [ -n "$(ls -A "$out")" ]; then
	echo "$out is not empty" >&2
	exit 1
fi
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=${work:-${TMPDIR:-/tmp}/encore-postgis-build-$target}
mkdir -p "$work/downloads" "$work/build"
work=$(cd "$work" && pwd)
# The libraries are installed here, and PostGIS into stage/
prefix=$work/prefix
mkdir -p "$prefix/.built" "$prefix/bin" "$prefix/include" "$prefix/lib/pkgconfig"

# How many compilers run at once: JOBS, or one for each CPU.  CGAL's, in
# SFCGAL, take a GB or two of memory each.
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
export PKG_CONFIG_PATH=$prefix/lib/pkgconfig
cc=cc
cxx=c++
case $os in
	darwin)
		export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}
		case $arch in
			arm64) mac_arch=arm64 gnu_host=aarch64-apple-darwin ;;
			amd64) mac_arch=x86_64 gnu_host=x86_64-apple-darwin ;;
		esac
		cc="clang -arch $mac_arch"
		cxx="clang++ -arch $mac_arch"
		;;
	linux)
		case $arch in
			arm64) gnu_host=aarch64-unknown-linux-gnu ;;
			amd64) gnu_host=x86_64-pc-linux-gnu ;;
		esac
		;;
	windows)
		gnu_host=x86_64-w64-mingw32
		cc=gcc
		cxx=g++
		;;
esac
# While building, what's linked with the libraries finds them by their
# absolute paths; the output finds them relative to itself instead.  On
# Windows, by PATH, where DLLs are found.
ldflags="-L$prefix/lib -Wl,-rpath,$prefix/lib"
if [ "$os" = windows ]; then
	ldflags="-L$prefix/lib"
	export PATH=$prefix/bin:$PATH
fi

# Paths as the native tools take them: cmake, b2 and Postgres on Windows
native() {
	if [ "$os" = windows ]; then cygpath -m "$1"; else echo "$1"; fi
}

sha256() {
	if command -v sha256sum >/dev/null; then
		sha256sum "$1" | awk '{print $1}'
	else
		shasum -a 256 "$1" | awk '{print $1}'
	fi
}

# Whether <name> is built at <version>, and recording that it is
built() { [ "$(cat "$prefix/.built/$1" 2>/dev/null)" = "$2 $target" ]; }
mark_built() { echo "$2 $target" >"$prefix/.built/$1"; }

# Downloads <url>, checks it against <sha256> and unpacks it into a new,
# empty <work dir>/src/<name>
unpack() {
	local name=$1 url=$2 want=$3
	local file=$work/downloads/${url##*/}
	if [ ! -f "$file" ] || [ "$(sha256 "$file")" != "$want" ]; then
		curl -fsSL --retry 3 -o "$file.part" "$url"
		mv "$file.part" "$file"
	fi
	local got
	got=$(sha256 "$file")
	if [ "$got" != "$want" ]; then
		echo "$url has SHA-256 $got, not $want" >&2
		exit 1
	fi
	rm -rf "$work/src/$name"
	mkdir -p "$work/src/$name"
	# MSYS2 makes a symlink a copy of what it links to, which an archive can
	# have after it: then a second pass, when everything is there
	if [ "$os" = windows ]; then
		tar -xf "$file" -C "$work/src/$name" 2>/dev/null || tar -xf "$file" -C "$work/src/$name"
	else
		tar -xf "$file" -C "$work/src/$name"
	fi
}

# Runs a build's command, logging to <log>
logged() {
	local log=$1
	shift
	"$@" >>"$log" 2>&1 || { tail -n 50 "$log" >&2; exit 1; }
}

cmake_build() {
	local src=$1 build=$2
	shift 2
	local args=(
		-G Ninja
		-DCMAKE_BUILD_TYPE=Release
		-DCMAKE_INSTALL_PREFIX="$(native "$prefix")"
		-DCMAKE_INSTALL_LIBDIR=lib
		-DCMAKE_PREFIX_PATH="$(native "$prefix")"
		-DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON
		-DCMAKE_INSTALL_RPATH="$prefix/lib"
		-DBUILD_SHARED_LIBS=ON
		-DBUILD_TESTING=OFF
	)
	case $os in
		darwin) args+=(-DCMAKE_OSX_ARCHITECTURES=$mac_arch) ;;
		windows) args+=(-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++) ;;
	esac
	rm -rf "$build"
	cmake -S "$(native "$src")" -B "$(native "$build")" "${args[@]}" "$@" >"$build.log" 2>&1 ||
		{ cat "$build.log" >&2; exit 1; }
	cmake --build "$(native "$build")" --parallel "$jobs" >>"$build.log" 2>&1 ||
		{ tail -n 50 "$build.log" >&2; exit 1; }
	cmake --install "$(native "$build")" >>"$build.log" 2>&1
}

# A configure-and-make build of <src> in <work dir>/build/<name>
configure_build() {
	local name=$1 src=$2
	shift 2
	local build=$work/build/$name log=$work/build/$name.log
	rm -rf "$build"
	mkdir -p "$build"
	: >"$log"
	cd "$build"
	# On Windows, by a relative path, as srcdir: MinGW's GCC can't open an
	# MSYS2 path in a test program's #include "$srcdir/...", as GMP's have
	local configure=$src/configure
	[ "$os" = windows ] && configure=$(realpath --relative-to="$build" "$src")/configure
	logged "$log" env CC="$cc" CXX="$cxx" LDFLAGS="$ldflags" \
		"$configure" --prefix="$prefix" --libdir="$prefix/lib" "$@"
	logged "$log" make -j"$jobs"
	logged "$log" make install
	cd - >/dev/null
}

build_sqlite() {
	built sqlite "$sqlite_version soname" && return
	echo "building SQLite $sqlite_version"
	unpack sqlite "$sqlite_url" "$sqlite_sha256"
	# Its shell too, which PROJ's build makes its database with.  On Linux,
	# with its usual soname: without one, what links with it names it by its
	# path.
	local opts=(--host=$gnu_host --disable-static --disable-readline --disable-editline)
	[ "$os" = linux ] && opts+=(--soname=legacy)
	configure_build sqlite "$work/src/sqlite/sqlite-autoconf-3540100" "${opts[@]}"
	mark_built sqlite "$sqlite_version soname"
}

build_json_c() {
	built json-c $json_c_version && return
	echo "building json-c $json_c_version"
	unpack json-c "$json_c_url" "$json_c_sha256"
	cmake_build "$work/src/json-c/json-c-$json_c_version" "$work/build/json-c" \
		-DBUILD_STATIC_LIBS=OFF \
		-DBUILD_APPS=OFF \
		-DDISABLE_EXTRA_LIBS=ON
	mark_built json-c $json_c_version
}

build_protobuf_c() {
	built protobuf-c $protobuf_c_version && return
	echo "building protobuf-c $protobuf_c_version"
	unpack protobuf-c "$protobuf_c_url" "$protobuf_c_sha256"
	# The library alone; protoc-c would need protobuf
	configure_build protobuf-c "$work/src/protobuf-c/protobuf-c-$protobuf_c_version" \
		--host=$gnu_host --disable-static --disable-protoc
	mark_built protobuf-c $protobuf_c_version
}

build_geos() {
	built geos $geos_version && return
	echo "building GEOS $geos_version"
	unpack geos "$geos_url" "$geos_sha256"
	cmake_build "$work/src/geos/geos-$geos_version" "$work/build/geos" \
		-DBUILD_DOCUMENTATION=OFF \
		-DBUILD_GEOSOP=OFF \
		-DBUILD_BENCHMARKS=OFF
	mark_built geos $geos_version
}

build_proj() {
	built proj "$proj_version sqlite-$sqlite_version-soname" && return
	echo "building PROJ $proj_version"
	unpack proj "$proj_url" "$proj_sha256"
	# Building PROJ's database runs SQLite's shell, which a cross build can't
	# run its own of; every Mac has one.
	local shell=$prefix/bin/sqlite3
	case $os in
		darwin) shell=/usr/bin/sqlite3 ;;
		windows) shell=$(native "$prefix/bin/sqlite3.exe") ;;
	esac
	# Its database built into the library, and never looked for elsewhere.
	# The grids it downloads or reads from files, and so libtiff and curl,
	# aren't used.
	cmake_build "$work/src/proj/proj-$proj_version" "$work/build/proj" \
		-DEXE_SQLITE3="$shell" \
		-DSQLite3_INCLUDE_DIR="$(native "$prefix/include")" \
		-DSQLite3_LIBRARY="$(native "$prefix/lib/libsqlite3$(dylib)")" \
		-DEMBED_RESOURCE_FILES=ON \
		-DUSE_ONLY_EMBEDDED_RESOURCE_FILES=ON \
		-DENABLE_TIFF=OFF \
		-DENABLE_CURL=OFF \
		-DBUILD_APPS=OFF \
		-DBUILD_PROJSYNC=OFF
	mark_built proj "$proj_version sqlite-$sqlite_version-soname"
}

build_gmp() {
	built gmp "$gmp_version cxx" && return
	echo "building GMP $gmp_version"
	unpack gmp "$gmp_url" "$gmp_sha256"
	# For the generic CPU of the target, rather than the build machine's: on
	# x86-64, with code for each CPU that it picks from as it runs.  With its
	# C++ interface, libgmpxx, so that CGAL's exact numbers are mpq_class in
	# every one of SFCGAL's files (CGAL_USE_GMPXX), rather than Gmpq in some
	# and Boost's in others, which doesn't link.
	local opts=(--host=$gnu_host --enable-shared --disable-static --enable-cxx)
	[ "$arch" = amd64 ] && opts+=(--enable-fat)
	# As C17: its configure's tests aren't C23, which GCC 15 compiles by
	# default.  In CC, as CFLAGS would replace the ones it picks.
	local cc="$cc -std=gnu17"
	local src=$work/src/gmp/gmp-$gmp_version
	if [ "$os" = windows ]; then
		# As MSYS2 builds it for MinGW: without GMP's -pedantic, and with
		# gmp.h declaring its functions without dllimport, which GCC
		# finds through the import library
		sed -i 's/^#define __GMP_DECLSPEC_IMPORT  __declspec(__dllimport__)$/#define __GMP_DECLSPEC_IMPORT/' "$src/gmp-h.in"
		grep -q '^#define __GMP_DECLSPEC_IMPORT$' "$src/gmp-h.in" ||
			{ echo "GMP's gmp-h.in isn't as encore/postgis.sh expects" >&2; exit 1; }
		CFLAGS="-O2 -pipe -Wno-attributes -Wno-ignored-attributes" \
			configure_build gmp "$src" "${opts[@]}"
	else
		configure_build gmp "$src" "${opts[@]}"
	fi
	mark_built gmp "$gmp_version cxx"
}

build_mpfr() {
	built mpfr "$mpfr_version gmp-$gmp_version cxx" && return
	echo "building MPFR $mpfr_version"
	unpack mpfr "$mpfr_url" "$mpfr_sha256"
	configure_build mpfr "$work/src/mpfr/mpfr-$mpfr_version" \
		--host=$gnu_host --enable-shared --disable-static --with-gmp="$prefix"
	mark_built mpfr "$mpfr_version gmp-$gmp_version cxx"
}

build_boost() {
	built boost $boost_version && return
	echo "building Boost $boost_version"
	unpack boost "$boost_url" "$boost_sha256"
	local src=$work/src/boost/boost_${boost_version//./_} log=$work/build/boost.log
	# Static libraries, linked into SFCGAL, of only what it uses
	local opts=(--prefix="$(native "$prefix")" --build-dir="$(native "$work/build/boost")"
		--with-thread --with-serialization
		link=static runtime-link=shared threading=multi variant=release
		cxxstd=17 -j"$jobs")
	local toolset=gcc
	case $os in
		darwin)
			toolset=clang
			opts+=(cxxflags=-fPIC cxxflags="-arch $mac_arch" linkflags="-arch $mac_arch")
			[ "$arch" = amd64 ] && opts+=(architecture=x86 address-model=64)
			;;
		linux) opts+=(cxxflags=-fPIC) ;;
		# 64-bit only, and with its build tree's paths hashed: they're
		# longer than Windows' 260 characters otherwise
		windows) opts+=(target-os=windows threadapi=win32 architecture=x86 address-model=64 --hash) ;;
	esac
	: >"$log"
	cd "$src"
	logged "$log" ./bootstrap.sh --with-toolset=$toolset
	logged "$log" ./b2 toolset=$toolset "${opts[@]}" install
	cd - >/dev/null
	mark_built boost $boost_version
}

build_cgal() {
	built cgal $cgal_version && return
	echo "installing CGAL $cgal_version"
	unpack cgal "$cgal_url" "$cgal_sha256"
	# Headers only
	cmake_build "$work/src/cgal/CGAL-$cgal_version" "$work/build/cgal"
	mark_built cgal $cgal_version
}

build_nlohmann_json() {
	built nlohmann_json $nlohmann_json_version && return
	echo "installing nlohmann_json $nlohmann_json_version"
	unpack nlohmann_json "$nlohmann_json_url" "$nlohmann_json_sha256"
	cmake_build "$work/src/nlohmann_json/json" "$work/build/nlohmann_json" \
		-DJSON_BuildTests=OFF \
		-DJSON_Install=ON
	mark_built nlohmann_json $nlohmann_json_version
}

build_sfcgal() {
	local deps="$sfcgal_version cgal-$cgal_version boost-$boost_version gmp-$gmp_version mpfr-$mpfr_version"
	built sfcgal "$deps" && return
	echo "building SFCGAL $sfcgal_version"
	unpack sfcgal "$sfcgal_url" "$sfcgal_sha256"
	cmake_build "$work/src/sfcgal/SFCGAL-v$sfcgal_version" "$work/build/sfcgal" \
		-DSFCGAL_BUILD_TESTS=OFF \
		-DSFCGAL_BUILD_EXAMPLES=OFF \
		-DSFCGAL_BUILD_CLI=OFF \
		-DUSE_CCACHE=OFF \
		-DBoost_USE_STATIC_LIBS=ON \
		-DCGAL_Boost_USE_STATIC_LIBS=ON \
		-DBoost_NO_SYSTEM_PATHS=ON \
		-DBOOST_ROOT="$prefix"
	mark_built sfcgal "$deps"
}

build_gdal() {
	built gdal "$gdal_version proj-$proj_version" && return
	echo "building GDAL $gdal_version"
	unpack gdal "$gdal_url" "$gdal_sha256"
	local src=$work/src/gdal/gdal-$gdal_version
	# The raster drivers that need no library beyond GDAL's own copies of
	# libtiff, libgeotiff, libpng, libjpeg, giflib and zlib: GTiff and the
	# other drivers GDAL always has, PNG, JPEG, GIF, AAIGrid and the raw
	# formats.
	#
	# Its data files are built into the library where the compiler has C23's
	# #embed: clang from 19, Apple's clang and MinGW's GCC 15, but not
	# manylinux's GCC 14.  GDAL tries it only with clang from 19 and GCC from
	# 15 by their version numbers, which Apple's clang has others of, so that
	# test is left to compiling, as here.  Without #embed, GDAL goes without
	# its data files, which none of these drivers need.
	local embed=OFF
	printf 'static const unsigned char d[] = {\n#embed __FILE__\n};\nint main(void) { return !d[0]; }\n' >"$work/build/embed.c"
	$cc -std=c23 -c "$work/build/embed.c" -o "$work/build/embed.o" 2>/dev/null && embed=ON
	perl -0pi.orig -e 's/CMAKE_VERSION VERSION_GREATER_EQUAL 3\.21 AND\s*\(\(CMAKE_C_COMPILER_ID.*?VERSION_GREATER_EQUAL 19\.0\)\)\)/CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)/s' "$src/gdal.cmake"
	grep -q 'if (CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)$' "$src/gdal.cmake" ||
		{ echo "GDAL's #embed test isn't as encore/postgis.sh expects" >&2; exit 1; }
	local opts=(
		-DGDAL_USE_EXTERNAL_LIBS=OFF
		-DGDAL_USE_ZLIB_INTERNAL=ON
		-DGDAL_USE_TIFF_INTERNAL=ON
		-DGDAL_USE_GEOTIFF_INTERNAL=ON
		-DGDAL_USE_PNG_INTERNAL=ON
		-DGDAL_USE_JPEG_INTERNAL=ON
		-DGDAL_USE_GIF_INTERNAL=ON
		-DGDAL_USE_JSONC_INTERNAL=ON
		-DGDAL_BUILD_OPTIONAL_DRIVERS=OFF
		-DOGR_BUILD_OPTIONAL_DRIVERS=OFF
		-DGDAL_ENABLE_DRIVER_PNG=ON
		-DGDAL_ENABLE_DRIVER_JPEG=ON
		-DGDAL_ENABLE_DRIVER_GIF=ON
		-DGDAL_ENABLE_DRIVER_AAIGRID=ON
		-DGDAL_ENABLE_DRIVER_RAW=ON
		-DEMBED_RESOURCE_FILES=$embed
		-DUSE_ONLY_EMBEDDED_RESOURCE_FILES=$embed
		-DBUILD_APPS=OFF
		-DBUILD_PYTHON_BINDINGS=OFF
		-DBUILD_JAVA_BINDINGS=OFF
		-DBUILD_CSHARP_BINDINGS=OFF
	)
	cmake_build "$src" "$work/build/gdal" "${opts[@]}"
	mark_built gdal "$gdal_version proj-$proj_version"
	echo "gdal_embedded_data=$embed" >"$prefix/.gdal-embed"
}

# On Windows: libxml2 at the version encore/deps.sh pins, built with MinGW for
# PostGIS to link with.  The module loads the Postgres build's libxml2.dll,
# which the server has loaded already.
build_libxml2() {
	local libxml2_version libxml2_url libxml2_sha256
	eval "$(grep -E '^libxml2_(version|url|sha256)=' "$pg_src/encore/deps.sh")"
	built libxml2 $libxml2_version && return
	echo "building libxml2 $libxml2_version"
	unpack libxml2 "$libxml2_url" "$libxml2_sha256"
	cmake_build "$work/src/libxml2/libxml2-$libxml2_version" "$work/build/libxml2" \
		-DLIBXML2_WITH_ICONV=OFF \
		-DLIBXML2_WITH_ICU=OFF \
		-DLIBXML2_WITH_LZMA=OFF \
		-DLIBXML2_WITH_ZLIB=OFF \
		-DLIBXML2_WITH_PROGRAMS=OFF \
		-DLIBXML2_WITH_PYTHON=OFF \
		-DLIBXML2_WITH_TESTS=OFF
	mark_built libxml2 $libxml2_version
}

# On Windows: a MinGW build of the Postgres source, with autoconf as
# PostGIS's Windows builds have it, installed in the prefix for its headers,
# pg_config, PGXS and libpostgres.a, the import library of postgres.exe's
# exports.  Without the libraries the MSVC build links with: none of them is
# in what an extension's built with.
build_postgres_mingw() {
	local key="$src_version-$src_catalog $(git -C "$pg_src" rev-parse HEAD 2>/dev/null || echo source)"
	pg_config=$prefix/pgsql/bin/pg_config
	built postgres-mingw "$key" && return
	echo "building PostgreSQL $src_version with MinGW, to build against"
	local build=$work/build/postgres log=$work/build/postgres.log
	rm -rf "$build" "$prefix/pgsql"
	mkdir -p "$build"
	: >"$log"
	cd "$build"
	logged "$log" "$pg_src/configure" --host=$gnu_host --prefix="$prefix/pgsql" \
		--without-icu --without-readline --without-zlib --without-llvm \
		--disable-nls --disable-tap-tests
	logged "$log" make -j"$jobs"
	logged "$log" make install
	cd - >/dev/null
	mark_built postgres-mingw "$key"
}

# The suffix of a shared library, as it's linked with: on Windows, its import
# library's
dylib() {
	case $os in
		darwin) echo .dylib ;;
		linux) echo .so ;;
		windows) echo .dll.a ;;
	esac
}

build_postgis() {
	echo "building PostGIS $postgis_version"
	unpack postgis "$postgis_url" "$postgis_sha256"
	local src=$work/src/postgis/postgis-$postgis_version log=$work/build/postgis.log
	# protoc-c's code, made newer than the .proto files so that make doesn't
	# make it again, and a protoc-c for configure to find that says so if it
	# does
	cp "$here"/postgis/*.pb-c.[ch] "$src/postgis/"
	touch "$src"/postgis/*.pb-c.[ch]
	mkdir -p "$work/tools"
	cat >"$work/tools/protoc-c" <<-'EOF'
		#!/bin/sh
		echo "protoc-c isn't built: its code is in encore/postgis/" >&2
		exit 1
	EOF
	chmod +x "$work/tools/protoc-c"
	local xml2=$pg_deps
	[ "$os" = windows ] && xml2=$prefix
	local opts=(
		--host=$gnu_host
		--with-pgconfig="$pg_config"
		--with-xml2config="$xml2/bin/xml2-config"
		--with-geosconfig="$prefix/bin/geos-config"
		--with-projdir="$prefix"
		--with-jsondir="$prefix"
		--with-protobufdir="$prefix"
		--with-gdalconfig="$prefix/bin/gdal-config"
		--with-sfcgal="$prefix/bin/sfcgal-config"
		--with-raster
		--with-topology
		--without-gui
		--without-phony-revision
	)
	local flags=$ldflags
	[ "$os" = windows ] || flags="$flags -L$pg_deps/lib -Wl,-rpath,$pg_deps/lib"
	: >"$log"
	cd "$src"
	logged "$log" env PATH="$work/tools:$PATH" CC="$cc" CXX="$cxx" LDFLAGS="$flags" \
		./configure "${opts[@]}"
	logged "$log" make -j"$jobs"
	# Into a stage, rather than the installation it's built against.  On
	# Windows, that's the MinGW one in the work directory, whose Windows
	# paths a stage can't prefix.
	rm -rf "$work/stage"
	if [ "$os" = windows ]; then
		logged "$log" make install
	else
		logged "$log" make install DESTDIR="$work/stage"
	fi
	cd - >/dev/null
}

if [ "$os" = windows ]; then
	build_libxml2
	build_postgres_mingw
fi
build_sqlite
build_json_c
build_protobuf_c
build_geos
build_proj
build_gmp
build_mpfr
build_boost
build_cgal
build_nlohmann_json
build_sfcgal
build_gdal
build_postgis

# The extensions' files, from where PostGIS installed them under the stage
if [ "$os" = windows ]; then
	pkglibdir=$(cygpath -u "$("$pg_config" --pkglibdir)")
	sharedir=$(cygpath -u "$("$pg_config" --sharedir)")
else
	pkglibdir=$work/stage$("$pg_config" --pkglibdir)
	sharedir=$work/stage$("$pg_config" --sharedir)
fi
mkdir -p "$out/lib" "$out/share/extension" "$out/LICENSES"
for ext in "${extensions[@]}"; do
	[ -f "$sharedir/extension/$ext.control" ] ||
		{ echo "PostGIS installed no extension $ext" >&2; exit 1; }
	cp "$sharedir/extension/$ext.control" "$sharedir/extension/$ext"--*.sql "$out/share/extension/"
done
for f in "$pkglibdir"/postgis*; do
	cp "$f" "$out/lib/"
done

# The libraries, each under the name it's loaded by: what PostGIS links with
# and what those link with, from the prefix and libxml2 from the Postgres
# build's.  Boost is linked into SFCGAL.
copy_libs() {
	local dir=$1 f name
	case $os in
		darwin)
			for f in "$dir"/lib/*.dylib; do
				[ -L "$f" ] && continue
				name=$(basename "$(otool -D "$f" | awk 'NR == 2')")
				cp "$f" "$out/lib/$name"
			done
			;;
		linux)
			for f in "$dir"/lib/*.so*; do
				[ -L "$f" ] && continue
				name=$(patchelf --print-soname "$f" 2>/dev/null) || continue
				if [ -n "$name" ]; then
					cp "$f" "$out/lib/$name"
				fi
			done
			;;
		windows)
			# Beside the modules, though Windows looks for them in PATH.  Not
			# those the Postgres build has, which the server has loaded.
			for f in "$dir"/bin/*.dll; do
				[ -e "$pg_out/bin/$(basename "$f")" ] || cp "$f" "$out/lib/"
			done
			;;
	esac
}
copy_libs "$prefix"
case $os in
	darwin) cp "$pg_deps/lib/libxml2.16.dylib" "$out/lib/" ;;
	linux) cp "$pg_deps/lib/$(patchelf --print-soname "$pg_deps/lib/libxml2.so")" "$out/lib/" ;;
	windows)
		# GCC's C++ and runtime libraries, and libiconv, from MSYS2's
		for f in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll libiconv-2.dll; do
			cp "$MSYSTEM_PREFIX/bin/$f" "$out/lib/"
		done
		;;
esac

# Drops each library in lib/ that no module loads, directly or through
# another library
needed_by() {
	case $os in
		darwin) otool -L "$1" | awk 'NR > 1 {print $1}' | sed 's|.*/||' ;;
		linux) patchelf --print-needed "$1" ;;
		windows) objdump -p "$1" | sed -n 's/^[[:space:]]*DLL Name: //p' ;;
	esac
}
keep=" "
queue=$(cd "$out/lib" && ls postgis*)
while [ -n "$queue" ]; do
	next=
	for f in $queue; do
		case $keep in *" $f "*) continue ;; esac
		keep="$keep$f "
		for dep in $(needed_by "$out/lib/$f"); do
			[ -e "$out/lib/$dep" ] && next="$next $dep"
		done
	done
	queue=$next
done
for f in "$out"/lib/*; do
	case $keep in *" $(basename "$f") "*) ;; *) rm "$f" ;; esac
done

# Strip, and make each library and module look for the libraries it loads
# beside itself.  Then check that nothing refers to a library that's neither
# in the output nor the system's.
case $os in
	darwin)
		for f in "$out"/lib/*; do
			strip -x "$f"
			otool -L "$f" | awk 'NR > 1 {print $1}' |
				while read -r dep; do
					case $dep in @loader_path/*|/usr/lib/*|/System/*) continue ;; esac
					[ -e "$out/lib/$(basename "$dep")" ] &&
						install_name_tool -change "$dep" "@loader_path/$(basename "$dep")" "$f" 2>/dev/null
				done
			case $f in
				*.dylib)
					case $(basename "$f") in
						postgis*) ;;
						*) install_name_tool -id "@rpath/$(basename "$f")" "$f" 2>/dev/null ;;
					esac
					;;
			esac
			# The build's own rpaths
			otool -l "$f" | awk '/LC_RPATH/ {r = 1} r && $1 == "path" {print $2; r = 0}' |
				while read -r rpath; do
					install_name_tool -delete_rpath "$rpath" "$f" 2>/dev/null
				done
			codesign -s - -f "$f" 2>/dev/null
		done
		# A function, as bash 3.2 can't parse a case in $(...)
		unresolved() {
			local f id dep
			for f in "$out"/lib/*; do
				id=$(otool -D "$f" | awk 'NR > 1')
				otool -L "$f" | awk -v id="$id" 'NR > 1 && $1 != id {print $1}' |
					while read -r dep; do
						case $dep in
							/usr/lib/*|/System/*) ;;
							@loader_path/*) [ -e "$out/lib/${dep##*/}" ] || echo "$dep" ;;
							*) echo "$dep" ;;
						esac
					done
			done
		}
		left=$(unresolved)
		;;
	linux)
		for f in "$out"/lib/*; do
			strip --strip-unneeded "$f"
			patchelf --set-rpath '$ORIGIN' "$f"
		done
		# What the manylinux policy lets a library take from the system
		system='^(libc|libm|libdl|libpthread|librt|libz|libstdc\+\+|libgcc_s|ld-linux-.*)\.so'
		left=$(for f in "$out"/lib/*; do
			patchelf --print-needed "$f" | while read -r lib; do
				[ -e "$out/lib/$lib" ] || echo "$lib" | grep -Ev "$system" || true
			done
		done)
		;;
	windows)
		for f in "$out"/lib/*; do
			strip --strip-unneeded "$f"
		done
		# Each DLL a module or library loads is in the output, is the Postgres
		# build's (postgres.exe's own exports, libxml2.dll), or is Windows'
		system32=$(cygpath -u "$SYSTEMROOT")/System32
		# A function, as bash 3.2 can't parse a case in $(...)
		unresolved() {
			local f lib
			for f in "$out"/lib/*; do
				needed_by "$f" | tr -d '\r' | while read -r lib; do
					case $lib in api-ms-win-*) continue ;; esac
					[ -e "$out/lib/$lib" ] || [ -e "$pg_out/bin/$lib" ] ||
						[ -e "$system32/$lib" ] || [ -e "$system32/$(echo "$lib" | tr 'A-Z' 'a-z')" ] ||
						echo "$lib"
				done
			done
		}
		left=$(unresolved)
		;;
esac
if [ -n "$left" ]; then
	echo "the output loads libraries that aren't in it:" >&2
	echo "$left" | sort -u >&2
	exit 1
fi

# The licenses
license() {
	local name=$1 src=$2
	shift 2
	local f
	mkdir -p "$out/LICENSES/$name"
	for f in "$@"; do
		cp -R "$src/$f" "$out/LICENSES/$name/"
	done
}
s=$work/src
license postgis "$s/postgis/postgis-$postgis_version" COPYING LICENSE.TXT
license geos "$s/geos/geos-$geos_version" COPYING
license proj "$s/proj/proj-$proj_version" COPYING
license sqlite "$s/sqlite/sqlite-autoconf-3540100" README.txt
license json-c "$s/json-c/json-c-$json_c_version" COPYING
license protobuf-c "$s/protobuf-c/protobuf-c-$protobuf_c_version" LICENSE
license gdal "$s/gdal/gdal-$gdal_version" LICENSE.TXT
license gmp "$s/gmp/gmp-$gmp_version" COPYING.LESSERv3 COPYINGv2 COPYINGv3
license mpfr "$s/mpfr/mpfr-$mpfr_version" COPYING.LESSER
license boost "$s/boost/boost_${boost_version//./_}" LICENSE_1_0.txt
license cgal "$s/cgal/CGAL-$cgal_version" LICENSE LICENSE.GPL LICENSE.LGPL
license sfcgal "$s/sfcgal/SFCGAL-v$sfcgal_version" LICENSE
if [ "$os" = windows ]; then
	xml2_src=$(ls -d "$s"/libxml2/libxml2-* | head -n 1)
	# The licenses of the MSYS2 packages the output has DLLs of: GCC's
	# runtime and libiconv
	for f in "$out"/lib/*.dll; do
		[ -e "$prefix/bin/$(basename "$f")" ] && continue
		case $(basename "$f") in postgis*) continue ;; esac
		pkg=$(pacman -Qqo "$MSYSTEM_PREFIX/bin/$(basename "$f")" 2>/dev/null) || continue
		files=$(pacman -Qlq "$pkg" | grep '/share/licenses/.*[^/]$' || true)
		[ -n "$files" ] || { echo "MSYS2's $pkg, which has $(basename "$f"), has no license files" >&2; exit 1; }
		mkdir -p "$out/LICENSES/$pkg"
		echo "$files" | while read -r file; do cp "$file" "$out/LICENSES/$pkg/"; done
	done
else
	xml2_src=$(ls -d "$pg_build"/deps.work/src/libxml2/libxml2-* 2>/dev/null | head -n 1)
fi
[ -n "$xml2_src" ] || { echo "no libxml2 source to take its license from" >&2; exit 1; }
license libxml2 "$xml2_src" Copyright

cat >"$out/VERSION" <<EOF
version=$postgis_version
postgres=$postgres_version
EOF
for f in "$prefix"/.built/*; do
	echo "$(basename "$f")=$(awk '{print $1}' "$f")" >>"$out/VERSION"
done
cat "$prefix/.gdal-embed" >>"$out/VERSION"

if [ -n "$check" ] && [ -n "$cross" ] && ! "$pg_out/bin/postgres" --version >/dev/null 2>&1; then
	echo "not checking the $target build, which this machine can't run"
	check=
fi
if [ -n "$check" ]; then
	# From other directories, to catch anything still found by its path
	tmp=$(mktemp -d /tmp/encore-postgis-check.XXXXXX)
	trap 'PATH=$tmp/pgsql/bin pg_ctl -D "$(native "$tmp/data")" -m immediate stop >/dev/null 2>&1 || true; rm -rf "$tmp"' EXIT
	cp -R "$pg_out" "$tmp/pgsql"
	cp -R "$out" "$tmp/postgis"
	# On Windows, PATH finds the modules' libraries, and the server listens
	# on TCP; the lists are separated by ;
	export PATH=$tmp/pgsql/bin:$PATH
	# pg_ctl runs the server through CMD.EXE there, which quotes with " (and
	# leaves $ alone), rather than sh, which would expand $system in one
	if [ "$os" = windows ]; then
		export PATH=$tmp/postgis/lib:$PATH
		conn=(-h 127.0.0.1 -p 54331)
		server_opts="-p 54331"
		sep=';'
		q='"'
	else
		conn=(-h "$tmp")
		server_opts="-c listen_addresses='' -k $tmp"
		sep=:
		q="'"
	fi
	initdb -U postgres -A trust -E UTF8 --locale C -D "$(native "$tmp/data")" >"$tmp/initdb.log" ||
		{ cat "$tmp/initdb.log" >&2; exit 1; }
	pg_ctl -D "$(native "$tmp/data")" -l "$(native "$tmp/server.log")" -w start -o "$server_opts \
		-c extension_control_path=$q\$system$sep$(native "$tmp/postgis/share")$q \
		-c dynamic_library_path=$q\$libdir$sep$(native "$tmp/postgis/lib")$q" >/dev/null ||
		{ cat "$tmp/server.log" >&2; exit 1; }
	# The SQL on stdin, which MSYS2 passes on unchanged
	sql() { echo "$1" | psql "${conn[@]}" -U postgres -X -q -A -t -v ON_ERROR_STOP=1 -d postgres -f - | tr -d '\r'; }
	expect() {
		local got
		got=$(sql "$1")
		[ "$got" = "$2" ] || { echo "$1: got '$got', not '$2'" >&2; exit 1; }
	}
	for ext in "${extensions[@]}"; do
		sql "CREATE EXTENSION $ext CASCADE" >/dev/null
	done
	# Each library: GEOS, PROJ's database, json-c, libxml2, protobuf-c,
	# GDAL's drivers, SFCGAL and topology
	expect "SELECT ST_Area(ST_Buffer(ST_MakePoint(0, 0), 1, 64)) BETWEEN 3.14 AND 3.15" t
	expect "SELECT round(ST_X(ST_Transform(ST_SetSRID(ST_MakePoint(15, 0), 4326), 32633)))" 500000
	expect "SELECT ST_AsText(ST_GeomFromGeoJSON('{\"type\":\"Point\",\"coordinates\":[1,2]}'))" "POINT(1 2)"
	expect "SELECT ST_AsText(ST_GeomFromGML('<gml:Point><gml:coordinates>1,2</gml:coordinates></gml:Point>'))" "POINT(1 2)"
	expect "SELECT length(ST_AsMVT(q)) > 0 FROM (SELECT ST_AsMVTGeom(ST_MakePoint(1, 1), ST_MakeEnvelope(0, 0, 4096, 4096)) AS geom) q" t
	expect "SET postgis.gdal_enabled_drivers = 'ENABLE_ALL'; SELECT length(ST_AsPNG(r)) > 0 AND length(ST_AsTIFF(r)) > 0 AND length(ST_AsJPEG(r)) > 0 FROM (SELECT ST_AsRaster(ST_Buffer(ST_MakePoint(0, 0), 10), 10, 10, '8BUI') AS r) q" t
	expect "SELECT ST_SRID(ST_Transform(ST_AsRaster(ST_SetSRID(ST_Buffer(ST_MakePoint(15, 0), 1), 4326), 10, 10, '8BUI'), 32633))" 32633
	expect "SELECT CG_Volume(CG_MakeSolid(CG_Extrude(ST_MakeEnvelope(0, 0, 1, 1), 0, 0, 1)))" 1
	expect "SELECT topology.CreateTopology('t', 0) > 0" t
	sql "SELECT postgis_full_version()"
	pg_ctl -D "$(native "$tmp/data")" -m fast -w stop >/dev/null
	echo "checked: every extension, and a function of each library, from $tmp/postgis"
fi

echo "built PostGIS $postgis_version for $postgres_version on $target in $out"
