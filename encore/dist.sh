#!/usr/bin/env bash
# Build the Postgres that Encore bundles, for one platform, into a directory
# that runs from wherever it's moved to:
#
#   encore/dist.sh [--target <os>-<arch>] [--commit <sha>] [--build-dir <dir>]
#                  [--no-check] <output dir>
#
# The target is the host's by default; darwin-amd64 can also be built on an
# Apple Silicon Mac.  The output holds bin/, lib/ and share/, PostgreSQL's
# COPYRIGHT, and a VERSION file of key=value lines:
#
#   version          <PostgreSQL version>-<12 characters of the commit>
#   pg_version       the PostgreSQL version, as in meson.build
#   catalog_version  CATALOG_VERSION_NO; data directories of another catalog
#                    version don't start
#   commit           the commit built
#
# Unless given --no-check, a copy of the output in another directory
# initializes a page store cluster and clones a database in it; for
# darwin-amd64 on Apple Silicon, only where Rosetta runs it.
#
# Needs meson, ninja, bison, flex and perl; patchelf on Linux; on Windows,
# Git Bash with the MSVC environment set up (vcvarsall, or
# msvc-dev-cmd in GitHub Actions).
set -euo pipefail

usage() {
	sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//' >&2
	exit 2
}

src=$(cd "$(dirname "$0")/.." && pwd)
target=
commit=
build=
check=1
out=
while [ $# -gt 0 ]; do
	case $1 in
		--target) target=$2; shift 2 ;;
		--commit) commit=$2; shift 2 ;;
		--build-dir) build=$2; shift 2 ;;
		--no-check) check=; shift ;;
		-h|--help) usage ;;
		-*) echo "unknown option $1" >&2; usage ;;
		*) [ -z "$out" ] || usage; out=$1; shift ;;
	esac
done
[ -n "$out" ] || usage

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
cross=
if [ "$target" != "$host" ]; then
	# Only what an Apple Silicon Mac can build for Intel Macs
	if [ "$target" != darwin-amd64 ] || [ "$host" != darwin-arm64 ]; then
		echo "can't build $target on $host" >&2
		exit 1
	fi
	cross=1
fi

if [ -z "$commit" ]; then
	commit=$(git -C "$src" rev-parse HEAD)
fi
pg_version=$(sed -n "s/^  version: '\(.*\)',$/\1/p" "$src/meson.build" | head -n 1)
catalog_version=$(awk '/^#define CATALOG_VERSION_NO/ {print $3}' \
	"$src/src/include/catalog/catversion.h")
version=$pg_version-${commit:0:12}
[ -n "$pg_version" ] && [ -n "$catalog_version" ] || {
	echo "could not read the version from $src" >&2
	exit 1
}

if [ -e "$out" ] && [ -n "$(ls -A "$out")" ]; then
	echo "$out is not empty" >&2
	exit 1
fi
mkdir -p "$out"
out=$(cd "$out" && pwd)
build=${build:-${TMPDIR:-/tmp}/encore-postgres-build-$target}
mkdir -p "$build"
build=$(cd "$build" && pwd)

# Paths as the native tools take them
native() {
	if [ "$os" = windows ]; then cygpath -m "$1"; else echo "$1"; fi
}

# Installed here and copied to the output.  A prefix containing "pgsql"
# keeps the modules in lib/ and the shared files in share/, rather than in
# postgresql/ beneath them.
prefix=$build/install/pgsql
rm -rf "$build/install"

opts=(
	--buildtype=release
	--prefix="$(native "$prefix")"
	--libdir=lib
	-Dauto_features=disabled
	-Drpath=false
	-Dtap_tests=disabled
)
case $os in
	darwin)
		# The SDK's libedit and zlib, which every Mac has
		opts+=(-Dzlib=enabled -Dreadline=enabled -Dlibedit_preferred=true)
		export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}
		;;
	linux)
		# libz.so.1 is on every distribution, readline not
		opts+=(-Dzlib=enabled)
		;;
	windows)
		;;
esac
if [ -n "$cross" ]; then
	cat >"$build/cross.ini" <<-EOF
		[binaries]
		c = ['clang', '-arch', 'x86_64']
		ar = 'ar'
		strip = 'strip'

		[host_machine]
		system = 'darwin'
		kernel = 'xnu'
		cpu_family = 'x86_64'
		cpu = 'x86_64'
		endian = 'little'

		[properties]
		needs_exe_wrapper = true
	EOF
	opts+=(--cross-file "$build/cross.ini")
fi

# Git Bash's own link, perl and the like come first in its PATH; meson
# wants MSVC's link and a native perl.
meson_path=$PATH
if [ "$os" = windows ]; then
	meson_path=$(echo "$PATH" | tr ':' '\n' |
		awk '/^(\/[a-z]\/Program Files\/Git)?(\/usr)?\/bin$|\/mingw64\/bin$|^\/usr\/local\/bin$/ {tail = tail ":" $0; next}
			{head = head (head ? ":" : "") $0}
			END {print head tail}')
fi

if [ -f "$build/meson/build.ninja" ]; then
	PATH=$meson_path meson setup --reconfigure "${opts[@]}" "$(native "$build/meson")" "$(native "$src")"
else
	PATH=$meson_path meson setup "${opts[@]}" "$(native "$build/meson")" "$(native "$src")"
fi
PATH=$meson_path meson compile -C "$(native "$build/meson")"
PATH=$meson_path meson install -C "$(native "$build/meson")" --quiet

cp -R "$prefix/." "$out/"
cp "$src/COPYRIGHT" "$out/"

# What's for building against Postgres, and ECPG
rm -rf "$out/include" "$out/lib/pgxs" "$out/lib/pkgconfig" "$out/share/doc"
rm -f "$out"/bin/ecpg* "$out"/lib/libecpg* "$out"/lib/libpgtypes* \
	"$out"/bin/libecpg* "$out"/bin/libpgtypes*
# libpq's name for linking against it; the programs load libpq.5
rm -f "$out/lib/libpq.dylib" "$out/lib/libpq.so"
find "$out" \( -name '*.a' -o -name '*.lib' -o -name '*.pdb' \) -delete

# Strip, and make what refers to the install's libraries look for them
# relative to itself
case $os in
	darwin)
		relocate() {
			local f=$1 dir=$2 id

			strip -x "$f"
			otool -L "$f" | awk -v lib="$prefix/lib/" 'NR > 1 && index($1, lib) == 1 {print $1}' |
				while read -r dep; do
					install_name_tool -change "$dep" "$dir/$(basename "$dep")" "$f" 2>/dev/null
				done
			id=$(otool -D "$f" | awk 'NR > 1')
			case $id in
				"$prefix"/lib/*) install_name_tool -id "@rpath/$(basename "$id")" "$f" 2>/dev/null ;;
			esac
			# Changing a binary invalidates its signature, which arm64 requires
			codesign -s - -f "$f" 2>/dev/null
		}
		for f in "$out"/bin/*; do
			relocate "$f" @executable_path/../lib
		done
		for f in "$out"/lib/*.dylib; do
			[ -L "$f" ] || relocate "$f" @loader_path
		done
		left=$(find "$out/bin" "$out/lib" -type f -exec otool -L {} + | grep -F "$prefix/" || true)
		;;
	linux)
		for f in "$out"/bin/* "$out"/lib/*.so*; do
			[ -L "$f" ] && continue
			strip --strip-unneeded "$f"
			case $f in
				"$out"/bin/*) patchelf --set-rpath '$ORIGIN/../lib' "$f" ;;
				*) patchelf --set-rpath '$ORIGIN' "$f" ;;
			esac
		done
		left=$(for f in "$out"/bin/* "$out"/lib/*.so*; do
			[ -L "$f" ] || patchelf --print-rpath "$f"
		done | grep -F "$prefix" || true)
		;;
	windows)
		# DLLs sit beside the programs, and modules are loaded by path
		left=
		;;
esac
if [ -n "$left" ]; then
	echo "the output still loads libraries from $prefix:" >&2
	echo "$left" >&2
	exit 1
fi

cat >"$out/VERSION" <<EOF
version=$version
pg_version=$pg_version
catalog_version=$catalog_version
commit=$commit
EOF

if [ -n "$check" ] && [ -n "$cross" ] && ! "$out/bin/postgres" --version >/dev/null 2>&1; then
	echo "not checking the $target build, which this machine can't run"
	check=
fi
if [ -n "$check" ]; then
	# From another directory, to catch anything still found by its path
	tmp=$(mktemp -d /tmp/encore-postgres-check.XXXXXX)
	trap 'PATH=$tmp/pgsql/bin pg_ctl -D "$(native "$tmp/data")" -m immediate stop >/dev/null 2>&1 || true; rm -rf "$tmp"' EXIT
	cp -R "$out" "$tmp/pgsql"
	export PATH=$tmp/pgsql/bin:$PATH
	initdb --pagestore -U postgres -A trust -D "$(native "$tmp/data")" >"$tmp/initdb.log" ||
		{ cat "$tmp/initdb.log" >&2; exit 1; }
	if [ "$os" = windows ]; then
		conn=(-h 127.0.0.1 -p 54329)
		server_opts="-p 54329"
	else
		conn=(-h "$tmp")
		server_opts="-c listen_addresses='' -k $tmp"
	fi
	pg_ctl -D "$(native "$tmp/data")" -l "$(native "$tmp/server.log")" -o "$server_opts" -w start >/dev/null ||
		{ cat "$tmp/server.log" >&2; exit 1; }
	sql() { psql "${conn[@]}" -U postgres -X -A -t -v ON_ERROR_STOP=1 -d "$1" -c "$2" | tr -d '\r'; }
	sql postgres "CREATE DATABASE tmpl" >/dev/null
	sql tmpl "CREATE EXTENSION pg_trgm; CREATE TABLE t AS SELECT g AS a FROM generate_series(1, 10000) g" >/dev/null
	sql postgres "SELECT pg_pagestore_freeze('tmpl')" >/dev/null
	sql postgres "CREATE DATABASE c TEMPLATE tmpl STRATEGY = SNAPSHOT" >/dev/null
	got=$(sql c "SELECT count(*) FROM t WHERE similarity(a::text, '42') >= 0")
	[ "$got" = 10000 ] || { echo "the clone has $got rows, not 10000" >&2; exit 1; }
	pg_ctl -D "$(native "$tmp/data")" -m fast -w stop >/dev/null
	echo "checked: initdb --pagestore, a contrib module, freezing and STRATEGY = SNAPSHOT from $tmp/pgsql"
fi

echo "built $version for $target in $out"
