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

# Crytek Sponza, with its materials and diffuse textures (the TGAs of the
# plants, chains and thorns carry their cutout masks in the alpha channel)
SPONZA=https://raw.githubusercontent.com/jimmiebergmann/Sponza/222338979d32f4f4818466291bdbc29f192b86ba
fetch sponza.obj "$SPONZA/sponza.obj"
fetch sponza.mtl "$SPONZA/sponza.mtl"
mkdir -p models/external/textures
for t in background chain_texture lion spnza_bricks_a_diff sponza_arch_diff \
         sponza_ceiling_a_diff sponza_column_a_diff sponza_column_b_diff \
         sponza_column_c_diff sponza_curtain_blue_diff sponza_curtain_diff \
         sponza_curtain_green_diff sponza_details_diff sponza_fabric_blue_diff \
         sponza_fabric_diff sponza_fabric_green_diff sponza_flagpole_diff \
         sponza_floor_a_diff sponza_roof_diff sponza_thorn_diff vase_dif \
         vase_hanging vase_plant vase_round; do
    fetch "textures/$t.tga" "$SPONZA/textures/$t.tga"
done

# Its tangent-space normal maps (*_ddn.tga), which sponza.mtl names with
# map_Disp: whatever the MTL lists, so the two can't drift apart
for t in $(awk '$1 == "map_Disp" || $1 == "map_Bump" || $1 == "map_bump" || $1 == "bump" || $1 == "norm" { print $NF }' \
               models/external/sponza.mtl | tr -d '\r' | sort -u); do
    fetch "$t" "$SPONZA/$t"
done
