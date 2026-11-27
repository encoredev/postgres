# protoc-c's code for PostGIS

`vector_tile.pb-c.[ch]` and `geobuf.pb-c.[ch]` are what protobuf-c's `protoc-c`
generates from PostGIS's `postgis/vector_tile.proto` and `postgis/geobuf.proto`,
which `ST_AsMVT` and `ST_AsGeobuf` are built from. `encore/postgis.sh` puts them
in PostGIS's source instead of building `protoc-c`, which would take building
protobuf (C++, with Abseil) on every platform. The generated code is the same
C wherever it is built.

They were generated with protobuf-c 1.5.2 from PostGIS 3.7.0's `.proto` files.
Building protobuf-c 1.5.2's `protoc-c` against protobuf 34 or later fails
(`FieldDescriptor::label` is gone), so this uses Debian trixie's protobuf:

```sh
mkdir -p gen/in gen/out
tar -xzf postgis-3.7.0.tar.gz
cp postgis-3.7.0/postgis/vector_tile.proto postgis-3.7.0/postgis/geobuf.proto gen/in/
curl -fsSL -o gen/protobuf-c.tar.gz \
  https://github.com/protobuf-c/protobuf-c/releases/download/v1.5.2/protobuf-c-1.5.2.tar.gz
docker run --rm -v "$PWD/gen:/gen" debian:trixie bash -c '
  apt-get update -qq && apt-get install -y -qq build-essential pkg-config \
    libprotobuf-dev libprotoc-dev protobuf-compiler
  cd /tmp && tar -xzf /gen/protobuf-c.tar.gz && cd protobuf-c-1.5.2 &&
    ./configure --prefix=/opt/pc && make -j8 && make install
  cd /gen/in && for p in vector_tile geobuf; do /opt/pc/bin/protoc-c --c_out=/gen/out $p.proto; done'
cp gen/out/*.pb-c.[ch] encore/postgis/
```

Generate them again when PostGIS changes either `.proto` file. The library
they're compiled with, `libprotobuf-c`, may be any version from 1.3.0 on: the
headers check that, and that the compiler that generated them is one it
supports.
