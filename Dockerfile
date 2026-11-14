# A Linux environment for building and testing the server from a Mac.
# epoll and eventfd exist only on Linux, so the epoll backend cannot be
# built or tested on macOS itself.
#
#   docker build -t tcpserver-dev .
#   docker run --rm -v "$PWD":/src:ro tcpserver-dev
#
# The source is mounted read-only and the build goes to /build inside the
# container, so nothing is written into the project folder.
FROM ubuntu:24.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        g++ \
        make \
        procps \
        python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Extra CMake options go in CMAKE_OPTIONS, for example
#   docker run --rm -e CMAKE_OPTIONS="-DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON" ...
ENV CMAKE_OPTIONS=""

CMD ["sh", "-c", "cmake -S /src -B /build -DWARNINGS_AS_ERRORS=ON $CMAKE_OPTIONS && cmake --build /build --parallel && ctest --test-dir /build --output-on-failure"]
