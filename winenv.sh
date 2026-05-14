podman run -it --rm \
    --device=/dev/kvm \
    --cap-add NET_ADMIN \
    -p 8006:8006 -p 3389:3389/tcp -p 3389:3389/udp -p 2222:22 \
    -e VERSION="11" \
    -e RAM_SIZE="4G" \
    docker.io/dockur/windows:latest
