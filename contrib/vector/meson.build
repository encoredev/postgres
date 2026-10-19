# pgvector 0.8.7, from https://github.com/pgvector/pgvector (its LICENSE is
# in this directory).  Encore's builds bundle it, as the Docker image Encore
# used before did.  src/, sql/, test/sql/, test/expected/, vector.control
# and LICENSE are pgvector's own, unchanged; to update, replace them with
# those of a newer release and update the lists below.
#
# pgvector builds with PGXS, which a cross build can't run; here it builds
# like the rest of contrib.

vector_version = '0.8.7'

vector_sources = files(
  'src/bitutils.c',
  'src/bitvec.c',
  'src/halfutils.c',
  'src/halfvec.c',
  'src/hnsw.c',
  'src/hnswbuild.c',
  'src/hnswinsert.c',
  'src/hnswscan.c',
  'src/hnswutils.c',
  'src/hnswvacuum.c',
  'src/ivfbuild.c',
  'src/ivfflat.c',
  'src/ivfinsert.c',
  'src/ivfkmeans.c',
  'src/ivfscan.c',
  'src/ivfutils.c',
  'src/ivfvacuum.c',
  'src/sparsevec.c',
  'src/vector.c',
)

if host_system == 'windows'
  vector_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'vector',
    '--FILEDESC', 'vector - vector data type and ivfflat and hnsw access methods',])
endif

# For auto-vectorization, as pgvector's Makefile and Makefile.win build it,
# but without -march=native
if cc.get_id() == 'msvc'
  vector_c_args = ['/fp:fast']
else
  vector_c_args = cc.get_supported_arguments(
    '-ftree-vectorize',
    '-fassociative-math',
    '-fno-signed-zeros',
    '-fno-trapping-math',
    '-ffp-contract=fast',
  )
endif

vector = shared_module('vector',
  vector_sources,
  c_args: vector_c_args,
  kwargs: contrib_mod_args,
)
contrib_targets += vector

install_data(
  'sql/vector.sql',
  rename: 'vector--@0@.sql'.format(vector_version),
  kwargs: contrib_data_args,
)

install_data(
  'sql/vector--0.1.0--0.1.1.sql',
  'sql/vector--0.1.1--0.1.3.sql',
  'sql/vector--0.1.3--0.1.4.sql',
  'sql/vector--0.1.4--0.1.5.sql',
  'sql/vector--0.1.5--0.1.6.sql',
  'sql/vector--0.1.6--0.1.7.sql',
  'sql/vector--0.1.7--0.1.8.sql',
  'sql/vector--0.1.8--0.2.0.sql',
  'sql/vector--0.2.0--0.2.1.sql',
  'sql/vector--0.2.1--0.2.2.sql',
  'sql/vector--0.2.2--0.2.3.sql',
  'sql/vector--0.2.3--0.2.4.sql',
  'sql/vector--0.2.4--0.2.5.sql',
  'sql/vector--0.2.5--0.2.6.sql',
  'sql/vector--0.2.6--0.2.7.sql',
  'sql/vector--0.2.7--0.3.0.sql',
  'sql/vector--0.3.0--0.3.1.sql',
  'sql/vector--0.3.1--0.3.2.sql',
  'sql/vector--0.3.2--0.4.0.sql',
  'sql/vector--0.4.0--0.4.1.sql',
  'sql/vector--0.4.1--0.4.2.sql',
  'sql/vector--0.4.2--0.4.3.sql',
  'sql/vector--0.4.3--0.4.4.sql',
  'sql/vector--0.4.4--0.5.0.sql',
  'sql/vector--0.5.0--0.5.1.sql',
  'sql/vector--0.5.1--0.6.0.sql',
  'sql/vector--0.6.0--0.6.1.sql',
  'sql/vector--0.6.1--0.6.2.sql',
  'sql/vector--0.6.2--0.7.0.sql',
  'sql/vector--0.7.0--0.7.1.sql',
  'sql/vector--0.7.1--0.7.2.sql',
  'sql/vector--0.7.2--0.7.3.sql',
  'sql/vector--0.7.3--0.7.4.sql',
  'sql/vector--0.7.4--0.8.0.sql',
  'sql/vector--0.8.0--0.8.1.sql',
  'sql/vector--0.8.1--0.8.2.sql',
  'sql/vector--0.8.2--0.8.3.sql',
  'sql/vector--0.8.3--0.8.4.sql',
  'sql/vector--0.8.4--0.8.5.sql',
  'sql/vector--0.8.5--0.8.6.sql',
  'sql/vector--0.8.6--0.8.7.sql',
  'vector.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'vector',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'inputdir': meson.current_source_dir() / 'test',
    'expecteddir': meson.current_source_dir() / 'test',
    'regress_args': ['--load-extension=vector'],
    'sql': [
      'bit',
      'btree',
      'cast',
      'copy',
      'halfvec',
      'hnsw_bit',
      'hnsw_halfvec',
      'hnsw_sparsevec',
      'hnsw_vector',
      'ivfflat_bit',
      'ivfflat_halfvec',
      'ivfflat_vector',
      'sparsevec',
      'vector_type',
    ],
  },
}
