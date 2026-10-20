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
#   <library>        the version of each library encore/deps.sh built, as in
#                    openssl=3.5.9
#
# The build has the extensions of contrib and pgvector (contrib/vector), and
# OpenSSL, libxml2, libxslt, lz4, zstd, ICU and libuuid, which
# encore/deps.sh builds and the output includes.  The extensions that
# Encore's Docker image made trusted, which users other than superusers may
# create, are trusted here too.
#
# Unless given --no-check, a copy of the output in another directory
# initializes a page store cluster, clones a database in it and creates every
# extension; for darwin-amd64 on Apple Silicon, only where Rosetta runs it.
#
# Needs meson, ninja, bison, flex, perl, pkg-config and what encore/deps.sh
# needs; patchelf on Linux; on Windows, pkgconf rather than another
# pkg-config, and Git Bash with the MSVC environment set up (vcvarsall, or
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

# The libraries Postgres links with beyond the system's, which it finds
# through their pkg-config files
deps=$build/deps
"$src/encore/deps.sh" --target "$target" --work-dir "$build/deps.work" "$deps"

opts=(
	--buildtype=release
	--prefix="$(native "$prefix")"
	--libdir=lib
	--pkg-config-path="$(native "$deps/lib/pkgconfig")"
	-Dauto_features=disabled
	-Drpath=false
	-Dtap_tests=disabled
	-Dssl=openssl
	-Dlibxml=enabled
	-Dlibxslt=enabled
	-Dlz4=enabled
	-Dzstd=enabled
	-Dicu=enabled
	-Duuid=e2fs
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
		pkg-config = 'pkg-config'

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
	# Strawberry Perl's pkg-config.bat comes first in PATH too; meson
	# rejects it and then looks no further
	pkgconf=$(PATH=$meson_path command -v pkgconf) ||
		{ echo "pkgconf is missing" >&2; exit 1; }
	export PKG_CONFIG=$(cygpath -m "$pkgconf")
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

# The libraries deps.sh built, each under the name it's loaded by.  libexslt
# and ICU's libraries for its tools and tests are built but not used.
unused() {
	case $1 in *exslt*|*icutu*|*icutest*|*icuio*) return 0 ;; esac
	return 1
}
case $os in
	darwin)
		for f in "$deps"/lib/*.dylib; do
			[ -L "$f" ] && continue
			name=$(basename "$(otool -D "$f" | awk 'NR == 2')")
			unused "$name" || cp "$f" "$out/lib/$name"
		done
		;;
	linux)
		for f in "$deps"/lib/*.so*; do
			[ -L "$f" ] && continue
			name=$(patchelf --print-soname "$f")
			unused "$name" || cp "$f" "$out/lib/$name"
		done
		;;
	windows)
		for f in "$deps"/bin/*.dll; do
			unused "$(basename "$f")" || cp "$f" "$out/bin/"
		done
		;;
esac

# Strip, and make what refers to the output's libraries look for them
# relative to itself.  Then check that nothing refers to a library that's
# neither in the output nor the system's.
case $os in
	darwin)
		relocate() {
			local f=$1 dir=$2 id

			strip -x "$f"
			otool -L "$f" | awk 'NR > 1 {print $1}' |
				while read -r dep; do
					case $dep in @executable_path/*|@loader_path/*|/usr/lib/*|/System/*) continue ;; esac
					[ -e "$out/lib/$(basename "$dep")" ] &&
						install_name_tool -change "$dep" "$dir/$(basename "$dep")" "$f" 2>/dev/null
				done
			id=$(otool -D "$f" | awk 'NR > 1')
			case $id in
				''|@rpath/*) ;;
				*) install_name_tool -id "@rpath/$(basename "$id")" "$f" 2>/dev/null ;;
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
		# A function, as bash 3.2 can't parse a case in $(...)
		unresolved() {
			local f id dep
			for f in "$out"/bin/* "$out"/lib/*.dylib; do
				[ -L "$f" ] && continue
				id=$(otool -D "$f" | awk 'NR > 1')
				otool -L "$f" | awk -v id="$id" 'NR > 1 && $1 != id {print $1}' |
					while read -r dep; do
						case $dep in
							/usr/lib/*|/System/*) ;;
							@executable_path/../lib/*|@loader_path/*)
								[ -e "$out/lib/${dep##*/}" ] || echo "$dep" ;;
							*) echo "$dep" ;;
						esac
					done
			done
		}
		left=$(unresolved)
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
		# What the manylinux policy lets a program take from the system,
		# which glibc 2.28 and later has
		system='^(libc|libm|libdl|libpthread|librt|libz|libstdc\+\+|libgcc_s|ld-linux-.*)\.so'
		left=$(for f in "$out"/bin/* "$out"/lib/*.so*; do
			[ -L "$f" ] && continue
			patchelf --print-rpath "$f" | grep -F "$prefix" || true
			patchelf --print-needed "$f" | while read -r lib; do
				[ -e "$out/lib/$lib" ] || echo "$lib" | grep -Ev "$system" || true
			done
		done)
		;;
	windows)
		# DLLs sit beside the programs, and modules are loaded by path
		left=
		;;
esac
if [ -n "$left" ]; then
	echo "the output loads libraries that aren't in it:" >&2
	echo "$left" | sort -u >&2
	exit 1
fi

# Trusted, as Encore's Docker image made them, so that a database's owner
# may create them without being a superuser
trusted=(amcheck autoinc bloom btree_gin btree_gist citext cube dblink dict_int
	dict_xsyn earthdistance fuzzystrmatch hstore insert_username intagg intarray
	isn lo ltree moddatetime pageinspect pg_buffercache pg_freespacemap pg_prewarm
	pg_stat_statements pg_trgm pg_visibility pgcrypto pgrowlocks pgstattuple
	plpgsql postgres_fdw refint seg sslinfo tablefunc tsm_system_rows
	tsm_system_time unaccent uuid-ossp vector)
for ext in "${trusted[@]}"; do
	control=$out/share/extension/$ext.control
	if [ ! -f "$control" ]; then
		echo "the build has no extension $ext" >&2
		exit 1
	fi
	grep -Eq '^[[:space:]]*trusted[[:space:]]*=[[:space:]]*true' "$control" ||
		echo 'trusted = true' >>"$control"
done

cat >"$out/VERSION" <<EOF
version=$version
pg_version=$pg_version
catalog_version=$catalog_version
commit=$commit
EOF
for f in "$deps"/.built/*; do
	echo "$(basename "$f")=$(awk '{print $1}' "$f")" >>"$out/VERSION"
done

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
	# The SQL on stdin, which Git Bash passes on unchanged
	sql_as() { echo "$3" | psql "${conn[@]}" -U "$1" -X -q -A -t -v ON_ERROR_STOP=1 -d "$2" -f - | tr -d '\r'; }
	sql() { sql_as postgres "$@"; }
	expect() {
		local got
		got=$(sql "$1" "$2")
		[ "$got" = "$3" ] || { echo "$2: got '$got', not '$3'" >&2; exit 1; }
	}
	sql postgres "CREATE DATABASE tmpl" >/dev/null
	sql tmpl "CREATE EXTENSION pg_trgm; CREATE TABLE t AS SELECT g AS a FROM generate_series(1, 10000) g" >/dev/null
	sql postgres "SELECT pg_pagestore_freeze('tmpl')" >/dev/null
	sql postgres "CREATE DATABASE c TEMPLATE tmpl STRATEGY = SNAPSHOT" >/dev/null
	got=$(sql c "SELECT count(*) FROM t WHERE similarity(a::text, '42') >= 0")
	[ "$got" = 10000 ] || { echo "the clone has $got rows, not 10000" >&2; exit 1; }

	# Every extension, which loads its module and what that links with: the
	# trusted ones as a database's owner who isn't a superuser, the others
	# as a superuser.  In UTF-8, for ICU, whatever the machine's locale is.
	sql postgres "CREATE ROLE owner LOGIN" >/dev/null
	sql postgres "CREATE DATABASE exts OWNER owner TEMPLATE template0 ENCODING 'UTF8' LOCALE 'C'" >/dev/null
	for ext in "${trusted[@]}"; do
		sql_as owner exts "CREATE EXTENSION IF NOT EXISTS \"$ext\" CASCADE" >/dev/null
	done
	for control in "$tmp"/pgsql/share/extension/*.control; do
		ext=$(basename "$control" .control)
		sql exts "CREATE EXTENSION IF NOT EXISTS \"$ext\" CASCADE" >/dev/null
	done
	# And each library deps.sh built
	expect exts "SELECT encode(digest('abc', 'sha256'), 'hex')" \
		ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
	expect exts "SELECT ssl_is_used()" f
	expect exts "SELECT substr(uuid_generate_v1()::text, 15, 1) || substr(uuid_generate_v4()::text, 15, 1)" 14
	expect exts "SELECT '[1,2,3]'::vector <-> '[1,2,5]'" 2
	expect exts "SELECT (xpath('/a/text()', '<a>x</a>'))[1]" x
	expect exts "SELECT xslt_process('<a>x</a>', '<xsl:stylesheet version=\"1.0\" xmlns:xsl=\"http://www.w3.org/1999/XSL/Transform\"><xsl:output method=\"text\"/><xsl:template match=\"/\">y<xsl:value-of select=\"a\"/></xsl:template></xsl:stylesheet>')" yx
	expect exts "SELECT 'a' < 'B' COLLATE \"und-x-icu\", 'a' < 'B' COLLATE \"C\"" "t|f"
	expect exts "CREATE TABLE z (t text COMPRESSION lz4); INSERT INTO z VALUES (repeat('x', 100000)); SELECT pg_column_compression(t) FROM z" lz4
	expect exts "SET wal_compression = zstd; SHOW wal_compression" zstd
	pg_ctl -D "$(native "$tmp/data")" -m fast -w stop >/dev/null
	echo "checked: initdb --pagestore, freezing and STRATEGY = SNAPSHOT, and every extension from $tmp/pgsql"
fi

echo "built $version for $target in $out"
