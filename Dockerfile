# svn2git enhanced — multi-stage production image
#
# Builder stage compiles both binaries:
#   - svn-all-fast-export   classic Qt-based converter (qmake build)
#   - svn2git-validate      validation & EN 50128 compliance CLI (CMake build)
# and runs the full test suite as a build gate.
#
# Runtime stage is a slim image with only the runtime dependencies,
# both binaries, documentation and sample rules files.

# --- Builder ----------------------------------------------------------------
FROM ubuntu:24.04 AS builder

ENV LC_ALL=C.UTF-8 \
    DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install --yes --no-install-recommends \
    build-essential \
    cmake \
    libapr1-dev \
    libsvn-dev \
    qt5-qmake \
    qtbase5-dev \
    libspdlog-dev \
    nlohmann-json3-dev \
    libsqlite3-dev \
    catch2 \
    git \
    subversion \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

# Classic converter (qmake).
RUN qmake && make -j"$(nproc)"

# Validation tooling (CMake) with the test suite as a hard build gate.
RUN cmake -B cmake-build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    && cmake --build cmake-build --parallel \
    && ctest --test-dir cmake-build --output-on-failure

# --- Runtime ----------------------------------------------------------------
FROM ubuntu:24.04

ENV LC_ALL=C.UTF-8 \
    DEBIAN_FRONTEND=noninteractive

# Runtime dependencies only: svn/git for repository access, Qt5 core and
# svn libs for the classic converter, spdlog/sqlite for the validator.
RUN apt-get update && apt-get install --yes --no-install-recommends \
    git \
    subversion \
    libqt5core5a \
    libsvn1 \
    libapr1 \
    libspdlog1.12 \
    libsqlite3-0 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/svn-all-fast-export /usr/local/bin/svn-all-fast-export
COPY --from=builder /build/cmake-build/svn2git-validate /usr/local/bin/svn2git-validate
COPY --from=builder /build/README.md /build/LICENSE /usr/local/share/doc/svn2git/
COPY --from=builder /build/samples /usr/local/share/doc/svn2git/samples
COPY --from=builder /build/tests/fixtures/authors.txt \
    /build/tests/fixtures/test.rules \
    /usr/local/share/doc/svn2git/samples/

WORKDIR /workdir
CMD ["svn2git-validate", "--help"]
