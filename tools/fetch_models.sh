#!/bin/sh
# Downloads the large test meshes used by scenes/bunny_cornell.scene and
# scenes/sponza.scene, the MagicaVoxel models of scenes/vox_*.scene and the
# glTF cars of scenes/car_*.scene,
# into models/external/ (ignored by git). The URLs are
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

# MagicaVoxel sample models by ephtracy (MIT license), for scenes/vox_*.scene
VOX=https://raw.githubusercontent.com/ephtracy/voxel-model/3bff8feaa7d94a6237f2c07dc4ca3c3d268ef06c/vox
fetch monu1.vox "$VOX/monument/monu1.vox"
fetch monu10.vox "$VOX/monument/monu10.vox"
fetch teapot.vox "$VOX/scan/teapot.vox"
fetch chr_knight.vox "$VOX/character/chr_knight.vox"

# glTF cars from the Khronos glTF sample assets, for scenes/car_*.scene:
# CarConcept (CC-BY 4.0, Eric Chadwick, Darmstadt Graphics Group, from a CC0
# model by Unity Fan) and ToyCar (CC0, Guido Odendahl and Eric Chadwick)
GLTF=https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models
fetch CarConcept.glb "$GLTF/CarConcept/glTF-Binary/CarConcept.glb"
fetch ToyCar.glb "$GLTF/ToyCar/glTF-Binary/ToyCar.glb"

# Sketchfab cars (CC-BY 4.0), for scenes/car_porsche930.scene and
# scenes/car_bmw_e30.scene. Sketchfab only serves downloads to a signed-in
# account, so these need your API token (free account, then
# https://sketchfab.com/settings/password, "API Token"):
#   SKETCHFAB_TOKEN=... tools/fetch_models.sh
# Each glTF archive is unpacked into models/external/<name>/ (scene.gltf,
# scene.bin, textures/, license.txt with the credit to give).
fetch_sketchfab() {
    if [ -f "models/external/$1/scene.gltf" ]; then
        echo "models/external/$1 already present"
        return
    fi
    if [ -z "$SKETCHFAB_TOKEN" ]; then
        echo "Skipping $1: set SKETCHFAB_TOKEN to download it from Sketchfab"
        return
    fi
    echo "Downloading $1 from Sketchfab..."
    # The API answers with short-lived links to each format; take the glTF zip
    url=$(curl -fsS --retry 3 -H "Authorization: Token $SKETCHFAB_TOKEN" \
              "https://api.sketchfab.com/v3/models/$2/download" \
          | python3 -c 'import json, sys; print(json.load(sys.stdin)["gltf"]["url"])')
    curl -fL --retry 3 -o "models/external/$1.zip" "$url"
    rm -rf "models/external/$1.part"
    python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
        "models/external/$1.zip" "models/external/$1.part"
    rm "models/external/$1.zip"
    mv "models/external/$1.part" "models/external/$1"
}

# "FREE 1975 Porsche 911 (930) Turbo" by Lionsharp Studios
# https://sketchfab.com/3d-models/free-1975-porsche-911-930-turbo-8568d9d14a994b9cae59499f0dbed21e
fetch_sketchfab porsche930 8568d9d14a994b9cae59499f0dbed21e
# "[FREE] BMW M3 E30" by Martin Trafas (TinoD2)
# https://sketchfab.com/3d-models/free-bmw-m3-e30-ac3c7013434e403e8faff87948caf422
fetch_sketchfab bmw_e30 ac3c7013434e403e8faff87948caf422
