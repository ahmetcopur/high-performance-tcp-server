# The Linux environment plus perf, for step 10. Kept apart from the
# Dockerfile so the image that runs the tests stays small.
#
#   docker build -t tcpserver-dev .
#   docker build -t tcpserver-profile -f Dockerfile.profile .
#
# perf needs more rights than a container has by default, so run it with
# --privileged, and with --init so that finished processes are cleaned
# up. See benchmarks/README.md.
FROM tcpserver-dev

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        linux-tools-generic \
    && rm -rf /var/lib/apt/lists/* \
    # The perf wrapper looks for a build that matches the running kernel
    # and finds none in a container. Any recent perf works.
    && ln -sf "$(ls /usr/lib/linux-tools/*/perf | head -1)" /usr/local/bin/perf
