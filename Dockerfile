# ngPost without a graphical interface, for a server, a NAS or a CI runner.
#
#   docker build -t ngpost .
#   docker run --rm -v "$PWD/config:/config" -v "$PWD/data:/data" ngpost --version
#
# The configuration lives at /config/ngPost/ngPost.conf -- ngPost reads it from
# $XDG_CONFIG_HOME, which this image points at /config. Copy ngPost.docker.conf
# there to start from something that works, and put your files under /data.
#
# What this image can and cannot do:
#   * 7-Zip and par2cmdline are installed. rar is non-free and cannot be
#     redistributed: mount your own binary and set RAR_TOOL = rar with
#     RAR_SOURCE = custom / RAR_PATH if you need it.
#   * The integrated VPN is not supported here. A tunnel needs NET_ADMIN,
#     /dev/net/tun and a volatile /run, and ngPost deliberately refuses to run
#     one when /run is persistent. Put the container behind the VPN instead.
#   * The build is the headless one (CONFIG+=no_hmi): Qt Core, Network, Sql,
#     DBus and qtkeychain only, no X11, so --auto and --monitor run with no
#     display of any kind.

ARG DEBIAN_VERSION=trixie-slim

FROM debian:${DEBIAN_VERSION} AS build

# One job by default: a fat -j on a small machine ends in the OOM killer, and
# the CI runner passes its own count.
ARG JOBS=1

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        build-essential \
        qt6-base-dev \
        qtkeychain-qt6-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /src
WORKDIR /build
RUN qmake6 CONFIG+=no_hmi /src/src/ngPost.pro \
    && make -j"${JOBS}" \
    && strip ngPost

FROM debian:${DEBIAN_VERSION}

# What registries show. "source" also links the ghcr.io package to its
# repository; release.yml adds the version, the revision and the date.
LABEL org.opencontainers.image.title="ngPost" \
      org.opencontainers.image.description="Usenet poster, headless build, with par2cmdline and 7-Zip" \
      org.opencontainers.image.source="https://github.com/Hydro74000/ngPost" \
      org.opencontainers.image.documentation="https://github.com/Hydro74000/ngPost/wiki/Docker"

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        libqt6core6t64 \
        libqt6network6 \
        libqt6sql6 \
        libqt6sql6-sqlite \
        libqt6dbus6 \
        libqt6keychain1 \
        par2 \
        7zip \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /build/ngPost /usr/local/bin/ngPost

# Qt needs a UTF-8 locale and says so on every run under the bare "C" of a slim
# image, before any output of ngPost. C.UTF-8 ships with glibc itself.
ENV LANG=C.UTF-8
ENV XDG_CONFIG_HOME=/config
# The image runs as 1000:1000. A new named or anonymous volume takes the owner
# of its mount point, so give these two to that user before declaring them.
# Bind mounts keep the host's owners: see the wiki's Docker page.
RUN mkdir -p /config /data && chown 1000:1000 /config /data
VOLUME ["/config", "/data"]
WORKDIR /data

USER 1000:1000

ENTRYPOINT ["ngPost"]
CMD ["--help"]
