#!/usr/bin/env bash
# Build the libraries that the Postgres Encore bundles links with, for one
# platform, into a prefix for encore/dist.sh to build against:
#
#   encore/deps.sh [--target <os>-<arch>] [--work-dir <dir>] <prefix>
#
# They are, each a release pinned below and checked against its SHA-256:
#
#   OpenSSL           pgcrypto, sslinfo and SSL connections
#   libxml2, libxslt  the xml type's functions, and xml2
#   lz4, zstd         lz4 and zstd compression
#   ICU               ICU collations
#   libuuid           uuid-ossp; on Linux util-linux's libuuid, on macOS the
#                     C library's, on Windows encore/uuid-win32
#
# The libraries are shared ones, which dist.sh copies into the bundle.  On
# Windows, ICU is ICU's own build for it rather than built from source.
#
# A library that <prefix>/.built records as built at the same version isn't
# built again.  Downloads are kept in <work dir>/downloads; the work
# directory is <prefix>.work unless given.
#
# Needs curl, perl, cmake and ninja, and make on Linux and macOS; on Windows,
# Git Bash with the MSVC environment set up, as dist.sh does.
set -euo pipefail

openssl_version=3.5.9
openssl_url=https://github.com/openssl/openssl/releases/download/openssl-$openssl_version/openssl-$openssl_version.tar.gz
openssl_sha256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a

libxml2_version=2.15.4
libxml2_url=https://download.gnome.org/sources/libxml2/2.15/libxml2-$libxml2_version.tar.xz
libxml2_sha256=98087fd181d9070724f3fbc65c7377db03038eb92bd882374daff44940138821

libxslt_version=1.1.45
libxslt_url=https://download.gnome.org/sources/libxslt/1.1/libxslt-$libxslt_version.tar.xz
libxslt_sha256=9acfe68419c4d06a45c550321b3212762d92f41465062ca4ea19e632ee5d216e

lz4_version=1.10.0
lz4_url=https://github.com/lz4/lz4/releases/download/v$lz4_version/lz4-$lz4_version.tar.gz
lz4_sha256=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b

zstd_version=1.5.7
zstd_url=https://github.com/facebook/zstd/releases/download/v$zstd_version/zstd-$zstd_version.tar.gz
zstd_sha256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3

icu_version=78.3
icu_url=https://github.com/unicode-org/icu/releases/download/release-$icu_version/icu4c-$icu_version-sources.tgz
icu_sha256=3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0
icu_windows_url=https://github.com/unicode-org/icu/releases/download/release-$icu_version/icu4c-$icu_version-Win64-MSVC2022.zip
icu_windows_sha256=446b671f9437227daa79e221d4521d75793f9ecd65ac44c06e34dd848f201ac2

util_linux_version=2.42.4
util_linux_url=https://www.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-$util_linux_version.tar.xz
util_linux_sha256=fbd62a100ab7bb8746ba0661255c3c48185b1e9021507c624da01fbc696330ec

usage() {
	sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//' >&2
	exit 2
}

here=$(cd "$(dirname "$0")" && pwd)
target=
work=
prefix=
while [ $# -gt 0 ]; do
	case $1 in
		--target) target=$2; shift 2 ;;
		--work-dir) work=$2; shift 2 ;;
		-h|--help) usage ;;
		-*) echo "unknown option $1" >&2; usage ;;
		*) [ -z "$prefix" ] || usage; prefix=$1; shift ;;
	esac
done
[ -n "$prefix" ] || usage

case $(uname -s) in
	Darwin) host_os=darwin ;;
	Linux) host_os=linux ;;
	MINGW*|MSYS*) host_os=windows ;;
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

mkdir -p "$prefix"
prefix=$(cd "$prefix" && pwd)
work=${work:-$prefix.work}
mkdir -p "$work/downloads" "$work/build"
work=$(cd "$work" && pwd)
mkdir -p "$prefix/.built" "$prefix/bin" "$prefix/include" "$prefix/lib/pkgconfig"

# Paths as the native tools take them
native() {
	if [ "$os" = windows ]; then cygpath -m "$1"; else echo "$1"; fi
}

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-4}")

case $os in
	darwin)
		export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}
		case $arch in
			arm64) mac_arch=arm64 ;;
			amd64) mac_arch=x86_64 ;;
		esac
		;;
	windows)
		# MSVC's link and a native Perl, not Git Bash's link and MSYS Perl,
		# and Windows' own tar, which unpacks .zip too
		PATH=$(echo "$PATH" | tr ':' '\n' |
			awk '/^(\/[a-z]\/Program Files\/Git)?(\/usr)?\/bin$|\/mingw64\/bin$|^\/usr\/local\/bin$/ {tail = tail ":" $0; next}
				{head = head (head ? ":" : "") $0}
				END {print head tail}')
		tar() { "$(cygpath -u "$SYSTEMROOT")/System32/tar.exe" "$@"; }
		;;
esac

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
		curl -fsSL --retry 3 -o "$(native "$file.part")" "$url"
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
	tar -xf "$(native "$file")" -C "$(native "$work/src/$name")"
}

# A pkg-config file for a library that installs none
write_pc() {
	local name=$1 version=$2 cflags=$3 libs=$4 requires=${5:-}
	cat >"$prefix/lib/pkgconfig/$name.pc" <<-EOF
		prefix=$(native "$prefix")
		libdir=\${prefix}/lib
		includedir=\${prefix}/include

		Name: $name
		Description: $name, for the Postgres Encore bundles
		Version: $version
		Requires: $requires
		Cflags: $cflags
		Libs: $libs
	EOF
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
		-DBUILD_SHARED_LIBS=ON
	)
	case $os in
		darwin) args+=(-DCMAKE_OSX_ARCHITECTURES=$mac_arch) ;;
		windows) args+=(-DCMAKE_C_COMPILER=cl) ;;
	esac
	rm -rf "$build"
	cmake -S "$(native "$src")" -B "$(native "$build")" "${args[@]}" "$@" >"$build.log" 2>&1 ||
		{ cat "$build.log" >&2; exit 1; }
	cmake --build "$(native "$build")" --parallel "$jobs" >>"$build.log" 2>&1 ||
		{ tail -n 50 "$build.log" >&2; exit 1; }
	cmake --install "$(native "$build")" >>"$build.log" 2>&1
}

# Runs a configure-and-make build's command, logging to <log>
logged() {
	local log=$1
	shift
	"$@" >>"$log" 2>&1 || { tail -n 50 "$log" >&2; exit 1; }
}

build_openssl() {
	built openssl $openssl_version && return
	echo "building OpenSSL $openssl_version"
	unpack openssl "$openssl_url" "$openssl_sha256"
	local src=$work/src/openssl/openssl-$openssl_version log=$work/openssl.log
	local config
	case $target in
		darwin-arm64) config=darwin64-arm64-cc ;;
		darwin-amd64) config=darwin64-x86_64-cc ;;
		linux-amd64) config=linux-x86_64 ;;
		linux-arm64) config=linux-aarch64 ;;
		windows-amd64) config=VC-WIN64A ;;
	esac
	# no-module builds the legacy provider into libcrypto, so that nothing
	# is loaded from a directory of the build machine's; no-autoload-config
	# keeps it from reading an openssl.cnf it wasn't built for.
	local opts=(shared no-apps no-docs no-tests no-module no-autoload-config
		--prefix="$(native "$prefix")" --libdir=lib)
	# The assembly needs NASM on Windows
	[ "$os" = windows ] && opts+=(no-asm)
	: >"$log"
	cd "$src"
	logged "$log" perl Configure "$config" "${opts[@]}"
	if [ "$os" = windows ]; then
		logged "$log" nmake
		logged "$log" nmake install_sw
		# OpenSSL writes no pkg-config files on Windows
		write_pc openssl $openssl_version "-I\${includedir}" "-L\${libdir} -llibssl -llibcrypto"
	else
		logged "$log" make -j"$jobs"
		logged "$log" make install_sw
	fi
	cd - >/dev/null
	mark_built openssl $openssl_version
}

build_libxml2() {
	built libxml2 $libxml2_version && return
	echo "building libxml2 $libxml2_version"
	unpack libxml2 "$libxml2_url" "$libxml2_sha256"
	# Postgres hands libxml2 UTF-8 only.  iconv, for other encodings in
	# documents' declarations and xsl:output, is the C library's on Linux
	# and macOS; Windows has none.
	local iconv=ON
	[ "$os" = windows ] && iconv=OFF
	cmake_build "$work/src/libxml2/libxml2-$libxml2_version" "$work/build/libxml2" \
		-DLIBXML2_WITH_ICONV=$iconv \
		-DLIBXML2_WITH_ICU=OFF \
		-DLIBXML2_WITH_LZMA=OFF \
		-DLIBXML2_WITH_ZLIB=OFF \
		-DLIBXML2_WITH_PROGRAMS=OFF \
		-DLIBXML2_WITH_PYTHON=OFF \
		-DLIBXML2_WITH_TESTS=OFF
	mark_built libxml2 $libxml2_version
}

build_libxslt() {
	built libxslt "$libxslt_version libxml2-$libxml2_version" && return
	echo "building libxslt $libxslt_version"
	unpack libxslt "$libxslt_url" "$libxslt_sha256"
	cmake_build "$work/src/libxslt/libxslt-$libxslt_version" "$work/build/libxslt" \
		-DLIBXSLT_WITH_PROFILER=OFF \
		-DLIBXSLT_WITH_PROGRAMS=OFF \
		-DLIBXSLT_WITH_PYTHON=OFF \
		-DLIBXSLT_WITH_TESTS=OFF
	mark_built libxslt "$libxslt_version libxml2-$libxml2_version"
}

build_lz4() {
	built lz4 $lz4_version && return
	echo "building lz4 $lz4_version"
	unpack lz4 "$lz4_url" "$lz4_sha256"
	cmake_build "$work/src/lz4/lz4-$lz4_version/build/cmake" "$work/build/lz4" \
		-DBUILD_STATIC_LIBS=OFF \
		-DLZ4_BUILD_CLI=OFF
	mark_built lz4 $lz4_version
}

build_zstd() {
	built zstd $zstd_version && return
	echo "building zstd $zstd_version"
	unpack zstd "$zstd_url" "$zstd_sha256"
	cmake_build "$work/src/zstd/zstd-$zstd_version/build/cmake" "$work/build/zstd" \
		-DZSTD_BUILD_STATIC=OFF \
		-DZSTD_BUILD_PROGRAMS=OFF \
		-DZSTD_BUILD_TESTS=OFF
	mark_built zstd $zstd_version
}

build_icu() {
	built icu $icu_version && return
	echo "installing ICU $icu_version"
	if [ "$os" = windows ]; then
		unpack icu "$icu_windows_url" "$icu_windows_sha256"
		local dist=$work/src/icu major=${icu_version%%.*} lib
		cp -R "$dist/include/unicode" "$prefix/include/"
		for lib in uc in dt; do
			cp "$dist/lib64/icu$lib.lib" "$prefix/lib/"
			cp "$dist/bin64/icu$lib$major.dll" "$prefix/bin/"
		done
		write_pc icu-uc $icu_version "-I\${includedir}" "-L\${libdir} -licuuc -licudt"
		write_pc icu-i18n $icu_version "-I\${includedir}" "-L\${libdir} -licuin" icu-uc
		mark_built icu $icu_version
		return
	fi

	unpack icu "$icu_url" "$icu_sha256"
	local src=$work/src/icu/icu/source build=$work/build/icu log=$work/icu.log
	local opts=(--disable-samples --disable-tests --disable-extras --disable-icuio
		--disable-layoutex)
	: >"$log"
	if [ -n "$cross" ]; then
		# Building ICU's data runs ICU's tools, so a cross build needs a
		# build for the host to take them from
		rm -rf "$build-host"
		mkdir -p "$build-host"
		cd "$build-host"
		logged "$log" "$src/configure" "${opts[@]}"
		logged "$log" make -j"$jobs"
		cd - >/dev/null
		opts+=(--host=x86_64-apple-darwin --with-cross-build="$build-host")
	fi
	rm -rf "$build"
	mkdir -p "$build"
	cd "$build"
	local cc=cc cxx=c++
	[ -n "$cross" ] && cc="clang -arch $mac_arch" cxx="clang++ -arch $mac_arch"
	logged "$log" env CC="${CC:-$cc}" CXX="${CXX:-$cxx}" "$src/configure" \
		--prefix="$prefix" --libdir="$prefix/lib" --enable-shared --disable-static \
		"${opts[@]}"
	logged "$log" make -j"$jobs"
	logged "$log" make install
	cd - >/dev/null
	mark_built icu $icu_version
}

build_uuid() {
	case $os in
		darwin)
			# The C library has libuuid's functions, and the SDK uuid/uuid.h
			write_pc uuid 2.0 "" ""
			;;
		linux)
			built uuid $util_linux_version && return
			echo "building libuuid $util_linux_version"
			unpack util-linux "$util_linux_url" "$util_linux_sha256"
			local build=$work/build/util-linux log=$work/util-linux.log
			rm -rf "$build"
			mkdir -p "$build"
			: >"$log"
			cd "$build"
			logged "$log" "$work/src/util-linux/util-linux-$util_linux_version/configure" \
				--prefix="$prefix" --libdir="$prefix/lib" \
				--disable-all-programs --enable-libuuid --disable-static \
				--disable-nls --disable-bash-completion --without-python \
				--without-systemd --without-udev
			logged "$log" make -j"$jobs"
			logged "$log" make install
			cd - >/dev/null
			# Its uuid.pc is for #include <uuid.h>; Postgres has <uuid/uuid.h>,
			# which a system's /usr/include otherwise provides
			sed -i 's|^Cflags: .*|Cflags: -I${includedir} -I${includedir}/uuid|' \
				"$prefix/lib/pkgconfig/uuid.pc"
			mark_built uuid $util_linux_version
			;;
		windows)
			local build=$work/build/uuid
			rm -rf "$build"
			mkdir -p "$build" "$prefix/include/uuid"
			cd "$build"
			# Options with -, as Git Bash would take /c for a path
			cl -nologo -O2 -MD -c -I"$(native "$here/uuid-win32")" \
				"$(native "$here/uuid-win32/uuid.c")"
			lib -nologo -OUT:encore_uuid.lib uuid.obj
			cd - >/dev/null
			cp "$build/encore_uuid.lib" "$prefix/lib/"
			cp "$here/uuid-win32/uuid/uuid.h" "$prefix/include/uuid/"
			write_pc uuid 2.0 "-I\${includedir}" "-L\${libdir} -lencore_uuid -lrpcrt4"
			;;
	esac
}

build_openssl
build_libxml2
build_libxslt
build_lz4
build_zstd
build_icu
build_uuid

echo "built the libraries for $target in $prefix"
