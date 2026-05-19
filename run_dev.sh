#!/bin/bash
# Lance la build de dev (branche levelup) depuis le distrobox my-distrobox.
# Les libs Qt6 requises se trouvent dans le conteneur, pas sur le host.
exec distrobox enter my-distrobox -- "$HOME/ngPost-dev/ngPost" "$@"
