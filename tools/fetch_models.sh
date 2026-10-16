#!/bin/sh
# Downloads the large test meshes used by scenes/bunny_cornell.scene and
# scenes/sponza.scene into models/external/ (ignored by git). The URLs are
# pinned to fixed commits so the files never change under the scenes.
set -e
cd "$(dirname "$0")/.."
mkdir -p models/external

fetch() {
    if [ -f "models/external/$1" ]; then
        echo "models/external/$1 already present"
    else
        echo "Downloading $1..."
        curl -fL --retry 3 -o "models/external/$1.part" "$2"
        mv "models/external/$1.part" "models/external/$1"
    fi
}

# Stanford bunny (Stanford 3D Scanning Repository), from Alec Jacobson's
# common-3d-test-models
fetch stanford-bunny.obj https://raw.githubusercontent.com/alecjacobson/common-3d-test-models/8a4f8642acaf43f9cd7b67858a1502e1055ef202/data/stanford-bunny.obj

# Crytek Sponza (geometry only)
fetch sponza.obj https://raw.githubusercontent.com/jimmiebergmann/Sponza/222338979d32f4f4818466291bdbc29f192b86ba/sponza.obj
